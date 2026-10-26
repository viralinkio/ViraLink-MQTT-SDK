// OTA: package discovery, MQTT chunks, fw_url downloads, verification,
// activation, confirmation and rollback, against a fake flash and HTTP source.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Attributes.h"
#include "vl/api/Ota.h"

using namespace vl;
using vltest::ClientRig;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

class FakeFirmware : public IFirmwareWriter {
 public:
  std::string image;
  size_t beginSize = 0;
  int begins = 0, aborts = 0, finishes = 0, restarts = 0, confirms = 0, rollbacks = 0;
  bool open = false;
  bool pending = false;  // pendingConfirm()
  bool rolled = false;   // rolledBack()
  Result beginResult = Result::Ok;
  Result finishResult = Result::Ok;

  Result begin(size_t size) override {
    ++begins;
    beginSize = size;
    if (beginResult != Result::Ok) return beginResult;
    image.clear();
    open = true;
    return Result::Ok;
  }
  Result write(const uint8_t* p, size_t n) override {
    if (!open) return Result::NotReady;
    image.append(reinterpret_cast<const char*>(p), n);
    return Result::Ok;
  }
  Result finish() override {
    ++finishes;
    open = false;
    return finishResult;
  }
  void abort() override {
    if (open) ++aborts;
    open = false;
  }
  void restart() override { ++restarts; }
  bool pendingConfirm() override { return pending; }
  void confirm() override {
    ++confirms;
    pending = false;
  }
  void rollback() override { ++rollbacks; }
  bool rolledBack() override { return rolled; }
};

class FakeHttp : public IHttpSource {
 public:
  std::string body;
  bool ranges = true;      // honours Range requests
  bool sendLength = true;  // reports the file size
  size_t cutAt = 0;        // the connection breaks once, at this offset; 0 = never
  int failOpens = 0;       // opens refused before one succeeds
  std::vector<size_t> opens;
  std::string lastUrl;
  const ITlsProvider* lastTls = nullptr;
  size_t pos = 0;
  bool isOpen = false;

  Result open(const char* url, size_t offset, const ITlsProvider* tls, size_t& start, size_t& total) override {
    opens.push_back(offset);
    lastUrl = url;
    lastTls = tls;
    if (failOpens > 0) {
      --failOpens;
      return Result::Rejected;
    }
    start = ranges ? offset : 0;
    pos = start;
    total = sendLength ? body.size() : 0;
    isOpen = true;
    return Result::Ok;
  }
  int read(uint8_t* buf, size_t cap) override {
    if (!isOpen) return -1;
    if (cutAt && pos >= cutAt) {
      cutAt = 0;
      return -1;
    }
    size_t end = cutAt ? cutAt : body.size();
    size_t n = end - pos < cap ? end - pos : cap;
    memcpy(buf, body.data() + pos, n);
    pos += n;
    return static_cast<int>(n);
  }
  void close() override { isOpen = false; }
};

std::vector<std::string> states;
void onState(OtaState s, const char* error, void*) { states.push_back(std::string(toString(s)) + (error ? ":" : "") + (error ? error : "")); }

size_t progressDone = 0, progressTotal = 0;
void onProgress(size_t done, size_t total, void*) {
  progressDone = done;
  progressTotal = total;
}

const char* const kTargetRequest =
    "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm,fw_url\"}";

std::string makeImage(size_t n, int seed = 3) {
  std::string s(n, '\0');
  for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(i * 7 + seed);
  return s;
}

std::string digestOf(const char* algo, const std::string& data) {
  HashAlgo a;
  TEST_ASSERT_TRUE(parseHashAlgo(algo, a));
  Hasher h;
  h.begin(a);
  h.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  char out[Hasher::kHexSize];
  h.hex(out);
  return out;
}

// A package as ThingsBoard publishes it. checksum null = the image's own.
std::string package(const char* version, const std::string& image, const char* algo = "SHA256",
                    const char* checksum = nullptr, const char* url = nullptr) {
  std::string sum = checksum ? checksum : digestOf(algo, image);
  std::string s = std::string("{\"fw_title\":\"ViraGate\",\"fw_version\":\"") + version + "\",\"fw_tag\":\"ViraGate " +
                  version + "\",\"fw_size\":" + std::to_string(image.size()) + ",\"fw_checksum_algorithm\":\"" +
                  algo + "\",\"fw_checksum\":\"" + sum + "\"";
  if (url) s += std::string(",\"fw_url\":\"") + url + "\"";
  return s + "}";
}

bool isChunkRequest(const Published& p) { return p.topic.compare(0, 14, "v2/fw/request/") == 0; }

std::vector<std::string> fwStates(const std::vector<Published>& pubs) {
  std::vector<std::string> out;
  for (size_t i = 0; i < pubs.size(); ++i) {
    if (pubs[i].topic == "v1/devices/me/telemetry" && pubs[i].payload.find("fw_state") != std::string::npos) {
      out.push_back(pubs[i].payload);
    }
  }
  return out;
}

int count(const std::vector<Published>& pubs, const char* topicPrefix) {
  int n = 0;
  for (size_t i = 0; i < pubs.size(); ++i) n += pubs[i].topic.compare(0, strlen(topicPrefix), topicPrefix) == 0;
  return n;
}

// The id of OTA's package request in pubs.
uint32_t targetRequest(const std::vector<Published>& pubs) {
  const char* prefix = "v1/devices/me/attributes/request/";
  for (size_t i = 0; i < pubs.size(); ++i) {
    if (pubs[i].topic.compare(0, strlen(prefix), prefix) != 0) continue;
    TEST_ASSERT_EQUAL_STRING(kTargetRequest, pubs[i].payload.c_str());
    return static_cast<uint32_t>(strtoul(pubs[i].topic.c_str() + strlen(prefix), nullptr, 10));
  }
  TEST_FAIL_MESSAGE("no package request");
  return 0;
}

struct OtaRig : ClientRig {
  FakeFirmware fw;
  FakeHttp http;
  Ota ota;
  std::vector<Published> log;  // everything published, oldest first

  explicit OtaRig(bool useWriter = true) {
    states.clear();
    progressDone = progressTotal = 0;
    if (useWriter) ota.setWriter(fw);
    ota.setHttp(http);
    TEST_ASSERT_EQUAL(Result::Ok, ota.begin("ViraGate", "1.0.0"));
    ota.onState(onState, nullptr);
    ota.onProgress(onProgress, nullptr);
    c.use(ota);
  }
  std::vector<Published> step() {
    std::vector<Published> p = flush();
    log.insert(log.end(), p.begin(), p.end());
    return p;
  }
  // Goes Online; returns the id of the package request.
  uint32_t online() {
    connect();
    return targetRequest(step());
  }
  void reconnect() {
    t.close();
    clock.advance(2000);
    c.loop();
    t.connack(0);
    t.subackAll();
    TEST_ASSERT_EQUAL(State::Online, c.state());
  }
  void assign(uint32_t req, const std::string& pkg) {
    t.message(("v1/devices/me/attributes/response/" + std::to_string(req)).c_str(), "{\"shared\":" + pkg + "}");
  }
  void update(const std::string& attrs) { t.message("v1/devices/me/attributes", attrs); }
  void chunk(uint32_t req, uint32_t n, const std::string& data) {
    t.message(("v2/fw/response/" + std::to_string(req) + "/chunk/" + std::to_string(n)).c_str(), data);
  }
  // Answers chunk requests from image until none is left; returns how many.
  int serve(const std::string& image) {
    int served = 0;
    for (int round = 0; round < 100; ++round) {
      std::vector<Published> p = step();
      bool any = false;
      for (size_t i = 0; i < p.size(); ++i) {
        if (!isChunkRequest(p[i])) continue;
        unsigned req = 0, n = 0;
        TEST_ASSERT_EQUAL(2, sscanf(p[i].topic.c_str(), "v2/fw/request/%u/chunk/%u", &req, &n));
        size_t size = strtoul(p[i].payload.c_str(), nullptr, 10);
        size_t off = n * size;
        chunk(req, n, off < image.size() ? image.substr(off, size) : std::string());
        any = true;
        ++served;
      }
      if (!any) break;
    }
    return served;
  }
  // Runs the loop until state s is reached (HTTP downloads progress per tick).
  void runUntil(const char* s) {
    for (int i = 0; i < 100 && (states.empty() || states.back().compare(0, strlen(s), s) != 0); ++i) {
      clock.advance(VL_OTA_RETRY_DELAY_MS);
      step();
    }
  }
};

void expectStates(const std::vector<std::string>& want) {
  TEST_ASSERT_EQUAL_UINT(want.size(), states.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), states[i].c_str());
}

}  // namespace

static void test_reports_firmware_and_requests_package() {
  OtaRig r;
  r.c.begin(r.cfg);
  r.c.loop();
  r.t.connack(0);
  bool chunks = false;
  for (size_t i = 0; i < r.t.subscribed.size(); ++i) chunks |= r.t.subscribed[i] == "v2/fw/response/+/chunk/+";
  TEST_ASSERT_TRUE(chunks);
  r.t.subackAll();
  std::vector<Published> p = r.step();
  targetRequest(p);
  bool info = false;
  for (size_t i = 0; i < p.size(); ++i) {
    info |= p[i].topic == "v1/devices/me/telemetry" &&
            p[i].payload == "{\"current_fw_title\":\"ViraGate\",\"current_fw_version\":\"1.0.0\"}";
  }
  TEST_ASSERT_TRUE(info);

  // After a reconnect the package is read again; the firmware is reported once per boot.
  r.reconnect();
  p = r.step();
  targetRequest(p);
  TEST_ASSERT_EQUAL(0, count(p, "v1/devices/me/telemetry"));
}

static void test_running_package_reports_updated_once() {
  OtaRig r;
  r.assign(r.online(), package("1.0.0", makeImage(100)));
  std::vector<Published> p = r.step();
  std::vector<std::string> fw = fwStates(p);
  TEST_ASSERT_EQUAL_UINT(1, fw.size());
  TEST_ASSERT_EQUAL_STRING("{\"fw_state\":\"UPDATED\"}", fw[0].c_str());
  TEST_ASSERT_EQUAL(0, count(p, "v2/fw/request/"));
  TEST_ASSERT_EQUAL(0, r.fw.begins);

  r.reconnect();
  r.assign(targetRequest(r.step()), package("1.0.0", makeImage(100)));
  TEST_ASSERT_EQUAL_UINT(0, fwStates(r.step()).size());
  expectStates({"UPDATED"});
  TEST_ASSERT_TRUE(r.ota.state() == OtaState::Updated);
}

static void test_mqtt_download_verifies_and_restarts() {
  OtaRig r;
  std::string img = makeImage(2500);
  r.assign(r.online(), package("2.0.0", img));
  TEST_ASSERT_EQUAL(3, r.serve(img));  // 960 + 960 + 580 bytes: 1024-byte packets less 64 of overhead
  expectStates({"DOWNLOADING", "DOWNLOADED", "VERIFIED", "UPDATING"});
  std::vector<std::string> fw = fwStates(r.log);
  TEST_ASSERT_EQUAL_UINT(4, fw.size());
  TEST_ASSERT_EQUAL_STRING("{\"fw_state\":\"DOWNLOADING\"}", fw[0].c_str());
  TEST_ASSERT_EQUAL_STRING("{\"fw_state\":\"UPDATING\"}", fw[3].c_str());
  for (size_t i = 0; i < r.log.size(); ++i) {
    if (isChunkRequest(r.log[i])) TEST_ASSERT_EQUAL_STRING("960", r.log[i].payload.c_str());
  }
  TEST_ASSERT_TRUE(r.fw.image == img);
  TEST_ASSERT_EQUAL_UINT(2500, r.fw.beginSize);
  TEST_ASSERT_EQUAL(1, r.fw.finishes);
  TEST_ASSERT_EQUAL_UINT(2500, progressDone);
  TEST_ASSERT_EQUAL_UINT(2500, progressTotal);

  // UPDATING was delivered, so the device restarts on the next loop, once.
  r.c.loop();
  r.c.loop();
  TEST_ASSERT_EQUAL(1, r.fw.restarts);
  // A package assigned meanwhile is ignored: the new image boots first.
  r.update(package("3.0.0", img));
  r.step();
  TEST_ASSERT_EQUAL(1, r.fw.begins);
}

static void test_restart_waits_at_most_the_delay() {
  OtaRig r;
  std::string img = makeImage(500);
  r.assign(r.online(), package("2.0.0", img));
  std::vector<Published> p = r.step();
  unsigned req = 0, n = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    if (isChunkRequest(p[i])) sscanf(p[i].topic.c_str(), "v2/fw/request/%u/chunk/%u", &req, &n);
  }
  r.t.failPublish = true;  // UPDATING stays queued
  r.chunk(req, 0, img);
  TEST_ASSERT_EQUAL_STRING("UPDATING", states.back().c_str());
  r.clock.advance(VL_OTA_RESTART_DELAY_MS - 1);
  r.c.loop();
  TEST_ASSERT_EQUAL(0, r.fw.restarts);
  r.clock.advance(1);
  r.c.loop();
  TEST_ASSERT_EQUAL(1, r.fw.restarts);
}

static void test_every_checksum_algorithm() {
  const char* algos[] = {"MD5", "SHA256", "SHA384", "SHA512", "CRC32", "MURMUR3_32", "MURMUR3_128"};
  std::string img = makeImage(700);
  for (size_t i = 0; i < sizeof(algos) / sizeof(algos[0]); ++i) {
    OtaRig r;
    r.assign(r.online(), package("2.0.0", img, algos[i]));
    r.serve(img);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("UPDATING", states.back().c_str(), algos[i]);
  }
  // A checksum typed in upper case matches too.
  OtaRig r;
  std::string upper = digestOf("SHA256", img);
  for (size_t i = 0; i < upper.size(); ++i) upper[i] = static_cast<char>(toupper(upper[i]));
  r.assign(r.online(), package("2.0.0", img, "SHA256", upper.c_str()));
  r.serve(img);
  TEST_ASSERT_EQUAL_STRING("UPDATING", states.back().c_str());
}

static void test_checksum_mismatch_fails_until_reassigned() {
  OtaRig r;
  std::string img = makeImage(2500);
  std::string bad = package("2.0.0", img, "SHA256", digestOf("SHA256", "other").c_str());
  r.assign(r.online(), bad);
  r.serve(img);
  expectStates({"DOWNLOADING", "DOWNLOADED", "FAILED:checksum mismatch"});
  std::vector<std::string> fw = fwStates(r.log);
  TEST_ASSERT_EQUAL_STRING("{\"fw_state\":\"FAILED\",\"fw_error\":\"checksum mismatch\"}", fw.back().c_str());
  TEST_ASSERT_EQUAL(0, r.fw.finishes);
  TEST_ASSERT_EQUAL(1, r.fw.aborts);
  TEST_ASSERT_TRUE(r.ota.state() == OtaState::Failed);

  // The same package again after a reconnect: not retried.
  r.reconnect();
  r.assign(targetRequest(r.step()), bad);
  TEST_ASSERT_EQUAL(0, count(r.step(), "v2/fw/request/"));
  TEST_ASSERT_EQUAL_UINT(3, states.size());

  // Unassigned, then assigned again: tried once more.
  r.update("{\"deleted\":[\"fw_title\",\"fw_version\",\"fw_tag\",\"fw_size\",\"fw_checksum\",\"fw_checksum_algorithm\"]}");
  r.update(bad);
  TEST_ASSERT_EQUAL_STRING("DOWNLOADING", states.back().c_str());
  TEST_ASSERT_EQUAL(2, r.fw.begins);
}

static void test_chunk_timeout_retries_then_fails() {
  OtaRig r;
  r.assign(r.online(), package("2.0.0", makeImage(2500)));
  std::vector<Published> p = r.step();
  TEST_ASSERT_EQUAL(1, count(p, "v2/fw/request/"));
  std::string last;
  for (int i = 0; i < VL_OTA_RETRIES; ++i) {
    r.clock.advance(VL_OTA_CHUNK_TIMEOUT_MS);
    p = r.step();
    TEST_ASSERT_EQUAL(1, count(p, "v2/fw/request/"));
    for (size_t j = 0; j < p.size(); ++j) {
      if (!isChunkRequest(p[j])) continue;
      TEST_ASSERT_TRUE(p[j].topic.find("/chunk/0") != std::string::npos);
      TEST_ASSERT_TRUE(p[j].topic != last);  // a new request id each time
      last = p[j].topic;
    }
  }
  r.clock.advance(VL_OTA_CHUNK_TIMEOUT_MS);
  r.step();
  TEST_ASSERT_EQUAL_STRING("FAILED:chunk timeout", states.back().c_str());
  TEST_ASSERT_EQUAL(1, r.fw.aborts);
}

static void test_reconnect_resumes_with_new_request_id() {
  OtaRig r;
  std::string img = makeImage(2500);
  r.assign(r.online(), package("2.0.0", img));
  std::vector<Published> p = r.step();
  unsigned req0 = 0, n = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    if (isChunkRequest(p[i])) sscanf(p[i].topic.c_str(), "v2/fw/request/%u/chunk/%u", &req0, &n);
  }
  r.chunk(req0, 0, img.substr(0, 960));
  r.step();  // asks for chunk 1, unanswered

  r.reconnect();
  p = r.step();
  targetRequest(p);
  unsigned req1 = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    if (isChunkRequest(p[i])) TEST_ASSERT_EQUAL(2, sscanf(p[i].topic.c_str(), "v2/fw/request/%u/chunk/%u", &req1, &n));
  }
  TEST_ASSERT_EQUAL_UINT(1, n);
  TEST_ASSERT_TRUE(req1 > req0 + 1);
  r.chunk(req0, 0, img.substr(0, 960));  // a late duplicate is dropped
  TEST_ASSERT_EQUAL_UINT(960, r.fw.image.size());
  r.chunk(req1, 1, img.substr(960, 960));
  r.serve(img);
  TEST_ASSERT_TRUE(r.fw.image == img);
  TEST_ASSERT_EQUAL_STRING("UPDATING", states.back().c_str());
}

static void test_invalid_packages_fail() {
  std::string img = makeImage(2500);
  {
    OtaRig r;
    r.assign(r.online(), package("2.0.0", img, "SHA1", "00"));
    TEST_ASSERT_EQUAL_STRING("FAILED:unsupported checksum algorithm", states.back().c_str());
    TEST_ASSERT_EQUAL(0, r.fw.begins);
  }
  {
    OtaRig r;
    r.assign(r.online(), "{\"fw_title\":\"ViraGate\",\"fw_version\":\"2.0.0\",\"fw_size\":2500}");
    TEST_ASSERT_EQUAL_STRING("FAILED:no checksum", states.back().c_str());
  }
  {
    OtaRig r;
    std::string sum = digestOf("MD5", img);
    r.assign(r.online(), "{\"fw_title\":\"ViraGate\",\"fw_version\":\"2.0.0\",\"fw_checksum_algorithm\":\"MD5\","
                         "\"fw_checksum\":\"" + sum + "\"}");
    TEST_ASSERT_EQUAL_STRING("FAILED:no fw_size", states.back().c_str());
  }
  {
    OtaRig r;
    r.fw.beginResult = Result::TooLarge;
    r.assign(r.online(), package("2.0.0", img));
    TEST_ASSERT_EQUAL_STRING("FAILED:image too large", states.back().c_str());
  }
  {
    OtaRig r;
    r.assign(r.online(), package("2.0.0", img));
    std::vector<Published> p = r.step();
    unsigned req = 0, n = 0;
    for (size_t i = 0; i < p.size(); ++i) {
      if (isChunkRequest(p[i])) sscanf(p[i].topic.c_str(), "v2/fw/request/%u/chunk/%u", &req, &n);
    }
    r.chunk(req, 0, img.substr(0, 100));
    TEST_ASSERT_EQUAL_STRING("FAILED:short chunk", states.back().c_str());
    TEST_ASSERT_EQUAL(1, r.fw.aborts);
  }
  {
    OtaRig r(false);  // no writer on the host platform
    r.assign(r.online(), package("2.0.0", img));
    TEST_ASSERT_EQUAL_STRING("FAILED:no firmware writer", states.back().c_str());
  }
}

static void test_new_package_replaces_download() {
  OtaRig r;
  std::string a = makeImage(2500), b = makeImage(1500, 9);
  r.assign(r.online(), package("2.0.0", a));
  std::vector<Published> p = r.step();
  unsigned req = 0, n = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    if (isChunkRequest(p[i])) sscanf(p[i].topic.c_str(), "v2/fw/request/%u/chunk/%u", &req, &n);
  }
  r.chunk(req, 0, a.substr(0, 960));
  r.update(package("2.0.1", b));
  TEST_ASSERT_EQUAL(1, r.fw.aborts);
  TEST_ASSERT_EQUAL(2, r.fw.begins);
  r.chunk(req + 1, 1, a.substr(960, 960));  // answers the old download: dropped
  r.serve(b);
  TEST_ASSERT_TRUE(r.fw.image == b);
  expectStates({"DOWNLOADING", "DOWNLOADING", "DOWNLOADED", "VERIFIED", "UPDATING"});
}

static void test_new_image_confirmed_once_online() {
  OtaRig r;
  r.fw.pending = true;
  r.c.begin(r.cfg);
  r.c.loop();
  TEST_ASSERT_EQUAL(0, r.fw.confirms);
  r.t.connack(0);
  r.t.subackAll();
  TEST_ASSERT_EQUAL(1, r.fw.confirms);
  r.clock.advance(VL_OTA_CONFIRM_TIMEOUT_MS);
  r.c.loop();
  TEST_ASSERT_EQUAL(0, r.fw.rollbacks);
}

static void test_image_that_never_comes_online_rolls_back() {
  OtaRig r;
  r.fw.pending = true;
  r.c.begin(r.cfg);
  r.c.loop();
  r.clock.advance(VL_OTA_CONFIRM_TIMEOUT_MS - 1);
  r.c.loop();
  TEST_ASSERT_EQUAL(0, r.fw.rollbacks);
  r.clock.advance(1);
  r.c.loop();
  r.c.loop();
  TEST_ASSERT_EQUAL(1, r.fw.rollbacks);
  TEST_ASSERT_EQUAL(0, r.fw.confirms);
}

static void test_rolled_back_package_is_not_retried() {
  OtaRig r;
  r.fw.rolled = true;
  std::string img = makeImage(700);
  r.assign(r.online(), package("2.0.0", img));
  expectStates({"FAILED:rolled back"});
  TEST_ASSERT_EQUAL(0, r.fw.begins);
  // Another package is installed as usual.
  r.update(package("2.0.1", img));
  r.serve(img);
  TEST_ASSERT_EQUAL_STRING("UPDATING", states.back().c_str());
}

static void test_url_download_resumes_with_range() {
  OtaRig r;
  PemTls pem("CA");
  r.ota.setHttpTls(&pem);
  std::string img = makeImage(2500);
  r.http.body = img;
  r.http.cutAt = 1000;
  r.assign(r.online(), package("2.0.0", img, "SHA256", nullptr, "https://files.example.com/fw.bin"));
  r.runUntil("UPDATING");
  expectStates({"DOWNLOADING", "DOWNLOADED", "VERIFIED", "UPDATING"});
  TEST_ASSERT_EQUAL_UINT(2, r.http.opens.size());
  TEST_ASSERT_EQUAL_UINT(0, r.http.opens[0]);
  TEST_ASSERT_EQUAL_UINT(1000, r.http.opens[1]);
  TEST_ASSERT_EQUAL_STRING("https://files.example.com/fw.bin", r.http.lastUrl.c_str());
  TEST_ASSERT_TRUE(r.http.lastTls == &pem);
  TEST_ASSERT_FALSE(r.http.isOpen);
  TEST_ASSERT_TRUE(r.fw.image == img);
  TEST_ASSERT_EQUAL(1, r.fw.begins);
  TEST_ASSERT_EQUAL(0, count(r.log, "v2/fw/request/"));
}

static void test_url_download_restarts_without_range_support() {
  OtaRig r;
  std::string img = makeImage(2500);
  r.http.body = img;
  r.http.cutAt = 1000;
  r.http.ranges = false;
  r.assign(r.online(), package("2.0.0", img, "SHA256", nullptr, "http://files.example.com/fw.bin"));
  r.runUntil("UPDATING");
  TEST_ASSERT_EQUAL_STRING("UPDATING", states.back().c_str());
  TEST_ASSERT_EQUAL(2, r.fw.begins);
  TEST_ASSERT_EQUAL(1, r.fw.aborts);
  TEST_ASSERT_TRUE(r.fw.image == img);
}

static void test_url_download_unknown_size() {
  OtaRig r;
  std::string img = makeImage(2500);
  r.http.body = img;
  r.http.sendLength = false;
  std::string sum = digestOf("CRC32", img);
  r.assign(r.online(), "{\"fw_title\":\"ViraGate\",\"fw_version\":\"2.0.0\",\"fw_checksum_algorithm\":\"CRC32\","
                       "\"fw_checksum\":\"" + sum + "\",\"fw_url\":\"https://files.example.com/fw.bin\"}");
  r.runUntil("UPDATING");
  TEST_ASSERT_EQUAL_STRING("UPDATING", states.back().c_str());
  TEST_ASSERT_EQUAL_UINT(0, r.fw.beginSize);
  TEST_ASSERT_TRUE(r.fw.image == img);
  TEST_ASSERT_EQUAL_UINT(0, progressTotal);
}

static void test_url_download_gives_up() {
  OtaRig r;
  r.http.failOpens = 100;
  r.assign(r.online(), package("2.0.0", makeImage(2500), "SHA256", nullptr, "https://files.example.com/fw.bin"));
  r.runUntil("FAILED");
  TEST_ASSERT_EQUAL_STRING("FAILED:download failed", states.back().c_str());
  TEST_ASSERT_EQUAL_UINT(VL_OTA_RETRIES + 1, r.http.opens.size());
  TEST_ASSERT_EQUAL(1, r.fw.aborts);
}

std::vector<std::string> sharedKeys;
void onShared(const char* key, JsonValue, void*) { sharedKeys.push_back(key); }

// Shared attributes and OTA attached together: both see the fw_* update, and
// OTA's package response is resolved whichever module receives it first.
static void test_shares_attribute_topics_with_shared_attributes() {
  ClientRig rig;
  SharedAttributes<1> shared;
  FakeFirmware fw;
  Ota ota;
  states.clear();
  sharedKeys.clear();
  ota.setWriter(fw);
  ota.begin("ViraGate", "1.0.0");
  ota.onState(onState, nullptr);
  shared.onUpdate(nullptr, onShared);
  rig.c.use(shared, ota);
  rig.connect();
  uint32_t req = targetRequest(rig.flush());
  rig.t.message(("v1/devices/me/attributes/response/" + std::to_string(req)).c_str(),
                "{\"shared\":" + package("1.0.0", makeImage(10)) + "}");
  expectStates({"UPDATED"});
  rig.t.message("v1/devices/me/attributes", package("2.0.0", makeImage(10)));
  TEST_ASSERT_EQUAL_STRING("DOWNLOADING", states.back().c_str());
  TEST_ASSERT_TRUE(!sharedKeys.empty() && sharedKeys[0] == "fw_title");
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_reports_firmware_and_requests_package);
  RUN_TEST(test_running_package_reports_updated_once);
  RUN_TEST(test_mqtt_download_verifies_and_restarts);
  RUN_TEST(test_restart_waits_at_most_the_delay);
  RUN_TEST(test_every_checksum_algorithm);
  RUN_TEST(test_checksum_mismatch_fails_until_reassigned);
  RUN_TEST(test_chunk_timeout_retries_then_fails);
  RUN_TEST(test_reconnect_resumes_with_new_request_id);
  RUN_TEST(test_invalid_packages_fail);
  RUN_TEST(test_new_package_replaces_download);
  RUN_TEST(test_new_image_confirmed_once_online);
  RUN_TEST(test_image_that_never_comes_online_rolls_back);
  RUN_TEST(test_rolled_back_package_is_not_retried);
  RUN_TEST(test_url_download_resumes_with_range);
  RUN_TEST(test_url_download_restarts_without_range_support);
  RUN_TEST(test_url_download_unknown_size);
  RUN_TEST(test_url_download_gives_up);
  RUN_TEST(test_shares_attribute_topics_with_shared_attributes);
  return UNITY_END();
}

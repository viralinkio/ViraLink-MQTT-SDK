#include "vl/api/Ota.h"

#if VL_ENABLE_OTA

#include <stdio.h>
#include <string.h>

#include "vl/Client.h"
#include "vl/Log.h"
#include "vl/core/Codec.h"

namespace vl {

namespace {

const char kTargetKeys[] = "fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm,fw_url";
// Topic and MQTT header of a chunk response, v2/fw/response/<id>/chunk/<n>.
const size_t kChunkOverhead = 64;
// ThingsBoard refuses a chunk larger than NETTY_MAX_PAYLOAD_SIZE.
const uint32_t kServerMaxPayload = 65536ul;

// Absent stays "", which is fine; present but not a string or too long fails.
bool copyAttr(JsonValue v, char* out, size_t cap) {
  out[0] = '\0';
  return !v.valid() || v.copyString(out, cap);
}

// Checksums typed into the ThingsBoard UI may be upper case.
bool equalsHex(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a >= 'A' && *a <= 'F' ? static_cast<char>(*a - 'A' + 'a') : *a;
    char y = *b >= 'A' && *b <= 'F' ? static_cast<char>(*b - 'A' + 'a') : *b;
    if (x != y) return false;
    if (!x) return true;
  }
}

}  // namespace

const char* toString(OtaState s) {
  switch (s) {
    case OtaState::Idle:
      return "IDLE";
    case OtaState::Downloading:
      return "DOWNLOADING";
    case OtaState::Downloaded:
      return "DOWNLOADED";
    case OtaState::Verified:
      return "VERIFIED";
    case OtaState::Updating:
      return "UPDATING";
    case OtaState::Updated:
      return "UPDATED";
    case OtaState::Failed:
      return "FAILED";
  }
  return "";
}

Ota::Ota() : writer_(platformFirmwareWriter()), http_(platformHttpSource()), target_() {}

Result Ota::begin(const char* title, const char* version) {
  if (!title || !title[0] || !version || !version[0]) return Result::Rejected;
  size_t t = strlen(title), v = strlen(version);
  if (t >= sizeof(title_) || v >= sizeof(version_)) return Result::TooLarge;
  memcpy(title_, title, t + 1);
  memcpy(version_, version, v + 1);
  begun_ = true;
  return Result::Ok;
}

void Ota::topics(TopicSink& sink) {
  sink.add(InTopic::AttrUpdate);
  sink.add(InTopic::AttrResponse);
  sink.add(InTopic::FwChunk);
}

bool Ota::handle(const Topic& topic, Bytes payload) {
  if (!client_) return false;
  switch (topic.kind) {
    case InTopic::AttrResponse:
      return client_->resolvePending(topic.id, payload.data(), payload.size());
    case InTopic::AttrUpdate:
      return begun_ && applyAttributes(JsonValue::parse(payload), false);
    case InTopic::FwChunk:
      onChunk(topic, payload);
      return true;
    default:
      return false;
  }
}

void Ota::bootCheck(uint64_t nowMs) {
  if (booted_ || !writer_) return;
  booted_ = true;
  pendingConfirm_ = writer_->pendingConfirm();
  rolledBack_ = writer_->rolledBack();
  confirmDeadline_ = nowMs + VL_OTA_CONFIRM_TIMEOUT_MS;
}

void Ota::onOnline() {
  if (!begun_ || !client_) return;
  uint64_t now = client_->uptimeMs();
  bootCheck(now);
  if (pendingConfirm_) {
    // Connected once: the new image is good, so cancel the rollback.
    pendingConfirm_ = false;
    writer_->confirm();
    VL_LOGI("ota: new firmware confirmed");
  }
  if (!infoSent_) {
    char buf[2 * (VL_OTA_TITLE_SIZE + VL_OTA_VERSION_SIZE) + 48];
    JsonWriter w(buf, sizeof(buf));
    codec::fwInfo(w, title_, version_);
    infoSent_ = sendTelemetry(w);
  }
  requestTarget(now);
  // Resume at the current chunk with a new request id.
  if (phase_ == Phase::Downloading && !target_.url[0]) requestChunk(now);
}

void Ota::tick(uint64_t nowMs) {
  if (!begun_ || !client_) return;
  bootCheck(nowMs);
  if (pendingConfirm_ && nowMs >= confirmDeadline_) {
    pendingConfirm_ = false;
    VL_LOGE("ota: new firmware never came online, rolling back");
    writer_->rollback();
  }
  if (needTarget_ && client_->online() && nowMs >= targetRetryAt_) requestTarget(nowMs);
  if (phase_ == Phase::Downloading) {
    if (target_.url[0]) {
      httpStep(nowMs);
    } else if (deadline_ && nowMs >= deadline_) {
      if (++retries_ > VL_OTA_RETRIES) {
        fail("chunk timeout");
        return;
      }
      VL_LOGW("ota: chunk %lu timed out, retry %u of %u", static_cast<unsigned long>(chunk_),
              static_cast<unsigned>(retries_), static_cast<unsigned>(VL_OTA_RETRIES));
      requestChunk(nowMs);
    }
  } else if (phase_ == Phase::Updating && !restarting_) {
    // Restart once fw_state UPDATING is delivered, or when the wait runs out.
    if (nowMs >= restartAt_ || client_->stats().queuedRecords == 0) {
      restarting_ = true;
      VL_LOGI("ota: restarting into %s %s", target_.title, target_.version);
      writer_->restart();
    }
  }
}

void Ota::onDetach() {
  abortDownload();
  if (phase_ == Phase::Downloading) {
    phase_ = Phase::Idle;
    target_ = Target();
  }
  targetReq_ = 0;
  ApiModule::onDetach();
}

void Ota::requestTarget(uint64_t nowMs) {
  needTarget_ = false;
  if (targetReq_) {
    client_->cancelPending(targetReq_);
    targetReq_ = 0;
  }
  uint32_t id = client_->addPending(VL_ATTR_TIMEOUT_MS, onTargetResponse, this, this);
  if (id != 0) {
    char buf[sizeof(kTargetKeys) + 24];
    JsonWriter w(buf, sizeof(buf));
    codec::attributeRequest(w, nullptr, kTargetKeys);
    RecordMeta m;
    m.topic = OutTopic::AttrRequest;
    m.cls = RecordClass::Control;
    m.arg = id;
    if (ok(client_->enqueue(m, reinterpret_cast<const uint8_t*>(buf), w.size()))) {
      targetReq_ = id;
      return;
    }
    client_->cancelPending(id);
  }
  needTarget_ = true;
  targetRetryAt_ = nowMs + VL_OTA_RETRY_DELAY_MS;
}

void Ota::onTargetResponse(Result r, const uint8_t* p, size_t n, void* ctx) {
  Ota* o = static_cast<Ota*>(ctx);
  o->targetReq_ = 0;
  if (r == Result::Timeout) {
    o->needTarget_ = true;
    o->targetRetryAt_ = 0;
    return;
  }
  if (r != Result::Ok) return;  // NotReady: the client stopped
  o->applyAttributes(JsonValue::parse(reinterpret_cast<const char*>(p), n)["shared"], true);
}

bool Ota::applyAttributes(JsonValue attrs, bool full) {
  if (attrs.type() != JsonValue::Type::Object) {
    if (full) unassign();
    return false;
  }
  if (!attrs["fw_title"].valid()) {
    JsonValue deleted = attrs["deleted"];
    bool removed = false;
    for (size_t i = 0; i < deleted.size() && !removed; ++i) removed = deleted.at(i).equals("fw_title");
    if (full || removed) unassign();
    return removed;
  }
  Target t = Target();
  const char* error = nullptr;
  long long size = attrs["fw_size"].asInt(0);
  if (!copyAttr(attrs["fw_title"], t.title, sizeof(t.title)) ||
      !copyAttr(attrs["fw_version"], t.version, sizeof(t.version)) || !t.title[0] || !t.version[0]) {
    error = "invalid fw_title or fw_version";
  } else if (!copyAttr(attrs["fw_url"], t.url, sizeof(t.url))) {
    error = "fw_url too long";
  } else if (!copyAttr(attrs["fw_checksum"], t.checksum, sizeof(t.checksum)) ||
             !copyAttr(attrs["fw_checksum_algorithm"], t.algoName, sizeof(t.algoName))) {
    error = "invalid fw_checksum";
  } else if (size < 0 || static_cast<unsigned long long>(size) > static_cast<size_t>(-1)) {
    error = "invalid fw_size";
  } else {
    t.size = static_cast<size_t>(size);
  }
  applyTarget(t, error);
  return true;
}

void Ota::applyTarget(const Target& t, const char* error) {
  if (phase_ == Phase::Updating) return;  // the new image boots shortly
  bool same = strcmp(t.title, target_.title) == 0 && strcmp(t.version, target_.version) == 0 &&
              strcmp(t.checksum, target_.checksum) == 0 && strcmp(t.algoName, target_.algoName) == 0 &&
              strcmp(t.url, target_.url) == 0 && t.size == target_.size;
  if (same && phase_ != Phase::Idle) return;  // already downloading, failed or up to date
  abortDownload();
  target_ = t;
  if (strcmp(t.title, title_) == 0 && strcmp(t.version, version_) == 0) {
    phase_ = Phase::Updated;
    rolledBack_ = false;
    if (!updatedSent_) {
      updatedSent_ = true;
      report(OtaState::Updated);
    }
    return;
  }
  if (rolledBack_) {
    // Most likely the package that was just rolled back: never loop on it.
    rolledBack_ = false;
    fail("rolled back");
    return;
  }
  if (error) {
    fail(error);
    return;
  }
  startDownload();
}

void Ota::unassign() {
  if (phase_ == Phase::Updating) return;
  abortDownload();
  target_ = Target();
  phase_ = Phase::Idle;
}

void Ota::startDownload() {
  bool url = target_.url[0] != '\0';
  verify_ = target_.checksum[0] && target_.algoName[0];
  if (verify_ && !parseHashAlgo(target_.algoName, algo_)) {
    fail("unsupported checksum algorithm");
    return;
  }
#if !VL_OTA_ALLOW_UNVERIFIED
  if (!verify_) {
    fail("no checksum");
    return;
  }
#endif
  if (!url && target_.size == 0) {
    fail("no fw_size");
    return;
  }
  if (!writer_) {
    fail("no firmware writer");
    return;
  }
  if (url && !http_) {
    fail("no HTTP client");
    return;
  }
  if (!url) {
    size_t packet = client_->maxPacket();
    if (packet > kServerMaxPayload) packet = kServerMaxPayload;
    chunkSize_ = packet > kChunkOverhead ? packet - kChunkOverhead : 0;
    if (VL_OTA_CHUNK_SIZE != 0 && VL_OTA_CHUNK_SIZE < chunkSize_) chunkSize_ = VL_OTA_CHUNK_SIZE;
    if (chunkSize_ == 0) {
      fail("transport buffer too small");
      return;
    }
  }
  Result r = writer_->begin(target_.size);
  if (r != Result::Ok) {
    fail(r == Result::TooLarge ? "image too large" : "flash not ready");
    return;
  }
  writerOpen_ = true;
  if (verify_) hasher_.begin(algo_);
  written_ = 0;
  expected_ = target_.size;
  chunk_ = 0;
  retries_ = 0;
  firstReq_ = nextReq_;
  phase_ = Phase::Downloading;
  VL_LOGI("ota: downloading %s %s", target_.title, target_.version);
  report(OtaState::Downloading);
  uint64_t now = client_->uptimeMs();
  if (url) {
    deadline_ = now;  // httpStep() opens the connection
  } else {
    requestChunk(now);
  }
}

void Ota::requestChunk(uint64_t nowMs) {
  deadline_ = 0;
  if (!client_->online()) return;  // onOnline() asks again
  char body[12];
  int n = snprintf(body, sizeof(body), "%lu", static_cast<unsigned long>(chunkSize_));
  RecordMeta m;
  m.topic = OutTopic::FwChunkRequest;
  m.cls = RecordClass::Control;
  m.arg = nextReq_++;
  m.arg2 = chunk_;
  Result r = client_->enqueue(m, reinterpret_cast<const uint8_t*>(body), static_cast<size_t>(n));
  // A refused request is retried like a lost one.
  deadline_ = nowMs + (ok(r) ? VL_OTA_CHUNK_TIMEOUT_MS : VL_OTA_RETRY_DELAY_MS);
}

void Ota::onChunk(const Topic& topic, Bytes payload) {
  // Responses to earlier requests, or for a chunk already written, are dropped.
  if (phase_ != Phase::Downloading || target_.url[0] || topic.id < firstReq_ || topic.id >= nextReq_ ||
      topic.id2 != chunk_) {
    return;
  }
  size_t left = expected_ - written_;
  size_t n = payload.size() < left ? payload.size() : left;
  if (n == 0) {
    fail("image shorter than fw_size");
    return;
  }
  if (n < chunkSize_ && n < left) {
    fail("short chunk");
    return;
  }
  if (!consume(payload.data(), n)) return;
  retries_ = 0;
  ++chunk_;
  if (written_ >= expected_) {
    finishDownload();
  } else {
    requestChunk(client_->uptimeMs());
  }
}

void Ota::httpStep(uint64_t nowMs) {
  if (!httpOpen_) {
    if (nowMs < deadline_) return;
    size_t start = 0, total = 0;
    const ITlsProvider* tls = httpTls_ ? httpTls_ : client_->tls();
    if (http_->open(target_.url, written_, tls, start, total) != Result::Ok) {
      httpRetry(nowMs, "download failed");
      return;
    }
    httpOpen_ = true;
    if (start != written_) {
      // The server ignored the Range request: start the image over.
      VL_LOGW("ota: server cannot resume, download restarts");
      writer_->abort();
      writerOpen_ = false;
      if (start != 0 || writer_->begin(target_.size) != Result::Ok) {
        fail("download restart failed");
        return;
      }
      writerOpen_ = true;
      if (verify_) hasher_.begin(algo_);
      written_ = 0;
    }
    if (!expected_) expected_ = total;
  }
  uint8_t buf[VL_OTA_HTTP_BLOCK];
  size_t want = sizeof(buf);
  if (expected_ && expected_ - written_ < want) want = expected_ - written_;
  int n = http_->read(buf, want);
  if (n > 0) {
    if (!consume(buf, static_cast<size_t>(n))) return;
    retries_ = 0;
    if (expected_ && written_ >= expected_) finishDownload();
    return;
  }
  http_->close();
  httpOpen_ = false;
  if (n == 0 && (expected_ ? written_ >= expected_ : written_ > 0)) {
    finishDownload();
    return;
  }
  httpRetry(nowMs, n == 0 ? "download incomplete" : "download failed");
}

void Ota::httpRetry(uint64_t nowMs, const char* error) {
  if (++retries_ > VL_OTA_RETRIES) {
    fail(error);
    return;
  }
  VL_LOGW("ota: %s, retry %u of %u", error, static_cast<unsigned>(retries_), static_cast<unsigned>(VL_OTA_RETRIES));
  deadline_ = nowMs + VL_OTA_RETRY_DELAY_MS;
}

bool Ota::consume(const uint8_t* p, size_t n) {
  if (writer_->write(p, n) != Result::Ok) {
    fail("flash write failed");
    return false;
  }
  if (verify_) hasher_.update(p, n);
  written_ += n;
  if (progressCb_) progressCb_(written_, expected_, progressCtx_);
  return true;
}

void Ota::finishDownload() {
  if (httpOpen_) {
    http_->close();
    httpOpen_ = false;
  }
  deadline_ = 0;
  report(OtaState::Downloaded);
  if (verify_) {
    char hex[Hasher::kHexSize];
    hasher_.hex(hex);
    if (!equalsHex(hex, target_.checksum)) {
      VL_LOGE("ota: checksum %s, expected %s", hex, target_.checksum);
      fail("checksum mismatch");
      return;
    }
    report(OtaState::Verified);
  }
  writerOpen_ = false;
  if (writer_->finish() != Result::Ok) {
    writer_->abort();
    fail("image rejected");
    return;
  }
  phase_ = Phase::Updating;
  report(OtaState::Updating);
  restartAt_ = client_->uptimeMs() + VL_OTA_RESTART_DELAY_MS;
}

void Ota::abortDownload() {
  if (httpOpen_) {
    http_->close();
    httpOpen_ = false;
  }
  if (writerOpen_) {
    writer_->abort();
    writerOpen_ = false;
  }
  deadline_ = 0;
}

void Ota::fail(const char* error) {
  abortDownload();
  phase_ = Phase::Failed;
  VL_LOGE("ota: %s %s failed: %s", target_.title, target_.version, error);
  report(OtaState::Failed, error);
}

bool Ota::sendTelemetry(const JsonWriter& w) {
  if (w.overflowed()) return false;
  RecordMeta m = client_->stampedMeta(OutTopic::Telemetry, kNoDevice);
  m.cls = RecordClass::Control;  // never dropped with data
  return ok(client_->enqueue(m, reinterpret_cast<const uint8_t*>(w.c_str()), w.size()));
}

void Ota::report(OtaState s, const char* error) {
  state_ = s;
  if (client_) {
    char buf[96];
    JsonWriter w(buf, sizeof(buf));
    codec::fwState(w, toString(s), error);
    if (!sendTelemetry(w)) VL_LOGW("ota: fw_state %s not queued", toString(s));
  }
  if (stateCb_) stateCb_(s, error, stateCtx_);
}

}  // namespace vl

#endif  // VL_ENABLE_OTA

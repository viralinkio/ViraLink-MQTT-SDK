// Persistent outbox journal: save and restore, removals, flush policy,
// segment reclaim, torn tails, store errors.
#include <string.h>
#include <unity.h>

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "Fakes.h"
#include "vl/core/Journal.h"

using namespace vl;
using vltest::FakeFlash;

void setUp() {}
void tearDown() {}

namespace {

// A gateway's sub-device table: index = position in names.
struct Names : DeviceMap {
  std::vector<std::string> names;
  bool full = false;
  const char* name(uint16_t dev) override { return dev < names.size() ? names[dev].c_str() : nullptr; }
  uint16_t index(const char* name) override {
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == name) return static_cast<uint16_t>(i);
    }
    if (full) return kNoDevice;
    names.push_back(name);
    return static_cast<uint16_t>(names.size() - 1);
  }
};

// One boot of a device: a RAM ring and a journal over a flash that outlives it.
struct Device {
  RingOutbox<1024> outbox;
  NullLock lock;
  uint8_t scratch[600];
  Journal journal;
  uint16_t nextPacket = 1;

  explicit Device(FakeFlash& f) : journal(f, scratch, sizeof(scratch)) {}

  Result begin(DeviceMap* names = nullptr) { return journal.begin(outbox, lock, names); }
  Result push(const std::string& p, RecordClass cls = RecordClass::Data, TimeKind tk = TimeKind::Utc,
              uint64_t time = 1000, uint16_t dev = kNoDevice) {
    RecordMeta m;
    m.topic = OutTopic::Attributes;
    m.cls = cls;
    m.timeKind = tk;
    m.time = time;
    m.dev = dev;
    m.bootId = 9;
    m.arg = 7;
    m.arg2 = 8;
    return outbox.push(m, reinterpret_cast<const uint8_t*>(p.data()), p.size());
  }
  std::vector<std::string> contents() const {
    std::vector<std::string> out;
    RecordView r;
    for (size_t i = 0; outbox.peek(i, r); ++i) out.push_back(std::string(reinterpret_cast<const char*>(r.payload), r.len));
    return out;
  }
  // Publishes every sendable record and acknowledges those for which keep() is false.
  void deliverAllBut(const std::string& keep = std::string()) {
    RecordView r;
    std::vector<uint32_t> held;
    while (outbox.beginSend(r)) {
      if (std::string(reinterpret_cast<const char*>(r.payload), r.len) == keep) {
        held.push_back(r.seq);
        continue;
      }
      uint16_t id = nextPacket++;
      outbox.markSent(r.seq, id, 0);
      TEST_ASSERT_TRUE(outbox.ack(id));
    }
    for (size_t i = 0; i < held.size(); ++i) outbox.markUnsent(held[i]);
  }
};

std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? "," : "") + v[i];
  return s;
}

}  // namespace

static void test_crc16_vector() {
  TEST_ASSERT_EQUAL_HEX16(0x29B1, crc16(reinterpret_cast<const uint8_t*>("123456789"), 9));
}

static void test_saved_records_survive_restart() {
  FakeFlash f(6, 512);
  {
    Device d(f);
    TEST_ASSERT_EQUAL(Result::Ok, d.begin());
    d.push("a");
    d.push("b", RecordClass::Control, TimeKind::Uptime, 5000);
    d.push("reply", RecordClass::Priority);  // RPC replies are not saved
    TEST_ASSERT_EQUAL(Result::Ok, d.journal.flush());
  }
  f.powerCycle();
  Device d(f);
  TEST_ASSERT_EQUAL(Result::Ok, d.begin());
  TEST_ASSERT_EQUAL_STRING("a,b", join(d.contents()).c_str());
  RecordView r;
  TEST_ASSERT_TRUE(d.outbox.peek(0, r));
  TEST_ASSERT_EQUAL(RecordClass::Data, r.meta.cls);
  TEST_ASSERT_EQUAL(OutTopic::Attributes, r.meta.topic);
  TEST_ASSERT_EQUAL(TimeKind::Utc, r.meta.timeKind);
  TEST_ASSERT_EQUAL_UINT64(1000, r.meta.time);
  TEST_ASSERT_EQUAL_UINT16(kNoDevice, r.meta.dev);
  TEST_ASSERT_EQUAL_UINT32(7, r.meta.arg);
  TEST_ASSERT_EQUAL_UINT32(8, r.meta.arg2);
  TEST_ASSERT_TRUE(d.outbox.peek(1, r));
  TEST_ASSERT_EQUAL(RecordClass::Control, r.meta.cls);
  // Uptime belongs to the earlier boot: the server stamps the record instead.
  TEST_ASSERT_EQUAL(TimeKind::None, r.meta.timeKind);
  TEST_ASSERT_EQUAL_UINT(0, d.outbox.unsaved());
  TEST_ASSERT_FALSE(f.overwrote);
}

static void test_removed_records_not_restored() {
  FakeFlash f(6, 512);
  {
    Device d(f);
    d.begin();
    d.push("a");
    d.push("b");
    d.push("c");
    d.journal.flush();
    d.deliverAllBut("a");
    d.push("d");
    d.journal.flush();
  }
  f.powerCycle();
  Device d(f);
  TEST_ASSERT_EQUAL(Result::Ok, d.begin());
  TEST_ASSERT_EQUAL_STRING("a,d", join(d.contents()).c_str());
}

static void test_unsaved_removal_means_resend() {
  FakeFlash f(6, 512);
  {
    Device d(f);
    d.begin();
    d.push("a");
    d.journal.flush();
    d.deliverAllBut();  // acknowledged, but the removal is not saved yet
  }
  f.powerCycle();
  Device d(f);
  d.begin();
  TEST_ASSERT_EQUAL_STRING("a", join(d.contents()).c_str());  // at least once
}

static void test_evicted_saved_record_not_restored() {
  FakeFlash f(6, 512);
  std::string p(200, 'x');
  {
    Device d(f);
    d.begin();
    for (int i = 0; i < 3; ++i) d.push(p + char('0' + i));
    d.journal.flush();
    d.push(p + '3');  // the ring holds three: evicts the first
    TEST_ASSERT_EQUAL_UINT(1, d.outbox.dropped());
    d.journal.flush();
  }
  f.powerCycle();
  Device d(f);
  d.begin();
  TEST_ASSERT_EQUAL_STRING((p + "1," + p + "2," + p + "3").c_str(), join(d.contents()).c_str());
}

static void test_flush_policy() {
  FakeFlash f(6, 512);
  Device d(f);
  d.begin();
  int w = f.writes;
  d.journal.tick(1000);
  TEST_ASSERT_EQUAL(w, f.writes);  // nothing pending

  // Control: on the next tick.
  d.push("ctl", RecordClass::Control);
  d.journal.tick(1000);
  TEST_ASSERT_GREATER_THAN(w, f.writes);
  TEST_ASSERT_EQUAL_UINT(0, d.outbox.unsaved());

  // Data: after VL_OUTBOX_FLUSH_MS...
  w = f.writes;
  for (int i = 0; i < VL_OUTBOX_FLUSH_RECORDS - 1; ++i) d.push("d");
  d.journal.tick(2000);
  d.journal.tick(2000 + VL_OUTBOX_FLUSH_MS - 1);
  TEST_ASSERT_EQUAL(w, f.writes);
  d.journal.tick(2000 + VL_OUTBOX_FLUSH_MS);
  TEST_ASSERT_GREATER_THAN(w, f.writes);
  TEST_ASSERT_EQUAL_UINT(0, d.outbox.unsaved());

  // ...or once VL_OUTBOX_FLUSH_RECORDS are unsaved.
  w = f.writes;
  for (int i = 0; i < VL_OUTBOX_FLUSH_RECORDS; ++i) d.push("e");
  d.journal.tick(10000);
  TEST_ASSERT_GREATER_THAN(w, f.writes);

  // Removals: on the same timer, or at once when the list is half full
  // (2 * VL_OUTBOX_FLUSH_RECORDS saved records were just acknowledged).
  w = f.writes;
  d.deliverAllBut();
  d.journal.tick(10001);
  TEST_ASSERT_GREATER_THAN(w, f.writes);
}

static void test_reclaim_copies_live_records_forward() {
  FakeFlash f(6, 512);
  std::string p(60, 'p');
  {
    Device d(f);
    d.begin();
    d.push("keep", RecordClass::Control);
    for (int i = 0; i < 300; ++i) {
      d.push(p + std::to_string(i));
      TEST_ASSERT_EQUAL(Result::Ok, d.journal.flush());
      d.deliverAllBut("keep");
      TEST_ASSERT_EQUAL(Result::Ok, d.journal.flush());
    }
    TEST_ASSERT_GREATER_THAN(20, f.erases);
    TEST_ASSERT_FALSE(d.journal.failed());
    TEST_ASSERT_LESS_THAN(6, d.journal.segmentsUsed());
  }
  TEST_ASSERT_FALSE(f.overwrote);
  f.powerCycle();
  Device d(f);
  TEST_ASSERT_EQUAL(Result::Ok, d.begin());
  TEST_ASSERT_EQUAL_STRING("keep", join(d.contents()).c_str());
}

static void test_torn_tail_keeps_earlier_records() {
  FakeFlash f(6, 512);
  {
    Device d(f);
    d.begin();
    d.push("a");
    d.push("b");
    d.journal.flush();
    d.push("c");
    f.budget = 20;  // power fails part way through c's entry
    TEST_ASSERT_EQUAL(Result::Rejected, d.journal.flush());
    TEST_ASSERT_TRUE(d.journal.failed());
  }
  f.powerCycle();
  {
    Device d(f);
    TEST_ASSERT_EQUAL(Result::Ok, d.begin());
    TEST_ASSERT_EQUAL_STRING("a,b", join(d.contents()).c_str());
    d.push("d");
    TEST_ASSERT_EQUAL(Result::Ok, d.journal.flush());  // not after the torn entry
  }
  TEST_ASSERT_FALSE(f.overwrote);
  f.powerCycle();
  Device d(f);
  d.begin();
  TEST_ASSERT_EQUAL_STRING("a,b,d", join(d.contents()).c_str());
}

static void test_record_too_large_to_save_is_skipped() {
  FakeFlash f(6, 512);
  {
    Device d(f);
    d.begin();
    d.push(std::string(580, 'L'));  // entry larger than the 600-byte scratch
    d.push("small");
    TEST_ASSERT_EQUAL(Result::Ok, d.journal.flush());
    TEST_ASSERT_EQUAL_UINT(0, d.outbox.unsaved());
    TEST_ASSERT_EQUAL_UINT(2, d.outbox.records());
  }
  f.powerCycle();
  Device d(f);
  d.begin();
  TEST_ASSERT_EQUAL_STRING("small", join(d.contents()).c_str());
}

static void test_store_error_stops_saving() {
  FakeFlash f(6, 512);
  Device d(f);
  d.begin();
  d.push("a");
  f.failNext = true;
  TEST_ASSERT_EQUAL(Result::Rejected, d.journal.flush());
  TEST_ASSERT_TRUE(d.journal.failed());
  int w = f.writes;
  d.push("b", RecordClass::Control);
  d.journal.tick(100000);
  TEST_ASSERT_EQUAL(w, f.writes);
  TEST_ASSERT_EQUAL_STRING("a,b", join(d.contents()).c_str());
}

static void test_store_too_small_rejected() {
  FakeFlash two(2, 4096);
  Device a(two);
  TEST_ASSERT_EQUAL(Result::Rejected, a.begin());
  FakeFlash small(4, 512);  // 2 KB < twice the ring plus two segments
  Device b(small);
  TEST_ASSERT_EQUAL(Result::Rejected, b.begin());
}

static void test_records_queued_before_begin_follow_restored_ones() {
  FakeFlash f(6, 512);
  {
    Device d(f);
    d.begin();
    d.push("a");
    d.push("b");
    d.journal.flush();
  }
  f.powerCycle();
  {
    Device d(f);
    d.push("new");
    TEST_ASSERT_EQUAL(Result::Ok, d.begin());
    TEST_ASSERT_EQUAL_STRING("a,b,new", join(d.contents()).c_str());
    d.journal.flush();
  }
  f.powerCycle();
  Device d(f);
  d.begin();
  TEST_ASSERT_EQUAL_STRING("a,b,new", join(d.contents()).c_str());
}

static void test_sub_device_records_keep_their_device_by_name() {
  FakeFlash f(6, 512);
  {
    Names n;
    n.names = {"Meter-A", "Meter-B"};
    Device d(f);
    d.begin(&n);
    d.push("a", RecordClass::Data, TimeKind::Utc, 1000, 0);
    d.push("b", RecordClass::Data, TimeKind::Utc, 1000, 1);
    d.push("main");
    d.push("lost", RecordClass::Data, TimeKind::Utc, 1000, 7);  // unknown index: not saved
    TEST_ASSERT_EQUAL(Result::Ok, d.journal.flush());
  }
  f.powerCycle();
  // This boot the gateway added its devices in the other order.
  Names n;
  n.names = {"Meter-B", "Meter-A"};
  Device d(f);
  TEST_ASSERT_EQUAL(Result::Ok, d.begin(&n));
  TEST_ASSERT_EQUAL_STRING("a,b,main", join(d.contents()).c_str());
  RecordView r;
  d.outbox.peek(0, r);
  TEST_ASSERT_EQUAL_UINT16(1, r.meta.dev);  // Meter-A
  d.outbox.peek(1, r);
  TEST_ASSERT_EQUAL_UINT16(0, r.meta.dev);  // Meter-B
  d.outbox.peek(2, r);
  TEST_ASSERT_EQUAL_UINT16(kNoDevice, r.meta.dev);
}

static void test_sub_device_records_dropped_without_a_gateway() {
  FakeFlash f(6, 512);
  {
    Names n;
    n.names = {"Meter-A"};
    Device d(f);
    d.begin(&n);
    d.push("a", RecordClass::Data, TimeKind::Utc, 1000, 0);
    d.push("main");
    d.journal.flush();
  }
  f.powerCycle();
  {
    Device d(f);
    TEST_ASSERT_EQUAL(Result::Ok, d.begin());  // no map: never guess a device
    TEST_ASSERT_EQUAL_STRING("main", join(d.contents()).c_str());
  }
  f.powerCycle();
  Names full;
  full.full = true;
  Device d(f);
  TEST_ASSERT_EQUAL(Result::Ok, d.begin(&full));  // table full: same
  TEST_ASSERT_EQUAL_STRING("main", join(d.contents()).c_str());
}

static void test_transient_records_never_saved() {
  FakeFlash f(6, 512);
  {
    Device d(f);
    d.begin();
    RecordMeta m;
    m.cls = RecordClass::Control;
    m.topic = OutTopic::Claim;
    m.transient = true;
    const char secret[] = "{\"secretKey\":\"s3cret\"}";
    d.outbox.push(m, reinterpret_cast<const uint8_t*>(secret), strlen(secret));
    d.push("a");
    TEST_ASSERT_EQUAL_UINT(1, d.outbox.unsaved());
    TEST_ASSERT_EQUAL_UINT(0, d.outbox.unsavedControl());
    d.journal.flush();
  }
  std::string image(f.mem.begin(), f.mem.end());
  TEST_ASSERT_EQUAL(std::string::npos, image.find("s3cret"));
  f.powerCycle();
  Device d(f);
  d.begin();
  TEST_ASSERT_EQUAL_STRING("a", join(d.contents()).c_str());
}

// ---- Power-cut fuzzing ----

namespace {

std::string fuzzPayload(uint32_t id, size_t len) { return std::to_string(id) + "#" + std::string(len, char('a' + id % 26)); }

uint32_t idOf(const std::string& p) { return static_cast<uint32_t>(std::stoul(p.substr(0, p.find('#')))); }

std::set<uint32_t> ids(const std::vector<std::string>& v) {
  std::set<uint32_t> out;
  for (size_t i = 0; i < v.size(); ++i) out.insert(idOf(v[i]));
  return out;
}

// What the next boot must restore: at least `required`, nothing outside `allowed`.
struct Expect {
  std::set<uint32_t> required;
  std::set<uint32_t> allowed;
};

void checkRestored(const Device& d, const Expect& e, int boot) {
  std::vector<std::string> got = d.contents();
  char msg[64];
  snprintf(msg, sizeof(msg), "boot %d", boot);
  uint32_t prev = 0;
  for (size_t i = 0; i < got.size(); ++i) {
    uint32_t id = idOf(got[i]);
    TEST_ASSERT_TRUE_MESSAGE(i == 0 || id > prev, msg);  // seq order, no duplicates
    prev = id;
    size_t fill = got[i].size() - got[i].find('#') - 1;
    TEST_ASSERT_EQUAL_STRING_MESSAGE(fuzzPayload(id, fill).c_str(), got[i].c_str(), msg);
    TEST_ASSERT_TRUE_MESSAGE(e.allowed.count(id) == 1, msg);  // nothing removed before a completed save
  }
  std::set<uint32_t> have = ids(got);
  for (std::set<uint32_t>::const_iterator it = e.required.begin(); it != e.required.end(); ++it) {
    TEST_ASSERT_TRUE_MESSAGE(have.count(*it) == 1, msg);  // saved and still queued at the cut
  }
}

}  // namespace

static void powerCutFuzz(bool fileSystem) {
  FakeFlash f(6, 512);
  f.loseUnsynced = fileSystem;
  std::mt19937 rng(20261026);
  uint32_t nextId = 1;
  uint64_t now = 0;
  Expect expect;
  int cuts = 0;
  for (int boot = 0; boot < 400; ++boot) {
    f.powerCycle();
    Device d(f);
    if (rng() % 5 == 0) f.budget = static_cast<long>(rng() % 600);  // power fails during recovery
    if (d.begin() != Result::Ok) {
      TEST_ASSERT_TRUE(f.dead);  // only a power cut may fail recovery
      ++cuts;
      continue;  // the store must still satisfy the same expectation
    }
    checkRestored(d, expect, boot);
    TEST_ASSERT_EQUAL_UINT(0, d.outbox.unsaved());

    // Everything restored is saved: a baseline as good as a completed flush.
    std::set<uint32_t> saved = ids(d.contents());
    std::set<uint32_t> since;
    if (f.budget < 0) f.budget = static_cast<long>(rng() % 4000);
    int ops = static_cast<int>(rng() % 300);
    for (int op = 0; op < ops && !f.dead; ++op) {
      unsigned r = rng() % 100;
      if (r < 50) {
        uint32_t id = nextId++;
        RecordClass cls = rng() % 10 == 0 ? RecordClass::Control : RecordClass::Data;
        if (vl::ok(d.push(fuzzPayload(id, rng() % 150), cls))) since.insert(id);
      } else if (r < 75) {
        int k = 1 + static_cast<int>(rng() % 6);
        RecordView v;
        while (k-- > 0 && d.outbox.beginSend(v)) {
          uint16_t pid = d.nextPacket++;
          d.outbox.markSent(v.seq, pid, now);
          d.outbox.ack(pid);
        }
      } else if (r < 90) {
        now += rng() % 3000;
        d.journal.tick(now);
      } else if (d.journal.flush() == Result::Ok) {
        saved = ids(d.contents());
        since.clear();
      }
    }
    if (f.dead) ++cuts;
    TEST_ASSERT_FALSE(f.overwrote);
    // At the cut: what a completed save held and is still queued must come
    // back; anything else may, if pushed since that save.
    std::set<uint32_t> live = ids(d.contents());
    expect.required.clear();
    std::set_intersection(saved.begin(), saved.end(), live.begin(), live.end(),
                          std::inserter(expect.required, expect.required.begin()));
    expect.allowed = saved;
    expect.allowed.insert(since.begin(), since.end());
  }
  TEST_ASSERT_GREATER_THAN(250, cuts);
  TEST_ASSERT_GREATER_THAN(200, f.erases);
}

static void test_power_cut_fuzz_flash() { powerCutFuzz(false); }
static void test_power_cut_fuzz_file_system() { powerCutFuzz(true); }

// A long-lived control record makes every reclaim copy it forward. Power
// fails at each byte of the run in turn, including inside those copies.
static void everyCutPoint(bool fileSystem) {
  const std::string keep = fuzzPayload(0, 150);
  int cutInReclaim = 0;
  for (long budget = 0;; ++budget) {
    FakeFlash f(6, 512);
    f.loseUnsynced = fileSystem;
    std::set<uint32_t> saved, since, live;
    int erasesBefore = 0;
    {
      Device d(f);
      TEST_ASSERT_EQUAL(Result::Ok, d.begin());
      f.budget = budget;
      d.push(keep, RecordClass::Control);
      since.insert(0);
      for (uint32_t id = 1; id <= 25 && !f.dead; ++id) {
        d.push(fuzzPayload(id, 150));
        since.insert(id);
        erasesBefore = f.erases;
        if (d.journal.flush() == Result::Ok) {
          saved = ids(d.contents());
          since.clear();
        }
        d.deliverAllBut(keep);
      }
      live = ids(d.contents());
      if (f.dead && f.erases > erasesBefore) ++cutInReclaim;
    }
    if (!f.dead) break;  // the whole run fit in the budget: every cut point done
    TEST_ASSERT_FALSE(f.overwrote);
    Expect e;
    std::set_intersection(saved.begin(), saved.end(), live.begin(), live.end(),
                          std::inserter(e.required, e.required.begin()));
    e.allowed = saved;
    e.allowed.insert(since.begin(), since.end());
    f.powerCycle();
    {
      Device d(f);
      TEST_ASSERT_EQUAL(Result::Ok, d.begin());
      checkRestored(d, e, static_cast<int>(budget));
      // The recovered store keeps working.
      d.push(fuzzPayload(99, 10));
      TEST_ASSERT_EQUAL(Result::Ok, d.journal.flush());
      live = ids(d.contents());
    }
    TEST_ASSERT_FALSE(f.overwrote);
    f.powerCycle();
    Device d(f);
    TEST_ASSERT_EQUAL(Result::Ok, d.begin());
    TEST_ASSERT_TRUE(ids(d.contents()) == live);
  }
  TEST_ASSERT_GREATER_THAN(50, cutInReclaim);
}

static void test_every_cut_point_flash() { everyCutPoint(false); }
static void test_every_cut_point_file_system() { everyCutPoint(true); }

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc16_vector);
  RUN_TEST(test_saved_records_survive_restart);
  RUN_TEST(test_removed_records_not_restored);
  RUN_TEST(test_unsaved_removal_means_resend);
  RUN_TEST(test_evicted_saved_record_not_restored);
  RUN_TEST(test_flush_policy);
  RUN_TEST(test_reclaim_copies_live_records_forward);
  RUN_TEST(test_torn_tail_keeps_earlier_records);
  RUN_TEST(test_record_too_large_to_save_is_skipped);
  RUN_TEST(test_store_error_stops_saving);
  RUN_TEST(test_store_too_small_rejected);
  RUN_TEST(test_records_queued_before_begin_follow_restored_ones);
  RUN_TEST(test_sub_device_records_keep_their_device_by_name);
  RUN_TEST(test_sub_device_records_dropped_without_a_gateway);
  RUN_TEST(test_transient_records_never_saved);
  RUN_TEST(test_power_cut_fuzz_flash);
  RUN_TEST(test_power_cut_fuzz_file_system);
  RUN_TEST(test_every_cut_point_flash);
  RUN_TEST(test_every_cut_point_file_system);
  return UNITY_END();
}

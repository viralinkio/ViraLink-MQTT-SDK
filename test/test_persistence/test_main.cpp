// vl::Persistence through a client, and FileOutboxStore on the host file
// system.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unity.h>

#include <string>
#include <vector>

#include "ClientRig.h"
#include "vl/api/Persistence.h"
#include "vl/pal/FileOutboxStore.h"

using namespace vl;
using vltest::ClientRig;
using vltest::FakeFlash;
using vltest::Published;

void setUp() {}
void tearDown() {}

namespace {

// One boot: a client with persistence over a store that outlives it.
struct Boot : ClientRig {
  Persistence persist;
  explicit Boot(IOutboxStore& store) : persist(store) { c.use(persist); }
};

std::string topicsAndPayloads(const std::vector<Published>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) s += (i ? " | " : "") + v[i].topic + " " + v[i].payload;
  return s;
}

}  // namespace

static void test_queued_records_survive_restart() {
  FakeFlash f(6, 4096);
  {
    Boot b(f);
    TEST_ASSERT_EQUAL(Result::Ok, b.persist.status());
    TEST_ASSERT_EQUAL(Result::Ok, b.c.begin(b.cfg));  // never connects
    b.c.telemetry().add("n", 1).send();
    TEST_ASSERT_EQUAL(Result::Queued, b.c.claim("s", 1000));
    b.c.loop();  // control record: saved now, with the telemetry before it
    TEST_ASSERT_EQUAL_UINT(0, b.c.outbox().unsaved());
    b.c.telemetry().add("n", 2).send();
    b.c.loop();  // the save window starts when a loop first sees the record
    b.clock.advance(VL_OUTBOX_FLUSH_MS - 1);
    b.c.loop();
    TEST_ASSERT_EQUAL_UINT(1, b.c.outbox().unsaved());
    b.clock.advance(1);
    b.c.loop();
    TEST_ASSERT_EQUAL_UINT(0, b.c.outbox().unsaved());
  }
  f.powerCycle();
  Boot b(f);
  TEST_ASSERT_EQUAL(Result::Ok, b.persist.status());
  TEST_ASSERT_EQUAL_UINT(3, b.c.stats().queuedRecords);
  b.connect();
  // Uptime stamps of the earlier boot are dropped: the server stamps them.
  TEST_ASSERT_EQUAL_STRING(
      "v1/devices/me/telemetry {\"n\":1} | v1/devices/me/claim {\"secretKey\":\"s\",\"durationMs\":1000} | "
      "v1/devices/me/telemetry {\"n\":2}",
      topicsAndPayloads(b.flush()).c_str());
}

static void test_delivered_records_not_resent() {
  FakeFlash f(6, 4096);
  {
    Boot b(f);
    b.c.begin(b.cfg);
    b.c.telemetry().add("n", 1).send();
    b.persist.flush();  // saved while offline
    b.c.loop();
    b.t.connack(0);
    b.t.subackAll();
    TEST_ASSERT_EQUAL_UINT(1, b.flush().size());  // delivered and acknowledged
    b.c.loop();
    b.clock.advance(VL_OUTBOX_FLUSH_MS);
    b.c.loop();  // the removal is saved
  }
  f.powerCycle();
  Boot b(f);
  TEST_ASSERT_EQUAL_UINT(0, b.c.stats().queuedRecords);
  b.connect();
  TEST_ASSERT_EQUAL_UINT(0, b.flush().size());
}

static void test_unusable_store_leaves_client_in_ram() {
  FakeFlash f(2, 512);
  Boot b(f);
  TEST_ASSERT_EQUAL(Result::Rejected, b.persist.status());
  b.connect();
  b.c.telemetry().add("n", 1).send();
  TEST_ASSERT_EQUAL_UINT(1, b.flush().size());
  TEST_ASSERT_EQUAL(Result::Rejected, b.persist.flush());
}

static void test_detach_stops_saving() {
  FakeFlash f(6, 4096);
  ClientRig r;
  {
    Persistence p(f);
    r.c.use(p);
    TEST_ASSERT_EQUAL(Result::Ok, p.status());
  }  // detached: the outbox no longer reports to it
  r.connect();
  r.c.telemetry().add("n", 1).send();
  TEST_ASSERT_EQUAL_UINT(1, r.flush().size());
}

#if VL_HAS_FILE_STORE
static void test_file_store_round_trip() {
  char dir[] = "/tmp/vl-outbox-XXXXXX";
  TEST_ASSERT_NOT_NULL(mkdtemp(dir));
  {
    FileOutboxStore s(dir, 20, 512);
    Boot b(s);
    TEST_ASSERT_EQUAL(Result::Ok, b.persist.status());
    for (int i = 0; i < 60; ++i) b.c.telemetry().add("n", i).send();  // spans several segment files
    TEST_ASSERT_EQUAL(Result::Ok, b.persist.flush());
  }
  {
    FileOutboxStore s(dir, 20, 512);
    Boot b(s);
    TEST_ASSERT_EQUAL(Result::Ok, b.persist.status());
    TEST_ASSERT_EQUAL_UINT(60, b.c.stats().queuedRecords);
    b.connect();
    std::vector<Published> all;
    for (int i = 0; i < 20 && all.size() < 60; ++i) {
      std::vector<Published> out = b.flush();
      all.insert(all.end(), out.begin(), out.end());
    }
    TEST_ASSERT_EQUAL_UINT(60, all.size());
    TEST_ASSERT_EQUAL_STRING("{\"n\":0}", all[0].payload.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"n\":59}", all[59].payload.c_str());
    TEST_ASSERT_EQUAL(Result::Ok, b.persist.flush());
    b.c.loop();
  }
  {
    FileOutboxStore s(dir, 20, 512);
    Boot b(s);
    TEST_ASSERT_EQUAL_UINT(0, b.c.stats().queuedRecords);
  }
  for (int i = 0; i < 20; ++i) {
    std::string p = std::string(dir) + "/ob" + std::to_string(i);
    unlink(p.c_str());
  }
  TEST_ASSERT_EQUAL(0, rmdir(dir));
}
#endif

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_queued_records_survive_restart);
  RUN_TEST(test_delivered_records_not_resent);
  RUN_TEST(test_unusable_store_leaves_client_in_ram);
  RUN_TEST(test_detach_stops_saving);
#if VL_HAS_FILE_STORE
  RUN_TEST(test_file_store_round_trip);
#endif
  return UNITY_END();
}

// FlashKvStore on an emulated NOR chip: the key-value contract, remounts,
// compaction, and a power cut at every byte of a run of writes.
#include <string.h>
#include <unity.h>

#include <map>
#include <string>

#include "Fakes.h"
#include "vl/flash/FlashKvStore.h"

using namespace vl;
using vltest::FakeNor;

void setUp() {}
void tearDown() {}

namespace {

Result put(FlashKvStore& s, const char* key, const std::string& v) { return s.set(key, v.data(), v.size()); }

// The value, "<missing>", or "<error N>".
std::string get(FlashKvStore& s, const char* key) {
  char buf[600];
  size_t len = 0;
  Result r = s.get(key, buf, sizeof(buf), len);
  if (r == Result::Rejected) return "<missing>";
  if (r != Result::Ok) return "<error " + std::to_string(static_cast<int>(r)) + ">";
  return std::string(buf, len);
}

}  // namespace

static void test_set_get_erase() {
  FakeNor nor(2 * 4096);
  FlashKvStore s(nor);
  TEST_ASSERT_EQUAL_STRING("<missing>", get(s, "token").c_str());
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "token", "abc"));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "host", "h.example"));
  TEST_ASSERT_EQUAL_STRING("abc", get(s, "token").c_str());
  TEST_ASSERT_EQUAL_STRING("h.example", get(s, "host").c_str());
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "token", "second"));  // the last write wins
  TEST_ASSERT_EQUAL_STRING("second", get(s, "token").c_str());
  TEST_ASSERT_EQUAL(Result::Ok, s.erase("token"));
  TEST_ASSERT_EQUAL_STRING("<missing>", get(s, "token").c_str());
  TEST_ASSERT_EQUAL(Result::Ok, s.erase("token"));  // already gone
  TEST_ASSERT_EQUAL(Result::Ok, s.erase("never"));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "empty", ""));
  TEST_ASSERT_EQUAL_STRING("", get(s, "empty").c_str());
  TEST_ASSERT_EQUAL_STRING("h.example", get(s, "host").c_str());
  TEST_ASSERT_FALSE(nor.overwrote);
}

static void test_keys_and_sizes() {
  FakeNor nor(2 * 4096);
  FlashKvStore s(nor);
  TEST_ASSERT_EQUAL(Result::Rejected, put(s, "", "x"));
  TEST_ASSERT_EQUAL(Result::Rejected, put(s, "sixteen-chars-xx", "x"));
  TEST_ASSERT_EQUAL(Result::Rejected, s.set(nullptr, "x", 1));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "fifteen-chars-x", "x"));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "tok", "0123456789"));
  char small[4];
  size_t len = 0;
  TEST_ASSERT_EQUAL(Result::TooLarge, s.get("tok", small, sizeof(small), len));
  TEST_ASSERT_EQUAL(Result::NoMemory, put(s, "big", std::string(4096, 'b')));  // more than a bank
  TEST_ASSERT_EQUAL_STRING("0123456789", get(s, "tok").c_str());
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "tok", "ab"));  // a shorter key prefix is another key
  TEST_ASSERT_EQUAL_STRING("<missing>", get(s, "to").c_str());
}

static void test_values_survive_a_remount() {
  FakeNor nor(4 * 4096);
  {
    FlashKvStore s(nor);
    TEST_ASSERT_EQUAL(Result::Ok, put(s, "a", "1"));
    TEST_ASSERT_EQUAL(Result::Ok, put(s, "b", "2"));
    TEST_ASSERT_EQUAL(Result::Ok, s.erase("a"));
  }
  FlashKvStore again(nor);
  TEST_ASSERT_EQUAL_STRING("<missing>", get(again, "a").c_str());
  TEST_ASSERT_EQUAL_STRING("2", get(again, "b").c_str());
  TEST_ASSERT_EQUAL(Result::Ok, put(again, "c", "3"));
  TEST_ASSERT_EQUAL_STRING("3", get(again, "c").c_str());
}

static void test_same_value_writes_nothing() {
  FakeNor nor(2 * 4096);
  FlashKvStore s(nor);
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "token", "abc"));
  int programs = nor.programs;
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "token", "abc"));
  TEST_ASSERT_EQUAL_INT(programs, nor.programs);
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "token", "abd"));
  TEST_ASSERT_TRUE(nor.programs > programs);
}

static void test_compaction_keeps_live_values() {
  FakeNor nor(2 * 512, 512);
  FlashKvStore s(nor);
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "keep", "kept value"));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "gone", "soon deleted"));
  TEST_ASSERT_EQUAL(Result::Ok, s.erase("gone"));
  for (int i = 0; i < 200; ++i) {  // many banks' worth of rewrites
    TEST_ASSERT_EQUAL(Result::Ok, put(s, "counter", std::to_string(i)));
    TEST_ASSERT_EQUAL(Result::Ok, put(s, "other", "v" + std::to_string(i % 7)));
  }
  TEST_ASSERT_TRUE(nor.erases >= 10);
  TEST_ASSERT_EQUAL_STRING("kept value", get(s, "keep").c_str());
  TEST_ASSERT_EQUAL_STRING("199", get(s, "counter").c_str());
  TEST_ASSERT_EQUAL_STRING("v3", get(s, "other").c_str());
  TEST_ASSERT_EQUAL_STRING("<missing>", get(s, "gone").c_str());
  TEST_ASSERT_FALSE(nor.overwrote);
  FlashKvStore again(nor);
  TEST_ASSERT_EQUAL_STRING("kept value", get(again, "keep").c_str());
  TEST_ASSERT_EQUAL_STRING("199", get(again, "counter").c_str());
}

static void test_full_store() {
  FakeNor nor(2 * 512, 512);
  FlashKvStore s(nor);
  std::string v(100, 'v');
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "k1", v));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "k2", v));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "k3", v));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "k4", v));
  TEST_ASSERT_EQUAL(Result::NoMemory, put(s, "k5", v));  // five live values do not fit in 512 bytes
  TEST_ASSERT_EQUAL_STRING(v.c_str(), get(s, "k4").c_str());
  TEST_ASSERT_EQUAL_STRING("<missing>", get(s, "k5").c_str());
  TEST_ASSERT_EQUAL(Result::Ok, s.erase("k1"));
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "k5", v));  // room again
  TEST_ASSERT_EQUAL_STRING(v.c_str(), get(s, "k5").c_str());
  TEST_ASSERT_EQUAL_STRING(v.c_str(), get(s, "k2").c_str());
}

static void test_flash_not_ready_or_failing() {
  FakeNor nor(2 * 4096);
  nor.ready = false;
  FlashKvStore s(nor);
  char buf[4];
  size_t len;
  TEST_ASSERT_EQUAL(Result::NotReady, s.get("a", buf, sizeof(buf), len));
  TEST_ASSERT_EQUAL(Result::NotReady, put(s, "a", "1"));
  nor.ready = true;  // the chip came up later
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "a", "1"));

  FakeNor tiny(4096);  // one block: no room for two banks
  FlashKvStore t(tiny);
  TEST_ASSERT_EQUAL(Result::NotReady, put(t, "a", "1"));

  // A read error at mount is not mistaken for blank flash: nothing is erased.
  FakeNor broken(2 * 4096);
  {
    FlashKvStore w(broken);
    TEST_ASSERT_EQUAL(Result::Ok, put(w, "a", "1"));
  }
  broken.dead = true;
  int erases = broken.erases;
  FlashKvStore r(broken);
  TEST_ASSERT_NOT_EQUAL(Result::Ok, r.get("a", buf, sizeof(buf), len));
  TEST_ASSERT_EQUAL_INT(erases, broken.erases);
  broken.dead = false;
  TEST_ASSERT_EQUAL_STRING("1", get(r, "a").c_str());
}

static void test_torn_record_then_write() {
  FakeNor nor(2 * 4096);
  {
    FlashKvStore s(nor);
    TEST_ASSERT_EQUAL(Result::Ok, put(s, "a", "old"));
    nor.budget = 5;
    TEST_ASSERT_NOT_EQUAL(Result::Ok, put(s, "a", "new value"));
  }
  nor.powerCycle();
  FlashKvStore s(nor);
  TEST_ASSERT_EQUAL_STRING("old", get(s, "a").c_str());
  TEST_ASSERT_EQUAL(Result::Ok, put(s, "b", "after"));  // compacts past the torn record
  TEST_ASSERT_FALSE(nor.overwrote);
  TEST_ASSERT_EQUAL_STRING("after", get(s, "b").c_str());
  TEST_ASSERT_EQUAL_STRING("old", get(s, "a").c_str());
  FlashKvStore again(nor);
  TEST_ASSERT_EQUAL_STRING("after", get(again, "b").c_str());
}

// A scripted run of writes on a store small enough to compact several times,
// cut at every budget: afterwards each key holds its last acknowledged value,
// except the key being written at the cut, which holds its old or new value.
static void test_power_cut_at_every_byte() {
  struct Op {
    const char* key;
    const char* value;  // null: erase
  };
  const Op ops[] = {{"token", "t-1"},   {"host", "a.example"}, {"token", "t-2-longer"}, {"port", "1883"},
                    {"host", nullptr},  {"token", "t-3"},      {"cert", "PEM......................"},
                    {"host", "b.ex"},   {"token", "t-4"},      {"port", "8883"},        {"cert", nullptr},
                    {"token", "t-5"},   {"host", "c.example"}, {"token", "t-6"},        {"port", "1884"}};
  const size_t kOps = sizeof(ops) / sizeof(ops[0]);

  // Budget that lets the whole run finish.
  long total = 0;
  {
    FakeNor nor(2 * 128, 128);
    nor.budget = 1L << 30;
    FlashKvStore s(nor);
    for (size_t i = 0; i < kOps; ++i) {
      Result r = ops[i].value ? put(s, ops[i].key, ops[i].value) : s.erase(ops[i].key);
      TEST_ASSERT_EQUAL(Result::Ok, r);
    }
    total = (1L << 30) - nor.budget;
    TEST_ASSERT_TRUE(nor.erases >= 3);  // the format and at least two compactions
  }

  for (long budget = 0; budget <= total; ++budget) {
    FakeNor nor(2 * 128, 128);
    std::map<std::string, std::string> model;
    std::string inFlight, oldValue, newValue;
    bool oldMissing = false, newMissing = false;
    {
      FlashKvStore s(nor);
      nor.budget = budget;
      for (size_t i = 0; i < kOps; ++i) {
        const Op& op = ops[i];
        Result r = op.value ? put(s, op.key, op.value) : s.erase(op.key);
        if (r != Result::Ok) {
          TEST_ASSERT_TRUE(nor.dead);
          inFlight = op.key;
          oldMissing = !model.count(op.key);
          oldValue = oldMissing ? "" : model[op.key];
          newMissing = !op.value;
          newValue = op.value ? op.value : "";
          break;
        }
        if (op.value) {
          model[op.key] = op.value;
        } else {
          model.erase(op.key);
        }
      }
    }
    nor.powerCycle();
    FlashKvStore s(nor);
    const char* keys[] = {"token", "host", "port", "cert"};
    for (const char* k : keys) {
      std::string got = get(s, k);
      std::string msg = "budget " + std::to_string(budget) + " key " + k + ": " + got;
      if (k == inFlight) {
        bool isOld = oldMissing ? got == "<missing>" : got == oldValue;
        bool isNew = newMissing ? got == "<missing>" : got == newValue;
        TEST_ASSERT_TRUE_MESSAGE(isOld || isNew, msg.c_str());
      } else {
        std::string want = model.count(k) ? model[k] : "<missing>";
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), got.c_str(), msg.c_str());
      }
    }
    // Still writable, and the write lasts.
    TEST_ASSERT_EQUAL_MESSAGE(Result::Ok, put(s, "after", "cut"), std::to_string(budget).c_str());
    FlashKvStore again(nor);
    TEST_ASSERT_EQUAL_STRING("cut", get(again, "after").c_str());
    TEST_ASSERT_FALSE(nor.overwrote);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_set_get_erase);
  RUN_TEST(test_keys_and_sizes);
  RUN_TEST(test_values_survive_a_remount);
  RUN_TEST(test_same_value_writes_nothing);
  RUN_TEST(test_compaction_keeps_live_values);
  RUN_TEST(test_full_store);
  RUN_TEST(test_flash_not_ready_or_failing);
  RUN_TEST(test_torn_record_then_write);
  RUN_TEST(test_power_cut_at_every_byte);
  return UNITY_END();
}

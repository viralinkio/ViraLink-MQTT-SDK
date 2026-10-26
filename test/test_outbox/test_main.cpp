#include <string.h>
#include <unity.h>

#include <string>
#include <vector>

#include "vl/core/Outbox.h"

using namespace vl;

void setUp() {}
void tearDown() {}

static RecordMeta meta(RecordClass cls = RecordClass::Data, OutTopic t = OutTopic::Telemetry) {
  RecordMeta m;
  m.cls = cls;
  m.topic = t;
  return m;
}

static Result push(Outbox& o, const char* s, RecordClass cls = RecordClass::Data) {
  return o.push(meta(cls), reinterpret_cast<const uint8_t*>(s), strlen(s));
}

// Sends everything sendable, returning payloads in send order with packet ids 1..n.
static std::vector<std::string> sendAll(Outbox& o, uint16_t firstId = 1, uint64_t now = 0) {
  std::vector<std::string> out;
  RecordView r;
  uint16_t id = firstId;
  while (o.beginSend(r)) {
    out.push_back(std::string(reinterpret_cast<const char*>(r.payload), r.len));
    o.markSent(r.seq, id++, now);
  }
  return out;
}

static void test_fifo_and_ack_frees() {
  RingOutbox<1024> o;
  TEST_ASSERT_EQUAL(Result::Queued, push(o, "a"));
  TEST_ASSERT_EQUAL(Result::Queued, push(o, "b"));
  TEST_ASSERT_EQUAL(Result::Queued, push(o, "c"));
  std::vector<std::string> s = sendAll(o);
  TEST_ASSERT_EQUAL_UINT(3, s.size());
  TEST_ASSERT_EQUAL_STRING("a", s[0].c_str());
  TEST_ASSERT_EQUAL_STRING("c", s[2].c_str());
  TEST_ASSERT_EQUAL_UINT(3, o.inFlight());
  // Out-of-order PUBACK still frees the right record.
  TEST_ASSERT_TRUE(o.ack(2));
  TEST_ASSERT_FALSE(o.ack(2));
  TEST_ASSERT_EQUAL_UINT(2, o.records());
  TEST_ASSERT_TRUE(o.ack(1));
  TEST_ASSERT_TRUE(o.ack(3));
  TEST_ASSERT_EQUAL_UINT(0, o.records());
  TEST_ASSERT_EQUAL_UINT(0, o.bytesUsed());
}

static void test_record_stays_until_acked_and_resends_after_close() {
  RingOutbox<1024> o;
  push(o, "a");
  push(o, "b");
  sendAll(o, 1, 0);
  o.onSessionClosed(60000);  // long after sending: no strike
  std::vector<std::string> again = sendAll(o, 10);
  TEST_ASSERT_EQUAL_UINT(2, again.size());
  TEST_ASSERT_EQUAL_STRING("a", again[0].c_str());
  // Old packet ids no longer match.
  TEST_ASSERT_FALSE(o.ack(1));
  TEST_ASSERT_TRUE(o.ack(10));
  TEST_ASSERT_EQUAL_UINT(0, o.deadLetters());
}

static void test_too_large_rejected_at_enqueue() {
  RingOutbox<1024> o;
  o.setMaxPayload(16);
  TEST_ASSERT_EQUAL(Result::TooLarge, push(o, "01234567890123456"));
  TEST_ASSERT_EQUAL(Result::Queued, push(o, "0123456789012345"));
  RingOutbox<256> small;
  std::string big(250, 'x');
  TEST_ASSERT_EQUAL(Result::TooLarge, push(small, big.c_str()));
}

static void test_data_dropped_oldest_first_and_counted() {
  RingOutbox<512> o;  // reserve 51 bytes; each record 48 bytes
  int queued = 0;
  char s[4];
  for (int i = 0; i < 20; ++i) {
    snprintf(s, sizeof(s), "%d", i);
    if (push(o, s) == Result::Queued) ++queued;
  }
  TEST_ASSERT_EQUAL(20, queued);
  TEST_ASSERT_TRUE(o.dropped() > 0);
  TEST_ASSERT_TRUE(o.bytesUsed() <= o.capacity() - o.controlReserve());
  std::vector<std::string> s2 = sendAll(o);
  TEST_ASSERT_EQUAL_UINT(20 - o.dropped(), s2.size());
  TEST_ASSERT_EQUAL_STRING("19", s2.back().c_str());
  // Survivors are the newest, still in order.
  snprintf(s, sizeof(s), "%u", static_cast<unsigned>(o.dropped()));
  TEST_ASSERT_EQUAL_STRING(s, s2.front().c_str());
}

static void test_control_never_dropped_and_uses_reserve() {
  RingOutbox<512> o;
  TEST_ASSERT_EQUAL(Result::Queued, push(o, "ctl", RecordClass::Control));
  for (int i = 0; i < 20; ++i) push(o, "d");
  // Control may use the reserve, evicting data if needed.
  TEST_ASSERT_EQUAL(Result::Queued, push(o, "ctl2", RecordClass::Control));
  std::vector<std::string> s = sendAll(o);
  TEST_ASSERT_EQUAL_STRING("ctl", s.front().c_str());
  TEST_ASSERT_EQUAL_STRING("ctl2", s.back().c_str());
}

static void test_control_queue_full_when_only_control_left() {
  RingOutbox<512> o;
  int queued = 0;
  for (int i = 0; i < 20; ++i) {
    if (push(o, "c", RecordClass::Control) == Result::Queued) ++queued;
  }
  TEST_ASSERT_EQUAL(512 / 48, queued);
  TEST_ASSERT_EQUAL(Result::QueueFull, push(o, "c", RecordClass::Control));
  TEST_ASSERT_EQUAL(Result::QueueFull, push(o, "d"));
  TEST_ASSERT_EQUAL_UINT32(1, o.dropped());  // only the data record counts as dropped
}

static void test_in_flight_records_are_never_evicted() {
  RingOutbox<512> o;
  push(o, "first");
  RecordView r;
  TEST_ASSERT_TRUE(o.beginSend(r));
  o.markSent(r.seq, 1, 0);
  for (int i = 0; i < 30; ++i) push(o, "d");
  TEST_ASSERT_TRUE(o.ack(1));  // still there
}

static void test_priority_jumps_unsent_but_stays_fifo() {
  RingOutbox<1024> o;
  push(o, "a");
  RecordView r;
  TEST_ASSERT_TRUE(o.beginSend(r));
  o.markSent(r.seq, 1, 0);  // "a" in flight
  push(o, "b");
  push(o, "r1", RecordClass::Priority);
  push(o, "r2", RecordClass::Priority);
  std::vector<std::string> s = sendAll(o, 2);
  TEST_ASSERT_EQUAL_UINT(3, s.size());
  TEST_ASSERT_EQUAL_STRING("r1", s[0].c_str());
  TEST_ASSERT_EQUAL_STRING("r2", s[1].c_str());
  TEST_ASSERT_EQUAL_STRING("b", s[2].c_str());
}

static void test_poison_record_dead_lettered_after_three_quick_closes() {
  RingOutbox<1024> o;
  push(o, "bad");
  push(o, "good");
  uint64_t now = 100000;
  for (int i = 0; i < 3; ++i) {
    RecordView r;
    TEST_ASSERT_TRUE(o.beginSend(r));
    TEST_ASSERT_EQUAL_STRING_LEN("bad", r.payload, 3);
    o.markSent(r.seq, 1, now);
    o.onSessionClosed(now + 200);  // server closed right after the publish
    now += 10000;
  }
  TEST_ASSERT_EQUAL_UINT32(1, o.deadLetters());
  std::vector<std::string> s = sendAll(o);
  TEST_ASSERT_EQUAL_UINT(1, s.size());
  TEST_ASSERT_EQUAL_STRING("good", s[0].c_str());
}

static void test_ordinary_outages_add_no_strikes() {
  RingOutbox<1024> o;
  push(o, "a");
  for (int i = 0; i < 10; ++i) {
    RecordView r;
    TEST_ASSERT_TRUE(o.beginSend(r));
    o.markSent(r.seq, 1, 0);
    o.onSessionClosed(6000);  // outside the 5 s window
  }
  TEST_ASSERT_EQUAL_UINT32(0, o.deadLetters());
  TEST_ASSERT_EQUAL_UINT(1, o.records());
}

static void test_mark_unsent_makes_record_sendable_again() {
  RingOutbox<256> o;
  push(o, "a");
  RecordView r;
  TEST_ASSERT_TRUE(o.beginSend(r));
  TEST_ASSERT_FALSE(o.beginSend(r));
  o.markUnsent(r.seq);
  TEST_ASSERT_TRUE(o.beginSend(r));
}

static void test_stamp_utc_only_current_boot_uptime_records() {
  RingOutbox<1024> o;
  RecordMeta m = meta();
  m.timeKind = TimeKind::Uptime;
  m.bootId = 7;
  m.time = 1000;
  o.push(m, reinterpret_cast<const uint8_t*>("a"), 1);
  m.bootId = 8;  // earlier boot
  o.push(m, reinterpret_cast<const uint8_t*>("b"), 1);
  TEST_ASSERT_EQUAL_UINT(1, o.stampUtc(7, 5000, 1767225600000ull));
  RecordView r;
  TEST_ASSERT_TRUE(o.beginSend(r));
  TEST_ASSERT_EQUAL(TimeKind::Utc, r.meta.timeKind);
  TEST_ASSERT_TRUE(r.meta.time == 1767225600000ull - 4000);
  TEST_ASSERT_TRUE(o.beginSend(r));
  TEST_ASSERT_EQUAL(TimeKind::Uptime, r.meta.timeKind);
}

static void test_compaction_keeps_payloads_contiguous() {
  RingOutbox<512> o;
  // Cycle many records through so head/tail wrap repeatedly.
  char s[16];
  uint16_t id = 1;
  for (int i = 0; i < 200; ++i) {
    snprintf(s, sizeof(s), "rec-%03d", i);
    TEST_ASSERT_EQUAL(Result::Queued, push(o, s));
    if (i % 3 == 2) {
      RecordView r;
      while (o.beginSend(r)) {
        TEST_ASSERT_EQUAL_UINT(7, r.len);
        TEST_ASSERT_EQUAL_MEMORY("rec-", r.payload, 4);
        o.markSent(r.seq, id, 0);
        TEST_ASSERT_TRUE(o.ack(id++));
      }
    }
  }
  TEST_ASSERT_EQUAL_UINT32(0, o.dropped());
}

static void test_fuzz_against_model() {
  RingOutbox<2048> o;
  std::vector<std::string> model;  // unacked payloads, in queue order (data only, no drops)
  uint32_t rng = 12345;
  uint16_t id = 1;
  for (int step = 0; step < 5000; ++step) {
    rng = rng * 1103515245u + 12345u;
    if ((rng >> 16) % 3 != 0 && o.bytesUsed() < 1200) {
      std::string p(1 + (rng >> 8) % 60, static_cast<char>('a' + step % 26));
      TEST_ASSERT_EQUAL(Result::Queued, push(o, p.c_str()));
      model.push_back(p);
    } else {
      RecordView r;
      if (o.beginSend(r)) {
        TEST_ASSERT_FALSE(model.empty());
        TEST_ASSERT_EQUAL_UINT(model.front().size(), r.len);
        TEST_ASSERT_EQUAL_MEMORY(model.front().data(), r.payload, r.len);
        o.markSent(r.seq, id, 0);
        TEST_ASSERT_TRUE(o.ack(id++));
        model.erase(model.begin());
      }
    }
    TEST_ASSERT_EQUAL_UINT(model.size(), o.records());
  }
}

// ---- Persistence support ----

static std::string payloadOf(const RecordView& r) { return std::string(reinterpret_cast<const char*>(r.payload), r.len); }

static std::vector<uint32_t> removedSeqs;
static void onRemoved(uint32_t seq, void*) { removedSeqs.push_back(seq); }

static void test_new_records_unsaved_priority_never_saved() {
  RingOutbox<1024> o;
  push(o, "a");
  push(o, "c", RecordClass::Control);
  push(o, "r", RecordClass::Priority);
  TEST_ASSERT_EQUAL_UINT(2, o.unsaved());
  TEST_ASSERT_EQUAL_UINT(1, o.unsavedControl());
  RecordView r;
  TEST_ASSERT_TRUE(o.nextUnsaved(r));
  TEST_ASSERT_EQUAL_STRING("a", payloadOf(r).c_str());
  o.setSaved(r.seq, 0);
  TEST_ASSERT_TRUE(o.nextUnsaved(r));
  TEST_ASSERT_EQUAL_STRING("c", payloadOf(r).c_str());
  TEST_ASSERT_EQUAL(RecordClass::Control, r.meta.cls);
  o.setSaved(r.seq, 1);
  TEST_ASSERT_FALSE(o.nextUnsaved(r));
  TEST_ASSERT_EQUAL_UINT(0, o.unsaved());
  TEST_ASSERT_EQUAL_UINT(0, o.unsavedControl());
  // Marking a record "never saved" also takes it off the unsaved list.
  push(o, "big");
  TEST_ASSERT_TRUE(o.nextUnsaved(r));
  o.setSaved(r.seq, Outbox::kNotSaved);
  TEST_ASSERT_FALSE(o.nextUnsaved(r));
  TEST_ASSERT_EQUAL_UINT(0, o.unsaved());
}

static void test_remove_hook_only_for_saved_records() {
  removedSeqs.clear();
  RingOutbox<1024> o;
  o.setRemoveHook(onRemoved, nullptr);
  push(o, "saved");
  push(o, "unsaved");
  RecordView r;
  o.nextUnsaved(r);
  uint32_t savedSeq = r.seq;
  o.setSaved(savedSeq, 2);
  std::vector<std::string> s = sendAll(o);
  TEST_ASSERT_EQUAL_UINT(2, s.size());
  TEST_ASSERT_TRUE(o.ack(1));
  TEST_ASSERT_TRUE(o.ack(2));
  TEST_ASSERT_EQUAL_UINT(1, removedSeqs.size());
  TEST_ASSERT_EQUAL_UINT32(savedSeq, removedSeqs[0]);
  TEST_ASSERT_EQUAL_UINT(0, o.unsaved());
}

static void test_remove_hook_on_eviction_and_dead_letter() {
  removedSeqs.clear();
  RingOutbox<512> o;  // 461 bytes for data: three 144-byte records
  o.setRemoveHook(onRemoved, nullptr);
  std::string p(100, 'x');
  push(o, p.c_str());
  RecordView r;
  o.nextUnsaved(r);
  o.setSaved(r.seq, 0);
  for (int i = 0; i < 3; ++i) push(o, p.c_str());  // the third evicts the saved one
  TEST_ASSERT_EQUAL_UINT(1, o.dropped());
  TEST_ASSERT_EQUAL_UINT(1, removedSeqs.size());
  TEST_ASSERT_EQUAL_UINT32(r.seq, removedSeqs[0]);

  o.nextUnsaved(r);
  o.setSaved(r.seq, 0);
  o.discard(r.seq);
  TEST_ASSERT_EQUAL_UINT(2, removedSeqs.size());
  TEST_ASSERT_EQUAL_UINT32(r.seq, removedSeqs[1]);
}

static void test_next_in_segment() {
  RingOutbox<1024> o;
  push(o, "a");
  push(o, "b");
  push(o, "c");
  RecordView r;
  uint8_t segs[] = {3, 4, 3};
  for (int i = 0; i < 3; ++i) {
    o.nextUnsaved(r);
    o.setSaved(r.seq, segs[i]);
  }
  TEST_ASSERT_TRUE(o.nextInSegment(3, r));
  TEST_ASSERT_EQUAL_STRING("a", payloadOf(r).c_str());
  o.setSaved(r.seq, 5);  // copied forward
  TEST_ASSERT_TRUE(o.nextInSegment(3, r));
  TEST_ASSERT_EQUAL_STRING("c", payloadOf(r).c_str());
  o.setSaved(r.seq, 5);
  TEST_ASSERT_FALSE(o.nextInSegment(3, r));
  TEST_ASSERT_TRUE(o.nextInSegment(4, r));
  TEST_ASSERT_EQUAL_STRING("b", payloadOf(r).c_str());
}

static RecordView view(uint32_t seq, const char* payload, RecordClass cls = RecordClass::Data) {
  RecordView r;
  r.meta.cls = cls;
  r.meta.topic = OutTopic::Attributes;
  r.meta.arg = 7;
  r.meta.time = 123;
  r.meta.timeKind = TimeKind::Utc;
  r.seq = seq;
  r.payload = reinterpret_cast<const uint8_t*>(payload);
  r.len = strlen(payload);
  return r;
}

static void test_restore_orders_by_seq_ahead_of_new_records() {
  RingOutbox<1024> o;
  push(o, "new1");  // queued this boot before the restore
  push(o, "new2");
  TEST_ASSERT_EQUAL(Result::Ok, o.restore(view(20, "r20"), 1));
  TEST_ASSERT_EQUAL(Result::Ok, o.restore(view(10, "r10", RecordClass::Control), 0));
  TEST_ASSERT_EQUAL(Result::Ok, o.restore(view(30, "r30"), 1));
  // A later copy of seq 10 only moves it to the newer segment.
  TEST_ASSERT_EQUAL(Result::Ok, o.restore(view(10, "r10", RecordClass::Control), 2));
  TEST_ASSERT_EQUAL_UINT(5, o.records());
  o.finishRestore(30);

  RecordView r;
  TEST_ASSERT_TRUE(o.nextInSegment(2, r));
  TEST_ASSERT_EQUAL_UINT32(10, r.seq);
  TEST_ASSERT_EQUAL(RecordClass::Control, r.meta.cls);
  TEST_ASSERT_EQUAL(OutTopic::Attributes, r.meta.topic);
  TEST_ASSERT_EQUAL_UINT32(7, r.meta.arg);
  TEST_ASSERT_EQUAL(TimeKind::Utc, r.meta.timeKind);
  TEST_ASSERT_FALSE(o.nextInSegment(0, r));
  TEST_ASSERT_EQUAL_UINT(2, o.unsaved());
  TEST_ASSERT_EQUAL_UINT(0, o.unsavedControl());

  std::vector<std::string> s = sendAll(o);
  TEST_ASSERT_EQUAL_UINT(5, s.size());
  TEST_ASSERT_EQUAL_STRING("r10", s[0].c_str());
  TEST_ASSERT_EQUAL_STRING("r20", s[1].c_str());
  TEST_ASSERT_EQUAL_STRING("r30", s[2].c_str());
  TEST_ASSERT_EQUAL_STRING("new1", s[3].c_str());
  TEST_ASSERT_EQUAL_STRING("new2", s[4].c_str());

  // Records queued before the restore were renumbered after it, and new ones follow.
  TEST_ASSERT_TRUE(o.nextUnsaved(r));
  TEST_ASSERT_EQUAL_UINT32(31, r.seq);
  push(o, "next");
  o.setSaved(31, 1);
  TEST_ASSERT_TRUE(o.nextUnsaved(r));
  TEST_ASSERT_EQUAL_UINT32(32, r.seq);
  o.setSaved(32, 1);
  TEST_ASSERT_TRUE(o.nextUnsaved(r));
  TEST_ASSERT_EQUAL_UINT32(33, r.seq);
}

static void test_restore_forget_and_capacity() {
  removedSeqs.clear();
  RingOutbox<512> o;
  o.setRemoveHook(onRemoved, nullptr);
  std::string p(100, 'x');
  for (uint32_t s = 1; s <= 3; ++s) TEST_ASSERT_EQUAL(Result::Ok, o.restore(view(s, p.c_str()), 0));
  // A fourth data record evicts the oldest; the hook reports it, since it is saved.
  TEST_ASSERT_EQUAL(Result::Ok, o.restore(view(4, p.c_str()), 0));
  TEST_ASSERT_EQUAL_UINT(3, o.records());
  TEST_ASSERT_EQUAL_UINT(1, o.dropped());
  TEST_ASSERT_EQUAL_UINT(1, removedSeqs.size());
  TEST_ASSERT_EQUAL_UINT32(1, removedSeqs[0]);
  // A removal entry from the log: no hook, no count.
  o.forget(3);
  TEST_ASSERT_EQUAL_UINT(2, o.records());
  TEST_ASSERT_EQUAL_UINT(1, removedSeqs.size());
  TEST_ASSERT_EQUAL_UINT(1, o.dropped());
  TEST_ASSERT_EQUAL(Result::TooLarge, o.restore(view(9, std::string(600, 'y').c_str()), 0));
}

static void test_forget_never_touches_records_queued_this_boot() {
  RingOutbox<1024> o;
  push(o, "new");  // seq 1, unsaved, not renumbered until finishRestore()
  o.restore(view(1, "old"), 0);
  o.forget(1);
  o.forget(1);  // the log may hold the removal of a seq the ring no longer has
  TEST_ASSERT_EQUAL_UINT(1, o.records());
  RecordView r;
  TEST_ASSERT_TRUE(o.nextUnsaved(r));
  TEST_ASSERT_EQUAL_STRING("new", payloadOf(r).c_str());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fifo_and_ack_frees);
  RUN_TEST(test_record_stays_until_acked_and_resends_after_close);
  RUN_TEST(test_too_large_rejected_at_enqueue);
  RUN_TEST(test_data_dropped_oldest_first_and_counted);
  RUN_TEST(test_control_never_dropped_and_uses_reserve);
  RUN_TEST(test_control_queue_full_when_only_control_left);
  RUN_TEST(test_in_flight_records_are_never_evicted);
  RUN_TEST(test_priority_jumps_unsent_but_stays_fifo);
  RUN_TEST(test_poison_record_dead_lettered_after_three_quick_closes);
  RUN_TEST(test_ordinary_outages_add_no_strikes);
  RUN_TEST(test_mark_unsent_makes_record_sendable_again);
  RUN_TEST(test_stamp_utc_only_current_boot_uptime_records);
  RUN_TEST(test_compaction_keeps_payloads_contiguous);
  RUN_TEST(test_fuzz_against_model);
  RUN_TEST(test_new_records_unsaved_priority_never_saved);
  RUN_TEST(test_remove_hook_only_for_saved_records);
  RUN_TEST(test_remove_hook_on_eviction_and_dead_letter);
  RUN_TEST(test_next_in_segment);
  RUN_TEST(test_restore_orders_by_seq_ahead_of_new_records);
  RUN_TEST(test_restore_forget_and_capacity);
  RUN_TEST(test_forget_never_touches_records_queued_this_boot);
  return UNITY_END();
}

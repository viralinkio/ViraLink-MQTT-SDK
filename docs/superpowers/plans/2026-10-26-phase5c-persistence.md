# Phase 5c: Persistent Outbox Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Queued records survive a restart or power cut. `vl::Persistence` saves the outbox through an
`IOutboxStore` (CRC16-framed records, torn-tail recovery) and restores it on the next boot. Data records are saved
every 5 s or every 16 records, control records on the next `loop()`. A power-cut fuzz test proves recovery.

**Architecture:** The RAM ring stays the working queue and the source of truth; the store is a write-ahead log of it.
A platform-free `Journal` (core) appends *record* entries for records the ring holds and *removal* entries for saved
records that left it (PUBACK, eviction, dead letter). Each ring record remembers the segment that holds its saved copy,
so the journal knows what is unsaved and what lives in a segment it wants to reclaim. Space is reclaimed oldest segment
first: its still-live records are copied forward, then it is erased. On boot the segments are replayed in generation
order into the ring. `Persistence` is an opt-in `ApiModule` that owns the journal and drives it from `tick()`, so an
application that does not use it links none of this code.

**Tech Stack:** C++11 (`-std=gnu++11`), PlatformIO `native` + Unity, ASan/UBSan, `-Werror`; POSIX file API (ESP32 VFS:
LittleFS, FAT or SPIFFS mounted by the application).

## Global Constraints

As in 5a/5b. Flash and file I/O count as I/O: the client lock is held only to copy a record out of the ring or to update
ring bookkeeping, never across a store call. `VL_ENABLE_PERSISTENCE` defaults on outside the Lite profile (the module
costs nothing unless attached).

## Store contract (`IOutboxStore`)

A fixed number of equal-size segments (`segmentCount()` up to `VL_OUTBOX_MAX_SEGMENTS`, `segmentSize()`). `write(seg,
off, p, n)` only appends: `off` is never below anything already written to the segment since its last `erase(seg)`.
`read()` returns the bytes available, fewer at the end of a file; erased NOR reads as `0xFF`. `sync(seg)` makes the
writes durable. Writes made before `sync` may be lost or torn by a power cut; nothing else may change.

## Log format (host byte order; the store never leaves the device)

Entry: `type u8 | 0 u8 | len u16 | body[len] | crc16 u16` (CRC-16/CCITT-FALSE over the first four bytes and the body).

| Type | Body |
| --- | --- |
| 1 segment header (first entry of a segment) | magic `"VLJ1"` u32, generation u32 |
| 2 record | seq u32, topic u8, cls u8, timeKind u8, 0 u8, dev u16, bootId u16, arg u32, arg2 u32, time u64, payload |
| 3 removals | seq u32 × n |

A scan of a segment stops at the end of the data (short read or type `0xFF`) or at the first entry that is truncated or
fails its CRC: the torn tail. A segment without a valid header is free.

## Decisions

- **What is saved.** Data and control records. Priority records (RPC replies) are not: the server has given up on them
  long before a reboot ends. A record whose entry does not fit the scratch buffer or a segment is not saved (logged).
- **When.** Records are copied out on the client task in `tick()`: at once if a control record is unsaved; otherwise
  when 16 records are unsaved, the removal list is half full, or a `tick()` first saw an unsaved change 5 s ago
  (`VL_OUTBOX_FLUSH_MS`, `VL_OUTBOX_FLUSH_RECORDS`). One flush writes the removals first, then every unsaved record in
  ring order, then syncs. `Persistence::flush()` saves everything now, e.g. before deep sleep.
- **Removals** of saved records are collected in a fixed list (`VL_OUTBOX_REMOVALS`, 32) by an outbox hook, which runs
  under the lock and does no I/O. If the list overflows, the extra records may be sent again after a reboot, which the
  at-least-once rule allows.
- **Reclaim.** Opening a new segment always leaves one free segment: if it took the last one, the oldest segment's live
  records (the ring says which) are copied into the new one, synced, and the old one is erased. They always fit, since
  they came from one segment. If the store holds only live data, records stay unsaved until space frees up. The store
  must hold at least 3 segments and twice the ring capacity (`Persistence` refuses smaller ones).
- **Recovery** runs when the module is attached (`client.use()`): segments are replayed oldest generation first. Record
  entries are restored in sequence order ahead of anything queued this boot (a later copy of the same sequence number
  only moves it to the newer segment), removal entries drop records. Restored uptime timestamps belong to an earlier
  boot, so they become "no timestamp" and the server stamps them on arrival. Sequence numbers continue after the
  highest one in the log; records queued before recovery are renumbered (they were never saved). The newest segment
  is appended to only if everything after its last good entry is erased; otherwise writing starts in a new segment.
- **Errors.** A failed store call stops persistence until the next boot (logged); the RAM ring carries on.
- **ESP32 backend.** `FileOutboxStore` uses one file per segment through the POSIX/VFS API (`fopen`, `fwrite`, `fsync`)
  on a file system the application mounts, so it works with Arduino `LittleFS.begin()` and with ESP-IDF alike, and is
  also tested on the host. Raw NOR and FRAM backends for STM32 come with the Phase 6 ports.

## Tasks

### Task 1: Outbox journal support
- [x] Tests (`test_outbox`): new records unsaved; priority never; `setSaved` / counters; remove hook only for saved
  records; `nextInSegment`; `restore` ordering, duplicates and capacity; `finishRestore` renumbering.
- [x] Implement: segment byte in the record header, hook, counters, iteration, restore.

### Task 2: Journal
- [x] `IOutboxStore`, `FakeFlash` (NOR semantics, write budget for power cuts), `crc16`.
- [x] Tests (`test_journal`): CRC vector; save and restore; removals; flush triggers; segment rollover and reclaim;
  torn tail; full store; store errors; restored uptime timestamps.
- [x] Implement `Journal`.

### Task 3: Power-cut fuzzing
- [x] Random producer/ack/evict/flush sequences with power cuts at random byte budgets, chained over many boots.
  After each recovery: every record live at the last completed flush and still live at the cut is restored; nothing
  restored that was removed before that flush; payloads and metadata exact; no duplicates; sequence order.

### Task 4: Module, client hook and file store
- [x] `Client::outbox()`, `vl::Persistence`, `FileOutboxStore`; tests through `ClientRig` and a temp directory.

### Task 5: Docs, example, gate
- [x] `examples/persistence`, README section, `ViraLink.h`, `library.json`, portability, roadmap.
- [x] Gate: `pio test -e native`, `scripts/check_portability.sh`, `scripts/build_examples.sh`.

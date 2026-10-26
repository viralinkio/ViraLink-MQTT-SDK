# Phase 5b: Health Attributes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `vl::Health` reports the device's vital signs as client attributes: `vl_uptime`, `vl_heap_free`,
`vl_heap_min`, `vl_reset_reason`, `vl_rssi`, `vl_iface`, `vl_queue_depth`, `vl_dropped`, `vl_dead_letter` and
`vl_sdk`. The interval is configurable, each key can be switched off, and `VL_COMPAT_V1_KEYS` restores the 1.0.2 names.

**Architecture:** An opt-in `ApiModule` with no subscriptions. Library counters come from `Client::stats()` and
`uptimeMs()`; chip values come from a new platform port, `ISystemInfo` (`platformSystemInfo()`), which a product or
test can replace with `Health::setSource()`. The payload is written by `codec::health()` from a `HealthSample`, so the
shape has golden-string tests.

**Tech Stack:** C++11 (`-std=gnu++11`), PlatformIO `native` + Unity, ASan/UBSan, `-Werror`; ESP-IDF `esp_system`,
`esp_wifi`.

## Global Constraints

Same as Phase 5a: C++11 and no heap in `src/`, capacities as `VL_*` macros, payload shapes in `core/Codec` with golden
tests, portable outside `platform/`. Health compiles only when `VL_ENABLE_HEALTH` is set (default on, Lite included).
Gate: `pio test -e native`, `scripts/check_portability.sh`, `scripts/build_examples.sh`.

## Wire format

Client attributes on `v1/devices/me/attributes`, keys in this order, each present only when enabled and known:

```json
{"vl_uptime":3600,"vl_heap_free":181234,"vl_heap_min":150112,"vl_reset_reason":"power_on","vl_rssi":-61,
 "vl_iface":"wifi","vl_queue_depth":3,"vl_dropped":0,"vl_dead_letter":0,"vl_sdk":"2.0.0-alpha.1"}
```

| Key | Value | 1.0.2 name (`VL_COMPAT_V1_KEYS`) |
| --- | --- | --- |
| `vl_uptime` | seconds since boot | `upTime` |
| `vl_heap_free` | free heap, bytes | `ESP Free Heap` |
| `vl_heap_min` | lowest free heap since boot, bytes | — |
| `vl_reset_reason` | `power_on`, `external`, `software`, `panic`, `int_wdt`, `task_wdt`, `wdt`, `deep_sleep`, `brownout`, `sdio`, `unknown` | — |
| `vl_rssi` | dBm of the active interface | — |
| `vl_iface` | `wifi`, `eth`, `gsm`, ... | — |
| `vl_queue_depth` | records in the outbox | — |
| `vl_dropped` | data records dropped since boot | — |
| `vl_dead_letter` | poison records discarded since boot | — |
| `vl_sdk` | `VL_SDK_VERSION` | `Viralink SDK Version` |

1.0.2 sent only `upTime`, `ESP Free Heap` (every 10 s) and `Viralink SDK Version` (on connect). Keys it never had
keep their `vl_` names in compatibility mode.

## Decisions

- **Only while online.** A snapshot taken offline is stale by the time it is delivered and would fill the ring, so
  nothing is queued while `client.online()` is false (this includes the provisioning session).
- **Full on connect, changing keys on the interval.** Every entry into Online queues all enabled keys. Each interval
  after that queues only the keys that change during a boot; `vl_sdk` and `vl_reset_reason` are left out, since the
  server keeps the last value of an attribute.
- **Interval** defaults to `VL_HEALTH_INTERVAL_MS` (60 s; 1.0.2 used 10 s, which costs a message every 10 s against
  tenant rate limits). 0 reports only on connect. The timer restarts on every connect.
- **Data class.** Health records are ordinary attributes: dropped oldest-first under pressure, never `Control`.
- **Key names** are a runtime choice (`Health::useV1Keys()`) whose default is `VL_COMPAT_V1_KEYS`, so both spellings
  are tested in one build.
- **Platform values.** `ISystemInfo` reports heap, minimum heap, reset reason, interface and RSSI; each can be unknown,
  and an unknown key is omitted. ESP32 uses `esp_get_free_heap_size()`, `esp_get_minimum_free_heap_size()`,
  `esp_reset_reason()`, and `esp_wifi_sta_get_ap_info()` for `wifi` and RSSI while the station is associated. The
  network manager (5e) will supply the interface when it lands. Other targets report nothing until their port (Phase 6).
- **Buffer.** The payload is written on the stack into `VL_HEALTH_SIZE` bytes (320; 160 on Lite). An overflow is logged
  and the report skipped.

## File Structure

| File | Change |
| --- | --- |
| `src/vl/Config.h` | `VL_COMPAT_V1_KEYS`, `VL_HEALTH_INTERVAL_MS`, `VL_HEALTH_SIZE` |
| `src/vl/core/Codec.h/.cpp` | `kHealth*` key bits, `HealthSample`, `codec::health()` |
| `src/vl/pal/SystemInfo.h/.cpp` | `ISystemInfo`, `platformSystemInfo()` (empty source off ESP32) |
| `src/vl/platform/esp32/EspSystemInfo.cpp` | ESP32 source |
| `src/vl/api/Health.h/.cpp` | the module |
| `src/ViraLink.h`, `CMakeLists.txt` | include, `esp_wifi` dependency |
| `test/support/Fakes.h` | `FakeSystemInfo` |
| `test/test_codec`, `test/test_health` | golden strings; module behaviour |
| `examples/basic_telemetry`, `test/portability`, `README.md`, roadmap | usage, docs |

### Task 1: Codec

- [x] Golden tests in `test_codec`: every key with `vl_` names; every key with 1.0.2 names; only some keys; unknown
  values omitted; the longest payload fits `VL_HEALTH_SIZE`.
- [x] Run, see them fail; add `HealthSample`, the key bits and `codec::health()`; run, see them pass.

### Task 2: Port and module

- [x] `ISystemInfo` + host default; `FakeSystemInfo`.
- [x] `test_health`: nothing before Online or during provisioning; full report on Online; interval reports carry only
  changing keys; interval 0; keys switched off; v1 names; reconnect resets the timer; unknown platform values omitted;
  counters come from `Client::stats()`.
- [x] Run, see them fail; implement `Health`; run, see them pass.

### Task 3: ESP32 source, build, docs, gate

- [x] `EspSystemInfo.cpp`, `esp_wifi` in `PRIV_REQUIRES`, `ViraLink.h`.
- [x] Health in `basic_telemetry` and the portability sketch; README section; roadmap tick.
- [x] Gate: `pio test -e native`, `scripts/check_portability.sh`, `scripts/build_examples.sh`.

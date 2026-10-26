# ViraLink Device Client v2

ThingsBoard-compatible MQTT device and gateway client for microcontrollers
(ESP32 today; ESP8266, STM32 and AVR planned). The spec is
`docs/design/ViraLink Device Client v2 — Design Document.md`; read the relevant
section before adding a feature.

## Commands

```sh
pio test -e native            # host unit tests, ASan + UBSan, -Werror
scripts/build_examples.sh     # every example: Arduino + ESP-IDF via PlatformIO
```

Both must pass before work is called done. ESP-IDF builds need the
`espressif32@6.6.0` platform pin (`IDF_PLATFORM` in the script).

## Layout

- `src/vl/core/`: platform-free logic (session, outbox, codec, topics, router,
  correlator, scheduler). Everything here is unit-tested on the host.
- `src/vl/pal/`, `src/vl/platform/<target>/`: clock, lock and transport ports.
- `src/vl/Client.*`: the public client; API modules implement `IApiModule`.
- `test/test_*/`: Unity tests; `test/support/Fakes.h` has `FakeClock` and
  `FakeTransport`. Wire payloads are checked against golden strings.

## Rules that override the C++ skills

The `cpp-pro`, `cpp-coding-standards` and `embedded-systems` skills apply only
where they agree with this section. When they conflict, this section wins.

- **C++11.** Library code (`src/`) builds with `-std=gnu++11` so it compiles on
  AVR. No C++14+ features: no concepts, `std::string_view`, `std::optional`,
  `if constexpr`, `std::scoped_lock` or `make_unique`. Tests may use more.
- **No heap after `begin()`.** No `new`, `malloc`, smart pointers, `std::string`,
  `std::vector` or `std::function` in `src/`. Capacities are template parameters
  or `VL_*` macros in `Config.h`; tables are fixed-size and report full with
  `Result::NoMemory` or `Result::QueueFull`.
- **No exceptions.** Fallible calls return `vl::Result`. Never throw.
- **Callbacks** are plain function pointers with a `void* ctx`, never
  `std::function` or capturing lambdas.
- **JSON** is written with `JsonWriter` (streaming, caller-owned buffer). Every
  payload shape lives in `core/Codec` with a golden-string test.
- **Concurrency.** Only the client task touches the socket, session and router.
  Producers may call send APIs from any task through the client's `ILock`. Hold
  the lock only around outbox and table access: never during network I/O, and
  never while invoking a user or module callback. State shared between tasks
  is `std::atomic`; `volatile` is for hardware registers and ISR flags only.
  Library APIs are never called from an ISR.
- **Raw pointers are non-owning views** (`Span`, `Bytes`, `const char*`) valid
  only for the duration of the call; copy anything that must outlive it.

## Style

Match the surrounding code: 2-space indent, lines up to 120 columns,
`#pragma once`, everything in `namespace vl`. Types are `PascalCase`, functions
and variables `camelCase`, members end in `_`, constants are `kName`, macros are
`VL_NAME`. `typedef` for function-pointer types is fine. Comments explain why
and stay short. New code gets host tests through the fakes, not hardware.

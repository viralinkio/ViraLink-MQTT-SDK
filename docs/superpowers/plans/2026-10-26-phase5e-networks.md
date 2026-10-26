# Phase 5e: Network Manager Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The client runs over several networks (WiFi, LAN8720 Ethernet, GSM) in priority order: it fails over at
once when the active link goes down, and fails back to a better one only after that one has been up for 30 s in a row,
so a flapping WiFi link does not bounce the session (design: Network manager).

**Architecture:** `vl::Networks<N>` is an `IMqttTransport` that forwards to the transport of the active network, so
`Client` and `Session` need no knowledge of it: `vl::Client client(nets, outbox)`. Each network is an `INetwork`
(name, link health, its MQTT transport, `select()`). Networks may share a transport (WiFi and Ethernet both reach the
broker through one `EspMqtt`; `select()` makes the interface lwIP's default route). A switch while a connection exists
disconnects the old transport and reports the link closed, so the session reconnects through the new one and resends
unacknowledged records (at-least-once). The selection logic is platform-free and host-tested with fake networks,
transports and clock.

**Tech Stack:** C++11; host Unity tests; ESP32 (`esp_netif`) adapter build-verified.

## Decisions

- **Priority** is the order of `add()`, best first (the design put a priority on `INetwork`; ordering by `add()` cannot
  collide and is what every example does).
- **Health** is `INetwork::up()`: link-level (associated or registered, with an IP), cheap, sampled every `loop()`.
- **Failover**: the active network down, or no active network → the best network that is up, at once.
- **Fail-back**: a better network that has been up for `failBackMs` (30 s) without a break.
- **Stall**: a network that is up but reaches no broker (WiFi without uplink) would hold the session forever. If the
  session has been trying for `stallMs` (120 s) through the active network without any CONNACK, and another network is
  up, the session moves and the stalled network sits out `retryMs` (5 min) before fail-back may pick it again. Any
  CONNACK, even a refusal, proves the path. A penalised network is still used when nothing else is up.
- **Link return skips backoff**, once per outage and never inside a delay the server asked for (CONNACK 3), so a
  flapping link cannot cause a reconnect storm. `IMqttTransport::linkUp()` (default true) tells the session whether any path exists;
  when it returns after being down, the session in Backoff connects at once instead of waiting out up to 60 s.
  `Networks::linkUp()` is "any network up"; with none up, `beginConnect` reports closed from `loop()`.
- **Events** from a transport that is not the active one are ignored (a late close or PUBACK after a switch).
- **Forwarded queries**: `validate()` asks every transport (all must accept `Config::tls`), `maxPacket()` is the
  smallest, so the outbox never holds a record the next network cannot send.
- **Health attributes**: `Networks` is also an `ISystemInfo`: `vl_iface` is the active network's name and `vl_rssi`
  its signal (`INetwork::rssi()`); heap and reset reason come from the platform. `health.setSource(nets)`.
- **ESP32 adapter** `EspNetif` (Arduino and ESP-IDF): any lwIP interface by key (`WIFI_STA_DEF`, `ETH_DEF`, `PPP_DEF`);
  up = interface up with an IPv4 address; `select()` = `esp_netif_set_default_netif`. lwIP keeps one DNS list: when
  WiFi and Ethernet sit on different LANs, use a public resolver (documented).
- **Generic adapter** `LinkNetwork`: name, transport and an `up` function pointer, for TinyGSM
  (`modem.isGprsConnected()`) or anything else. Bringing the modem up stays the application's job.
- Out of scope: modem network time as a clock source (Phase 6, with the TinyGSM port).

## Tasks

### Task 1: Session link return
- [x] `IMqttTransport::linkUp()`; Session in Backoff connects at once when it goes false → true. Test in test_session.

### Task 2: `Networks<N>`
- [x] Tests (test_networks): first connect on the best up network; none up (closed from loop, linkUp false);
  failover with disconnect + one closed; fail-back hysteresis with a flap resetting it; switch while idle is silent;
  stall and penalty; CONNACK clears stall; inactive events ignored; shared transport; validate / maxPacket; capacity;
  iface and rssi.
- [x] Client integration: online on WiFi, WiFi drops, records resent on Ethernet.

### Task 3: Adapters, example, docs, gate
- [x] `EspNetif`, `LinkNetwork`.
- [x] `examples/multi_network` (WiFi + LAN8720, GSM shown in comments).
- [x] README section, roadmap, gate.

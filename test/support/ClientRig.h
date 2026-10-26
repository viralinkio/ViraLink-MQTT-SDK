// Client wired to a fake transport and clock, for module tests.
#pragma once

#include <unity.h>

#include <vector>

#include "Fakes.h"
#include "vl/Client.h"

namespace vltest {

struct ClientRig {
  FakeTransport t;
  FakeClock clock;
  vl::RingOutbox<4096> outbox;
  vl::Client c;
  vl::Config cfg;

  ClientRig() : c(t, outbox) {
    c.setClock(clock);
    cfg.host = "console.viralink.ir";
    cfg.auth = vl::AccessToken("TOKEN");
    cfg.ratePerSec = 0;
  }
  void connect() {
    TEST_ASSERT_EQUAL(vl::Result::Ok, c.begin(cfg));
    c.loop();
    t.connack(0);
    t.subackAll();
    TEST_ASSERT_EQUAL(vl::State::Online, c.state());
  }
  // Runs the loop twice (module ticks enqueue after the drain), returns what
  // was published and acknowledges it.
  std::vector<Published> flush() {
    t.published.clear();
    c.loop();
    c.loop();
    std::vector<Published> out = t.published;
    t.pubackAll();
    t.published.clear();
    return out;
  }
};

}  // namespace vltest

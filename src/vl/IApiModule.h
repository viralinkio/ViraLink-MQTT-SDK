#pragma once

#include <stdint.h>

#include "vl/Span.h"
#include "vl/core/Topics.h"

namespace vl {

class Client;

// Collects the incoming topic kinds a module wants routed to it. Each kind
// becomes one subscription in the registry that is replayed on every
// (re)connect.
class TopicSink {
 public:
  virtual ~TopicSink() {}
  virtual void add(InTopic kind) = 0;
};

// One module per API (RPC, attributes, OTA, gateway, ...), compiled only when
// used. All methods run on the client task with no library lock held.
class IApiModule {
 public:
  virtual ~IApiModule() {}
  virtual void attach(Client& client) { (void)client; }
  virtual void topics(TopicSink& sink) = 0;
  // Return true if the message was consumed.
  virtual bool handle(const Topic& topic, Bytes payload) = 0;
  // Called on every entry into Online, after all SUBACKs.
  virtual void onOnline() {}
  virtual void tick(uint64_t nowMs) { (void)nowMs; }
  // The client dropped this module (client destroyed or detach()); the
  // module must not use the client afterwards.
  virtual void onDetach() {}
  // True if the destructor calls Client::detach(), as ApiModule's does. The
  // client tells only such modules that it is going away: any other module
  // must outlive the client.
  virtual bool detachesItself() const { return false; }
};

// Base for modules: keeps the client pointer and detaches on destruction,
// so a client and its modules can be destroyed in either order.
class ApiModule : public IApiModule {
 public:
  ~ApiModule() override;
  void attach(Client& client) override { client_ = &client; }
  void onDetach() override { client_ = nullptr; }
  bool detachesItself() const override { return true; }

 protected:
  Client* client_ = nullptr;
};

}  // namespace vl

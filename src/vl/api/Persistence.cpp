#include "vl/api/Persistence.h"

#if VL_ENABLE_PERSISTENCE

#include "vl/Client.h"
#include "vl/Log.h"

namespace vl {

const char* Persistence::Devices::name(uint16_t dev) { return client ? client->deviceName(dev) : nullptr; }

uint16_t Persistence::Devices::index(const char* name) { return client ? client->deviceIndex(name) : kNoDevice; }

void Persistence::attach(Client& client) {
  ApiModule::attach(client);
  devices_.client = &client;
  status_ = journal_.begin(client.outbox(), client.lock(), &devices_);
  if (status_ == Result::Ok) {
    Stats s = client.stats();
    VL_LOGI("outbox store: %lu records restored", static_cast<unsigned long>(s.queuedRecords));
  }
}

void Persistence::onDetach() {
  journal_.end();
  devices_.client = nullptr;
  status_ = Result::NotReady;
  ApiModule::onDetach();
}

}  // namespace vl

#endif  // VL_ENABLE_PERSISTENCE

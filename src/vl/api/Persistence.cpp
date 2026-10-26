#include "vl/api/Persistence.h"

#if VL_ENABLE_PERSISTENCE

#include "vl/Client.h"
#include "vl/Log.h"

namespace vl {

void Persistence::attach(Client& client) {
  ApiModule::attach(client);
  status_ = journal_.begin(client.outbox(), client.lock());
  if (status_ == Result::Ok) {
    Stats s = client.stats();
    VL_LOGI("outbox store: %lu records restored", static_cast<unsigned long>(s.queuedRecords));
  }
}

void Persistence::onDetach() {
  journal_.end();
  status_ = Result::NotReady;
  ApiModule::onDetach();
}

}  // namespace vl

#endif  // VL_ENABLE_PERSISTENCE

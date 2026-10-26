#pragma once

#include "vl/Config.h"

#if VL_ENABLE_PROVISION

#include <stddef.h>
#include <stdint.h>

#include "vl/Client.h"
#include "vl/Credentials.h"
#include "vl/IApiModule.h"
#include "vl/Result.h"
#include "vl/core/JsonReader.h"
#include "vl/pal/KeyValueStore.h"

namespace vl {

// Provisioning ended. On Ok the device now runs its normal session with
// Provisioning::credentials(). Otherwise the client is stopped; error is the
// server's message or a short reason, valid during the call.
typedef void (*ProvisionDoneCb)(Result r, const char* error, void* ctx);

// ThingsBoard device provisioning. start() replaces client.begin(): with
// saved credentials the normal session starts at once; without them a short
// provisioning session (username "provision", only /provision/response
// subscribed) sends the request, saves the credentials it gets back and then
// starts the normal session with them. Other modules and the outbox wait for
// the normal session. Runs on the client task.
class Provisioning : public ApiModule {
 public:
  // store null: nothing is saved, so every boot provisions again.
  explicit Provisioning(IKeyValueStore* store = nullptr) : store_(store) {}

  // The device profile's provision key and secret; name null lets the server
  // name the device. Copied.
  Result setDevice(const char* name, const char* key, const char* secret);
  // Credentials the device brings: AccessToken(token) or MqttBasic(...),
  // copied, or X509() with setCertificate(). Default: the server generates an
  // access token.
  Result setCredentials(const Credentials& c);
  // The device certificate for X509(), sent as PEM. Not copied: it must
  // outlive provisioning. Config::tls must present the same certificate.
  void setCertificate(const char* certPem) { certPem_ = certPem; }
  void onDone(ProvisionDoneCb cb, void* ctx) {
    doneCb_ = cb;
    doneCtx_ = ctx;
  }

  // Starts the client with saved credentials, or provisions first. Call
  // instead of client.begin(). Rejected without setDevice(), or for X509
  // without a certificate and Config::tls.
  Result start(Client& client, const Config& cfg);
  // Erases the saved credentials, e.g. on AuthFailed before start() again.
  Result forget();
  // Saved or provisioned credentials; valid while this object lives.
  const Credentials& credentials() const { return creds_; }

  void topics(TopicSink& sink) override { sink.add(InTopic::ProvisionResponse); }
  bool handle(const Topic& topic, Bytes payload) override;
  void onOnline() override;
  void tick(uint64_t nowMs) override;

 private:
  Credentials view(Credentials::Kind kind) const;
  bool load();
  void save();
  void finish();
  bool takeCredentials(JsonValue response);
  static void writeRequest(JsonWriter& w, void* ctx);

  IKeyValueStore* store_;
  ProvisionDoneCb doneCb_ = nullptr;
  void* doneCtx_ = nullptr;
  const char* certPem_ = nullptr;

  char name_[VL_PROVISION_NAME_SIZE] = {};
  char key_[VL_PROVISION_KEY_SIZE] = {};
  char secret_[VL_PROVISION_KEY_SIZE] = {};
  Credentials::Kind requested_ = Credentials::Kind::None;
  // Requested credentials before the response, then the ones in use.
  Credentials creds_;
  char clientId_[24] = {};
  char username_[VL_USERNAME_SIZE] = {};
  char password_[VL_PASSWORD_SIZE] = {};
  char error_[64] = {};

  bool active_ = false;  // provisioning session running
  bool send_ = false;    // request still to be published on this session
  bool done_ = false;    // response handled; tick() finishes
  Result result_ = Result::Ok;
  uint64_t deadline_ = 0;
};

}  // namespace vl

#endif  // VL_ENABLE_PROVISION

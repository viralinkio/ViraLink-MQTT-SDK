#include "vl/api/Provision.h"

#if VL_ENABLE_PROVISION

#include <string.h>

#include "vl/Log.h"
#include "vl/core/Codec.h"
#include "vl/core/JsonReader.h"

namespace vl {

namespace {

const char kStoreKey[] = "vl_cred";
// 'V', format version, kind, then clientId, username, password, each NUL-terminated.
const uint8_t kBlobVersion = 1;
const size_t kBlobSize = 3 + 24 + VL_USERNAME_SIZE + VL_PASSWORD_SIZE;

bool copyStr(char* dst, size_t cap, const char* src) {
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) return false;
  if (n) memcpy(dst, src, n);
  dst[n] = '\0';
  return true;
}

// Absent stays ""; present must be a string that fits.
bool copyOptional(JsonValue v, char* dst, size_t cap) {
  dst[0] = '\0';
  return !v.valid() || v.copyString(dst, cap);
}

const char* orNull(const char* s) { return s[0] ? s : nullptr; }

}  // namespace

Result Provisioning::setDevice(const char* name, const char* key, const char* secret) {
  if (!key || !key[0] || !secret || !secret[0]) return Result::Rejected;
  if (!copyStr(name_, sizeof(name_), name) || !copyStr(key_, sizeof(key_), key) ||
      !copyStr(secret_, sizeof(secret_), secret)) {
    return Result::TooLarge;
  }
  return Result::Ok;
}

Result Provisioning::setCredentials(const Credentials& c) {
  if (c.kind == Credentials::Kind::AccessToken && (!c.username || !c.username[0])) return Result::Rejected;
  if (c.kind == Credentials::Kind::MqttBasic && !c.username && !c.clientId) return Result::Rejected;
  if (!copyStr(clientId_, sizeof(clientId_), c.clientId) || !copyStr(username_, sizeof(username_), c.username) ||
      !copyStr(password_, sizeof(password_), c.password)) {
    return Result::TooLarge;
  }
  requested_ = c.kind;
  creds_ = view(c.kind);
  return Result::Ok;
}

Credentials Provisioning::view(Credentials::Kind kind) const {
  Credentials c;
  c.kind = kind;
  c.clientId = orNull(clientId_);
  c.username = orNull(username_);
  c.password = orNull(password_);
  return c;
}

Result Provisioning::start(Client& client, const Config& cfg) {
  if (store_ && load()) {
    VL_LOGI("provision: using saved credentials");
    Config c = cfg;
    c.auth = creds_;
    return client.begin(c);
  }
  if (!key_[0]) return Result::Rejected;
  if (requested_ == Credentials::Kind::X509 && (!certPem_ || !cfg.tls)) return Result::Rejected;
  creds_ = view(requested_);
  Result r = client.attach(*this);
  if (r != Result::Ok) return r;
  Config c = cfg;
  c.auth = AccessToken("provision");
  active_ = true;
  send_ = false;
  done_ = false;
  deadline_ = client.uptimeMs() + VL_PROVISION_TIMEOUT_MS;
  r = client.beginExclusive(c, *this);
  if (r != Result::Ok) {
    active_ = false;
    client.detach(*this);
  }
  return r;
}

Result Provisioning::forget() { return store_ ? store_->erase(kStoreKey) : Result::Ok; }

bool Provisioning::handle(const Topic& topic, Bytes payload) {
  if (topic.kind != InTopic::ProvisionResponse) return false;
  if (!active_ || done_) return true;
  JsonValue v = JsonValue::parse(payload);
  JsonValue status = v["status"];
  if (!status.valid()) status = v["provisionDeviceStatus"];  // spelling in older ThingsBoard docs
  done_ = true;
  if (!status.equals("SUCCESS")) {
    result_ = Result::Rejected;
    if (!v["errorMsg"].copyString(error_, sizeof(error_))) copyStr(error_, sizeof(error_), "provisioning refused");
    VL_LOGE("provision: refused: %s", error_);
    return true;
  }
  result_ = takeCredentials(v) ? Result::Ok : Result::Rejected;
  return true;
}

bool Provisioning::takeCredentials(JsonValue response) {
  JsonValue type = response["credentialsType"];
  JsonValue value = response["credentialsValue"];
  Credentials::Kind kind;
  bool ok;
  clientId_[0] = username_[0] = password_[0] = '\0';
  if (type.equals("ACCESS_TOKEN")) {
    kind = Credentials::Kind::AccessToken;
    ok = value.copyString(username_, sizeof(username_)) && username_[0];
  } else if (type.equals("MQTT_BASIC")) {
    kind = Credentials::Kind::MqttBasic;
    ok = copyOptional(value["clientId"], clientId_, sizeof(clientId_)) &&
         copyOptional(value["userName"], username_, sizeof(username_)) &&
         copyOptional(value["password"], password_, sizeof(password_)) && (clientId_[0] || username_[0]);
  } else if (type.equals("X509_CERTIFICATE")) {
    kind = Credentials::Kind::X509;  // the certificate in Config::tls is the credential
    ok = true;
  } else {
    copyStr(error_, sizeof(error_), "unsupported credentials type");
    return false;
  }
  if (!ok) {
    copyStr(error_, sizeof(error_), "invalid credentials");
    return false;
  }
  creds_ = view(kind);
  return true;
}

void Provisioning::onOnline() {
  if (active_) send_ = true;
}

void Provisioning::tick(uint64_t nowMs) {
  if (!active_ || !client_) return;
  if (!done_ && nowMs >= deadline_) {
    result_ = Result::Timeout;
    copyStr(error_, sizeof(error_), "no response");
    done_ = true;
  }
  if (done_) {
    finish();
    return;
  }
  if (!send_) return;
  Result r = client_->publishNow(OutTopic::ProvisionRequest, 0, writeRequest, this);
  if (r == Result::Ok) {
    send_ = false;
    VL_LOGI("provision: request sent");
  } else if (r != Result::NotReady) {
    result_ = r;
    copyStr(error_, sizeof(error_), "request too large");
    done_ = true;
  }
}

void Provisioning::finish() {
  active_ = false;
  Client* c = client_;
  Result r = result_;
  // The normal session must not subscribe to /provision/response. Detaching
  // here makes the client skip the next module's tick once; harmless.
  c->detach(*this);
  if (r == Result::Ok) {
    save();
    VL_LOGI("provision: done, starting the session");
    c->setCredentials(creds_);
  } else {
    c->end();
  }
  if (doneCb_) doneCb_(r, r == Result::Ok ? nullptr : error_, doneCtx_);
}

void Provisioning::writeRequest(JsonWriter& w, void* ctx) {
  Provisioning* p = static_cast<Provisioning*>(ctx);
  codec::provisionRequest(w, orNull(p->name_), p->key_, p->secret_, p->view(p->requested_), p->certPem_);
}

bool Provisioning::load() {
  uint8_t blob[kBlobSize];
  size_t n = 0;
  if (store_->get(kStoreKey, blob, sizeof(blob), n) != Result::Ok || n < 6) return false;
  if (blob[0] != 'V' || blob[1] != kBlobVersion) return false;
  uint8_t kind = blob[2];
  if (kind < static_cast<uint8_t>(Credentials::Kind::AccessToken) || kind > static_cast<uint8_t>(Credentials::Kind::X509)) {
    return false;
  }
  char* dst[3] = {clientId_, username_, password_};
  const size_t caps[3] = {sizeof(clientId_), sizeof(username_), sizeof(password_)};
  size_t len[3];
  // Validate everything before touching the buffers, which hold the requested
  // credentials until a load succeeds.
  const uint8_t* p = blob + 3;
  const uint8_t* end = blob + n;
  for (int i = 0; i < 3; ++i) {
    const void* nul = memchr(p, 0, static_cast<size_t>(end - p));
    if (!nul) return false;
    len[i] = static_cast<size_t>(static_cast<const uint8_t*>(nul) - p);
    if (len[i] >= caps[i]) return false;
    p += len[i] + 1;
  }
  if (kind == static_cast<uint8_t>(Credentials::Kind::AccessToken) && len[1] == 0) return false;
  p = blob + 3;
  for (int i = 0; i < 3; ++i) {
    memcpy(dst[i], p, len[i] + 1);
    p += len[i] + 1;
  }
  creds_ = view(static_cast<Credentials::Kind>(kind));
  return true;
}

void Provisioning::save() {
  if (!store_) return;
  uint8_t blob[kBlobSize];
  size_t n = 0;
  blob[n++] = 'V';
  blob[n++] = kBlobVersion;
  blob[n++] = static_cast<uint8_t>(creds_.kind);
  const char* parts[3] = {clientId_, username_, password_};
  for (int i = 0; i < 3; ++i) {
    size_t len = strlen(parts[i]) + 1;
    memcpy(blob + n, parts[i], len);
    n += len;
  }
  Result r = store_->set(kStoreKey, blob, n);
  // The session still starts; the next boot provisions again.
  if (r != Result::Ok) VL_LOGE("provision: credentials not saved: %s", toString(r));
}

}  // namespace vl

#endif  // VL_ENABLE_PROVISION

#pragma once

#include <stdint.h>

namespace vl {

struct Credentials {
  enum class Kind : uint8_t { None, AccessToken, MqttBasic, X509 };
  Kind kind = Kind::None;
  const char* clientId = nullptr;
  const char* username = nullptr;
  const char* password = nullptr;
};

inline Credentials AccessToken(const char* token) {
  Credentials c;
  c.kind = Credentials::Kind::AccessToken;
  c.username = token;
  return c;
}

inline Credentials MqttBasic(const char* clientId, const char* username, const char* password) {
  Credentials c;
  c.kind = Credentials::Kind::MqttBasic;
  c.clientId = clientId;
  c.username = username;
  c.password = password;
  return c;
}

// Device identified by its TLS client certificate (set Config::tls).
inline Credentials X509() {
  Credentials c;
  c.kind = Credentials::Kind::X509;
  return c;
}

}  // namespace vl

// Arduino's network Client interface, as far as the library calls it.
#pragma once

#include <Arduino.h>

class Client {
 public:
  virtual ~Client() {}
  virtual int connect(const char* host, uint16_t port) = 0;
  virtual size_t write(const uint8_t* buf, size_t size) = 0;
  virtual int available() = 0;
  virtual int read(uint8_t* buf, size_t size) = 0;
  virtual void stop() = 0;
  virtual uint8_t connected() = 0;
};

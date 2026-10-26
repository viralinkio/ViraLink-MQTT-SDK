#pragma once

#include <stddef.h>
#include <string.h>

#include "vl/Config.h"
#include "vl/Result.h"
#include "vl/core/JsonReader.h"

namespace vl {

// Fixed registry of named RPC methods, shared by Rpc and Gateway.
template <class Fn>
class MethodTable {
 public:
  struct Entry {
    char name[VL_RPC_METHOD_SIZE];
    Fn fn;
    void* ctx;
  };

  MethodTable(Entry* table, size_t cap) : table_(table), cap_(cap) {}

  // Registers or replaces the handler for name; the name is copied.
  Result add(const char* name, Fn fn, void* ctx) {
    if (!name || !fn) return Result::Rejected;
    size_t n = strlen(name);
    if (n == 0 || n >= VL_RPC_METHOD_SIZE) return Result::Rejected;
    for (size_t i = 0; i < count_; ++i) {
      if (strcmp(table_[i].name, name) == 0) {
        table_[i].fn = fn;
        table_[i].ctx = ctx;
        return Result::Ok;
      }
    }
    if (count_ >= cap_) return Result::NoMemory;
    Entry& e = table_[count_++];
    memcpy(e.name, name, n + 1);
    e.fn = fn;
    e.ctx = ctx;
    return Result::Ok;
  }

  // Looks up the method named by a JSON string and copies the name into name
  // (VL_RPC_METHOD_SIZE bytes). A name that does not fit cannot be registered:
  // name is then "" and the result null.
  const Entry* find(JsonValue method, char* name) const {
    if (!method.copyString(name, VL_RPC_METHOD_SIZE)) {
      name[0] = '\0';
      return nullptr;
    }
    for (size_t i = 0; i < count_; ++i) {
      if (strcmp(table_[i].name, name) == 0) return &table_[i];
    }
    return nullptr;
  }

 private:
  Entry* table_;
  size_t cap_;
  size_t count_ = 0;
};

}  // namespace vl

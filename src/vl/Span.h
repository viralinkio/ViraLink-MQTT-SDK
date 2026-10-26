#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {

// Non-owning view of a contiguous range. Valid only for the duration of the
// call it is passed to.
template <class T>
class Span {
 public:
  Span() : ptr_(nullptr), size_(0) {}
  Span(T* p, size_t n) : ptr_(p), size_(n) {}

  T* data() const { return ptr_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  T& operator[](size_t i) const { return ptr_[i]; }
  T* begin() const { return ptr_; }
  T* end() const { return ptr_ + size_; }

 private:
  T* ptr_;
  size_t size_;
};

typedef Span<const uint8_t> Bytes;

}  // namespace vl

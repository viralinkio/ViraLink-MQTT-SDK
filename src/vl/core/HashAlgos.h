// Streaming digests behind Hasher (Hash.h). Each state is plain data so the
// Hasher can keep them in one union.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace vl {
namespace hash {

struct Md5State {
  uint32_t h[4];
  uint64_t len;
  uint8_t buf[64];
};
void md5Init(Md5State& s);
void md5Update(Md5State& s, const uint8_t* p, size_t n);
void md5Final(Md5State& s, uint8_t out[16]);

struct Sha256State {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
};
void sha256Init(Sha256State& s);
void sha256Update(Sha256State& s, const uint8_t* p, size_t n);
void sha256Final(Sha256State& s, uint8_t out[32]);

// SHA-512, or SHA-384 (other initial values, digest cut to 48 bytes).
struct Sha512State {
  uint64_t h[8];
  uint64_t len;  // bytes; firmware images never reach 2^61
  uint8_t buf[128];
};
void sha512Init(Sha512State& s, bool sha384);
void sha512Update(Sha512State& s, const uint8_t* p, size_t n);
void sha512Final(Sha512State& s, uint8_t out[64]);

uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n);  // start with 0xFFFFFFFF, finish with ~

// MurmurHash3 x86_32 and x64_128, seed 0 (Guava murmur3_32 / murmur3_128).
struct Murmur32State {
  uint32_t h;
  uint32_t len;
  uint8_t tail[4];
};
void murmur32Init(Murmur32State& s);
void murmur32Update(Murmur32State& s, const uint8_t* p, size_t n);
uint32_t murmur32Final(Murmur32State& s);

struct Murmur128State {
  uint64_t h1;
  uint64_t h2;
  uint64_t len;
  uint8_t tail[16];
};
void murmur128Init(Murmur128State& s);
void murmur128Update(Murmur128State& s, const uint8_t* p, size_t n);
void murmur128Final(Murmur128State& s, uint64_t& h1, uint64_t& h2);

}  // namespace hash
}  // namespace vl

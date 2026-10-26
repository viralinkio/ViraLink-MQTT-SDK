#include "vl/core/Hash.h"

#include <string.h>

namespace vl {

namespace {

struct AlgoName {
  const char* name;
  HashAlgo algo;
};
const AlgoName kAlgos[] = {
    {"MD5", HashAlgo::Md5},           {"SHA256", HashAlgo::Sha256},         {"SHA384", HashAlgo::Sha384},
    {"SHA512", HashAlgo::Sha512},     {"CRC32", HashAlgo::Crc32},           {"MURMUR3_32", HashAlgo::Murmur3_32},
    {"MURMUR3_128", HashAlgo::Murmur3_128},
};

size_t toHex(const uint8_t* d, size_t n, char* out) {
  static const char kDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = kDigits[d[i] >> 4];
    out[2 * i + 1] = kDigits[d[i] & 15];
  }
  out[2 * n] = '\0';
  return 2 * n;
}

void putLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

void putLe64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

}  // namespace

bool parseHashAlgo(const char* name, HashAlgo& out) {
  if (!name) return false;
  for (size_t i = 0; i < sizeof(kAlgos) / sizeof(kAlgos[0]); ++i) {
    if (strcmp(name, kAlgos[i].name) == 0) {
      out = kAlgos[i].algo;
      return true;
    }
  }
  return false;
}

void Hasher::begin(HashAlgo algo) {
  algo_ = algo;
  switch (algo) {
    case HashAlgo::Md5:
      hash::md5Init(s_.md5);
      break;
    case HashAlgo::Sha256:
      hash::sha256Init(s_.sha256);
      break;
    case HashAlgo::Sha384:
    case HashAlgo::Sha512:
      hash::sha512Init(s_.sha512, algo == HashAlgo::Sha384);
      break;
    case HashAlgo::Crc32:
      s_.crc32 = 0xFFFFFFFFul;
      break;
    case HashAlgo::Murmur3_32:
      hash::murmur32Init(s_.m32);
      break;
    case HashAlgo::Murmur3_128:
      hash::murmur128Init(s_.m128);
      break;
  }
}

void Hasher::update(const uint8_t* p, size_t n) {
  switch (algo_) {
    case HashAlgo::Md5:
      hash::md5Update(s_.md5, p, n);
      break;
    case HashAlgo::Sha256:
      hash::sha256Update(s_.sha256, p, n);
      break;
    case HashAlgo::Sha384:
    case HashAlgo::Sha512:
      hash::sha512Update(s_.sha512, p, n);
      break;
    case HashAlgo::Crc32:
      s_.crc32 = hash::crc32Update(s_.crc32, p, n);
      break;
    case HashAlgo::Murmur3_32:
      hash::murmur32Update(s_.m32, p, n);
      break;
    case HashAlgo::Murmur3_128:
      hash::murmur128Update(s_.m128, p, n);
      break;
  }
}

size_t Hasher::hex(char* out) {
  uint8_t d[64];
  switch (algo_) {
    case HashAlgo::Md5:
      hash::md5Final(s_.md5, d);
      return toHex(d, 16, out);
    case HashAlgo::Sha256:
      hash::sha256Final(s_.sha256, d);
      return toHex(d, 32, out);
    case HashAlgo::Sha384:
      hash::sha512Final(s_.sha512, d);
      return toHex(d, 48, out);
    case HashAlgo::Sha512:
      hash::sha512Final(s_.sha512, d);
      return toHex(d, 64, out);
    case HashAlgo::Crc32:
      putLe32(d, ~s_.crc32);
      return toHex(d, 4, out);
    case HashAlgo::Murmur3_32:
      putLe32(d, hash::murmur32Final(s_.m32));
      return toHex(d, 4, out);
    case HashAlgo::Murmur3_128: {
      uint64_t h1, h2;
      hash::murmur128Final(s_.m128, h1, h2);
      putLe64(d, h1);
      putLe64(d + 8, h2);
      return toHex(d, 16, out);
    }
  }
  out[0] = '\0';
  return 0;
}

namespace hash {

namespace {

// Reflected polynomial 0xEDB88320, four bits per step: a 64-byte table
// instead of 1 KB.
const uint32_t kCrcNibble[16] = {
    0x00000000ul, 0x1DB71064ul, 0x3B6E20C8ul, 0x26D930ACul, 0x76DC4190ul, 0x6B6B51F4ul, 0x4DB26158ul, 0x5005713Cul,
    0xEDB88320ul, 0xF00F9344ul, 0xD6D6A3E8ul, 0xCB61B38Cul, 0x9B64C2B0ul, 0x86D3D2D4ul, 0xA00AE278ul, 0xBDBDF21Cul,
};

uint32_t rotl32(uint32_t x, unsigned r) { return (x << r) | (x >> (32 - r)); }
uint64_t rotl64(uint64_t x, unsigned r) { return (x << r) | (x >> (64 - r)); }

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

uint64_t le64(const uint8_t* p) { return static_cast<uint64_t>(le32(p)) | static_cast<uint64_t>(le32(p + 4)) << 32; }

const uint32_t kM32C1 = 0xcc9e2d51ul;
const uint32_t kM32C2 = 0x1b873593ul;

uint32_t m32Mix(uint32_t k) {
  k *= kM32C1;
  k = rotl32(k, 15);
  return k * kM32C2;
}

const uint64_t kM128C1 = 0x87c37b91114253d5ull;
const uint64_t kM128C2 = 0x4cf5ad432745937full;

uint64_t m128MixK1(uint64_t k) {
  k *= kM128C1;
  k = rotl64(k, 31);
  return k * kM128C2;
}

uint64_t m128MixK2(uint64_t k) {
  k *= kM128C2;
  k = rotl64(k, 33);
  return k * kM128C1;
}

uint64_t fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdull;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ull;
  k ^= k >> 33;
  return k;
}

void m128Block(Murmur128State& s, const uint8_t* p) {
  s.h1 ^= m128MixK1(le64(p));
  s.h1 = rotl64(s.h1, 27) + s.h2;
  s.h1 = s.h1 * 5 + 0x52dce729u;
  s.h2 ^= m128MixK2(le64(p + 8));
  s.h2 = rotl64(s.h2, 31) + s.h1;
  s.h2 = s.h2 * 5 + 0x38495ab5u;
}

}  // namespace

uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    crc = (crc >> 4) ^ kCrcNibble[crc & 15];
    crc = (crc >> 4) ^ kCrcNibble[crc & 15];
  }
  return crc;
}

void murmur32Init(Murmur32State& s) {
  s.h = 0;
  s.len = 0;
}

void murmur32Update(Murmur32State& s, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    s.tail[s.len & 3] = p[i];
    ++s.len;
    if ((s.len & 3) != 0) continue;
    s.h ^= m32Mix(le32(s.tail));
    s.h = rotl32(s.h, 13);
    s.h = s.h * 5 + 0xe6546b64ul;
  }
}

uint32_t murmur32Final(Murmur32State& s) {
  uint32_t k = 0;
  switch (s.len & 3) {
    case 3:
      k ^= static_cast<uint32_t>(s.tail[2]) << 16;
      // fall through
    case 2:
      k ^= static_cast<uint32_t>(s.tail[1]) << 8;
      // fall through
    case 1:
      k ^= s.tail[0];
      s.h ^= m32Mix(k);
      break;
    default:
      break;
  }
  uint32_t h = s.h ^ s.len;
  h ^= h >> 16;
  h *= 0x85ebca6bul;
  h ^= h >> 13;
  h *= 0xc2b2ae35ul;
  h ^= h >> 16;
  return h;
}

void murmur128Init(Murmur128State& s) {
  s.h1 = 0;
  s.h2 = 0;
  s.len = 0;
}

void murmur128Update(Murmur128State& s, const uint8_t* p, size_t n) {
  size_t used = static_cast<size_t>(s.len & 15);
  s.len += n;
  if (used) {
    size_t take = 16 - used < n ? 16 - used : n;
    memcpy(s.tail + used, p, take);
    p += take;
    n -= take;
    if (used + take < 16) return;
    m128Block(s, s.tail);
  }
  for (; n >= 16; p += 16, n -= 16) m128Block(s, p);
  memcpy(s.tail, p, n);
}

void murmur128Final(Murmur128State& s, uint64_t& h1, uint64_t& h2) {
  size_t rem = static_cast<size_t>(s.len & 15);
  uint64_t k1 = 0, k2 = 0;
  for (size_t i = rem; i-- > 8;) k2 ^= static_cast<uint64_t>(s.tail[i]) << (8 * (i - 8));
  for (size_t i = rem < 8 ? rem : 8; i-- > 0;) k1 ^= static_cast<uint64_t>(s.tail[i]) << (8 * i);
  if (rem > 8) s.h2 ^= m128MixK2(k2);
  if (rem > 0) s.h1 ^= m128MixK1(k1);
  h1 = s.h1 ^ s.len;
  h2 = s.h2 ^ s.len;
  h1 += h2;
  h2 += h1;
  h1 = fmix64(h1);
  h2 = fmix64(h2);
  h1 += h2;
  h2 += h1;
}

}  // namespace hash
}  // namespace vl

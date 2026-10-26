// MD5 (RFC 1321). Kept only because ThingsBoard offers it and 1.0.2 devices
// accept nothing else; never used for anything but image checksums.
#include <string.h>

#include "vl/core/HashAlgos.h"

namespace vl {
namespace hash {

namespace {

const uint32_t kK[64] = {
    0xd76aa478ul, 0xe8c7b756ul, 0x242070dbul, 0xc1bdceeeul, 0xf57c0faful, 0x4787c62aul, 0xa8304613ul, 0xfd469501ul,
    0x698098d8ul, 0x8b44f7aful, 0xffff5bb1ul, 0x895cd7beul, 0x6b901122ul, 0xfd987193ul, 0xa679438eul, 0x49b40821ul,
    0xf61e2562ul, 0xc040b340ul, 0x265e5a51ul, 0xe9b6c7aaul, 0xd62f105dul, 0x02441453ul, 0xd8a1e681ul, 0xe7d3fbc8ul,
    0x21e1cde6ul, 0xc33707d6ul, 0xf4d50d87ul, 0x455a14edul, 0xa9e3e905ul, 0xfcefa3f8ul, 0x676f02d9ul, 0x8d2a4c8aul,
    0xfffa3942ul, 0x8771f681ul, 0x6d9d6122ul, 0xfde5380cul, 0xa4beea44ul, 0x4bdecfa9ul, 0xf6bb4b60ul, 0xbebfbc70ul,
    0x289b7ec6ul, 0xeaa127faul, 0xd4ef3085ul, 0x04881d05ul, 0xd9d4d039ul, 0xe6db99e5ul, 0x1fa27cf8ul, 0xc4ac5665ul,
    0xf4292244ul, 0x432aff97ul, 0xab9423a7ul, 0xfc93a039ul, 0x655b59c3ul, 0x8f0ccc92ul, 0xffeff47dul, 0x85845dd1ul,
    0x6fa87e4ful, 0xfe2ce6e0ul, 0xa3014314ul, 0x4e0811a1ul, 0xf7537e82ul, 0xbd3af235ul, 0x2ad7d2bbul, 0xeb86d391ul,
};
const uint8_t kShift[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

uint32_t rotl(uint32_t x, unsigned r) { return (x << r) | (x >> (32 - r)); }

void block(uint32_t h[4], const uint8_t* p) {
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = static_cast<uint32_t>(p[4 * i]) | static_cast<uint32_t>(p[4 * i + 1]) << 8 |
           static_cast<uint32_t>(p[4 * i + 2]) << 16 | static_cast<uint32_t>(p[4 * i + 3]) << 24;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  for (unsigned i = 0; i < 64; ++i) {
    uint32_t f;
    unsigned g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    uint32_t t = d;
    d = c;
    c = b;
    b += rotl(a + f + kK[i] + m[g], kShift[(i / 16) * 4 + (i & 3)]);
    a = t;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
}

}  // namespace

void md5Init(Md5State& s) {
  s.h[0] = 0x67452301ul;
  s.h[1] = 0xefcdab89ul;
  s.h[2] = 0x98badcfeul;
  s.h[3] = 0x10325476ul;
  s.len = 0;
}

void md5Update(Md5State& s, const uint8_t* p, size_t n) {
  size_t used = static_cast<size_t>(s.len & 63);
  s.len += n;
  if (used) {
    size_t take = 64 - used < n ? 64 - used : n;
    memcpy(s.buf + used, p, take);
    p += take;
    n -= take;
    if (used + take < 64) return;
    block(s.h, s.buf);
  }
  for (; n >= 64; p += 64, n -= 64) block(s.h, p);
  memcpy(s.buf, p, n);
}

void md5Final(Md5State& s, uint8_t out[16]) {
  static const uint8_t kPad[64] = {0x80};
  uint64_t bits = s.len * 8;
  size_t used = static_cast<size_t>(s.len & 63);
  md5Update(s, kPad, used < 56 ? 56 - used : 120 - used);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (8 * i));
  md5Update(s, len, 8);
  for (int i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(s.h[i / 4] >> (8 * (i & 3)));
}

}  // namespace hash
}  // namespace vl

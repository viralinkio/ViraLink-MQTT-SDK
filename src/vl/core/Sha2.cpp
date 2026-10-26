// SHA-256, SHA-384 and SHA-512 (FIPS 180-4). The message schedule is a
// rolling 16-word window, so a block needs 64 or 128 bytes of stack rather
// than 256 or 640.
#include <string.h>

#include "vl/core/HashAlgos.h"

namespace vl {
namespace hash {

namespace {

const uint8_t kPad[128] = {0x80};

const uint32_t kK256[64] = {
    0x428a2f98ul, 0x71374491ul, 0xb5c0fbcful, 0xe9b5dba5ul, 0x3956c25bul, 0x59f111f1ul, 0x923f82a4ul, 0xab1c5ed5ul,
    0xd807aa98ul, 0x12835b01ul, 0x243185beul, 0x550c7dc3ul, 0x72be5d74ul, 0x80deb1feul, 0x9bdc06a7ul, 0xc19bf174ul,
    0xe49b69c1ul, 0xefbe4786ul, 0x0fc19dc6ul, 0x240ca1ccul, 0x2de92c6ful, 0x4a7484aaul, 0x5cb0a9dcul, 0x76f988daul,
    0x983e5152ul, 0xa831c66dul, 0xb00327c8ul, 0xbf597fc7ul, 0xc6e00bf3ul, 0xd5a79147ul, 0x06ca6351ul, 0x14292967ul,
    0x27b70a85ul, 0x2e1b2138ul, 0x4d2c6dfcul, 0x53380d13ul, 0x650a7354ul, 0x766a0abbul, 0x81c2c92eul, 0x92722c85ul,
    0xa2bfe8a1ul, 0xa81a664bul, 0xc24b8b70ul, 0xc76c51a3ul, 0xd192e819ul, 0xd6990624ul, 0xf40e3585ul, 0x106aa070ul,
    0x19a4c116ul, 0x1e376c08ul, 0x2748774cul, 0x34b0bcb5ul, 0x391c0cb3ul, 0x4ed8aa4aul, 0x5b9cca4ful, 0x682e6ff3ul,
    0x748f82eeul, 0x78a5636ful, 0x84c87814ul, 0x8cc70208ul, 0x90befffaul, 0xa4506cebul, 0xbef9a3f7ul, 0xc67178f2ul,
};

const uint64_t kK512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull,
    0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull, 0x983e5152ee66dfabull,
    0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull,
    0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull, 0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull,
    0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull, 0xca273eceea26619cull,
    0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull, 0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

const uint64_t kInit384[8] = {
    0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull,
    0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull,
};
const uint64_t kInit512[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

uint32_t rotr32(uint32_t x, unsigned r) { return (x >> r) | (x << (32 - r)); }
uint64_t rotr64(uint64_t x, unsigned r) { return (x >> r) | (x << (64 - r)); }

void block256(uint32_t h[8], const uint8_t* p) {
  uint32_t w[16];
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (unsigned i = 0; i < 64; ++i) {
    if (i < 16) {
      w[i] = static_cast<uint32_t>(p[4 * i]) << 24 | static_cast<uint32_t>(p[4 * i + 1]) << 16 |
             static_cast<uint32_t>(p[4 * i + 2]) << 8 | static_cast<uint32_t>(p[4 * i + 3]);
    } else {
      uint32_t w15 = w[(i - 15) & 15], w2 = w[(i - 2) & 15];
      uint32_t s0 = rotr32(w15, 7) ^ rotr32(w15, 18) ^ (w15 >> 3);
      uint32_t s1 = rotr32(w2, 17) ^ rotr32(w2, 19) ^ (w2 >> 10);
      w[i & 15] += s0 + w[(i - 7) & 15] + s1;
    }
    uint32_t t1 = k + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) + ((e & f) ^ (~e & g)) + kK256[i] + w[i & 15];
    uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += k;
}

void block512(uint64_t h[8], const uint8_t* p) {
  uint64_t w[16];
  uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (unsigned i = 0; i < 80; ++i) {
    if (i < 16) {
      uint64_t v = 0;
      for (unsigned j = 0; j < 8; ++j) v = v << 8 | p[8 * i + j];
      w[i] = v;
    } else {
      uint64_t w15 = w[(i - 15) & 15], w2 = w[(i - 2) & 15];
      uint64_t s0 = rotr64(w15, 1) ^ rotr64(w15, 8) ^ (w15 >> 7);
      uint64_t s1 = rotr64(w2, 19) ^ rotr64(w2, 61) ^ (w2 >> 6);
      w[i & 15] += s0 + w[(i - 7) & 15] + s1;
    }
    uint64_t t1 = k + (rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41)) + ((e & f) ^ (~e & g)) + kK512[i] + w[i & 15];
    uint64_t t2 = (rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += k;
}

}  // namespace

void sha256Init(Sha256State& s) {
  static const uint32_t kInit[8] = {0x6a09e667ul, 0xbb67ae85ul, 0x3c6ef372ul, 0xa54ff53aul,
                                    0x510e527ful, 0x9b05688cul, 0x1f83d9abul, 0x5be0cd19ul};
  memcpy(s.h, kInit, sizeof(kInit));
  s.len = 0;
}

void sha256Update(Sha256State& s, const uint8_t* p, size_t n) {
  size_t used = static_cast<size_t>(s.len & 63);
  s.len += n;
  if (used) {
    size_t take = 64 - used < n ? 64 - used : n;
    memcpy(s.buf + used, p, take);
    p += take;
    n -= take;
    if (used + take < 64) return;
    block256(s.h, s.buf);
  }
  for (; n >= 64; p += 64, n -= 64) block256(s.h, p);
  memcpy(s.buf, p, n);
}

void sha256Final(Sha256State& s, uint8_t out[32]) {
  uint64_t bits = s.len * 8;
  size_t used = static_cast<size_t>(s.len & 63);
  sha256Update(s, kPad, used < 56 ? 56 - used : 120 - used);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  sha256Update(s, len, 8);
  for (int i = 0; i < 32; ++i) out[i] = static_cast<uint8_t>(s.h[i / 4] >> (24 - 8 * (i & 3)));
}

void sha512Init(Sha512State& s, bool sha384) {
  memcpy(s.h, sha384 ? kInit384 : kInit512, sizeof(s.h));
  s.len = 0;
}

void sha512Update(Sha512State& s, const uint8_t* p, size_t n) {
  size_t used = static_cast<size_t>(s.len & 127);
  s.len += n;
  if (used) {
    size_t take = 128 - used < n ? 128 - used : n;
    memcpy(s.buf + used, p, take);
    p += take;
    n -= take;
    if (used + take < 128) return;
    block512(s.h, s.buf);
  }
  for (; n >= 128; p += 128, n -= 128) block512(s.h, p);
  memcpy(s.buf, p, n);
}

void sha512Final(Sha512State& s, uint8_t out[64]) {
  uint64_t bits = s.len * 8;
  size_t used = static_cast<size_t>(s.len & 127);
  sha512Update(s, kPad, used < 112 ? 112 - used : 240 - used);
  uint8_t len[16] = {};  // 128-bit length; the high half is always 0 here
  for (int i = 0; i < 8; ++i) len[8 + i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  sha512Update(s, len, 16);
  for (int i = 0; i < 64; ++i) out[i] = static_cast<uint8_t>(s.h[i / 8] >> (56 - 8 * (i & 7)));
}

}  // namespace hash
}  // namespace vl

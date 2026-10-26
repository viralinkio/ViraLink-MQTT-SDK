// OTA checksums against golden values from Guava 33.6 (the hash library
// ThingsBoard's BaseOtaPackageService uses), fed whole and in pieces.
#include <string.h>
#include <unity.h>

#include <string>

#include "vl/core/Hash.h"

using namespace vl;

void setUp() {}
void tearDown() {}

namespace {

const char* const kFox = "The quick brown fox jumps over the lazy dog";

struct Golden {
  const char* algo;
  const char* empty;
  const char* abc;
  const char* fox;
  const char* big;  // bigInput()
};

const Golden kGolden[] = {
    {"MD5", "d41d8cd98f00b204e9800998ecf8427e", "900150983cd24fb0d6963f7d28e17f72", "9e107d9d372bb6826bd81d3542a419d6",
     "2b1e78d5765de9e10495a01412a1cf22"},
    {"SHA256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
     "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
     "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
     "5097e7d587352f5097062ae679f37bda5802d9f875aba14c8cb4d1a188ada179"},
    {"SHA384", "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b",
     "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
     "ca737f1014a48f4c0b6dd43cb177b0afd9e5169367544c494011e3317dbf9a509cb1e5dc1e85a941bbee3d7f2afbc9b1",
     "4f33e6bdc22d2129245d832f3b149770b799aa9e63b4c79bd3073aca7e5afc8332c83cace1f5d81c22a256f09dcb3f98"},
    {"SHA512",
     "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a5"
     "38327af927da3e",
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a"
     "9ac94fa54ca49f",
     "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb642e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0"
     "538f3db854fee6",
     "b41d42e106eca6bf57123566b7ed1550c37d33af23afbfa8e302dfd44c988b3003a26b4140ef42f535e66ea424e7c4a31616307269e6d520a6"
     "508f726da23d0a"},
    // Guava prints CRC32 and Murmur3 little-endian: zlib's crc32("abc") is 352441c2.
    {"CRC32", "00000000", "c2412435", "39a34f41", "1e160289"},
    {"MURMUR3_32", "00000000", "fa93ddb3", "23f74f2e", "d8b98517"},
    {"MURMUR3_128", "00000000000000000000000000000000", "6778ad3f3f3f96b4522dca264174a23b",
     "6c1b07bc7bbc4be347939ac4a93c437a", "45c99d896b9dfe1729ce34cea6aaa7d3"},
};

std::string bigInput() {
  std::string s(1000, '\0');
  for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(i * 31 + 7);
  return s;
}

// Hashes data fed in pieces of `step` bytes.
std::string digest(const char* algo, const std::string& data, size_t step) {
  HashAlgo a;
  TEST_ASSERT_TRUE_MESSAGE(parseHashAlgo(algo, a), algo);
  Hasher h;
  h.begin(a);
  for (size_t i = 0; i < data.size(); i += step) {
    size_t n = data.size() - i < step ? data.size() - i : step;
    h.update(reinterpret_cast<const uint8_t*>(data.data() + i), n);
  }
  char out[Hasher::kHexSize];
  size_t n = h.hex(out);
  TEST_ASSERT_EQUAL_UINT(strlen(out), n);
  return out;
}

}  // namespace

static void test_golden_values() {
  for (size_t i = 0; i < sizeof(kGolden) / sizeof(kGolden[0]); ++i) {
    const Golden& g = kGolden[i];
    TEST_ASSERT_EQUAL_STRING_MESSAGE(g.empty, digest(g.algo, "", 1).c_str(), g.algo);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(g.abc, digest(g.algo, "abc", 3).c_str(), g.algo);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(g.fox, digest(g.algo, kFox, strlen(kFox)).c_str(), g.algo);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(g.big, digest(g.algo, bigInput(), 1000).c_str(), g.algo);
  }
}

// Pieces that straddle every block size (4, 16, 64, 128 bytes) give the same
// digest as one update.
static void test_streaming_matches_one_shot() {
  const size_t steps[] = {1, 3, 15, 16, 17, 63, 64, 65, 127, 128, 129};
  std::string big = bigInput();
  for (size_t i = 0; i < sizeof(kGolden) / sizeof(kGolden[0]); ++i) {
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); ++s) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE(kGolden[i].big, digest(kGolden[i].algo, big, steps[s]).c_str(), kGolden[i].algo);
    }
  }
}

static void test_begin_resets() {
  HashAlgo a;
  TEST_ASSERT_TRUE(parseHashAlgo("SHA256", a));
  Hasher h;
  h.begin(a);
  h.update(reinterpret_cast<const uint8_t*>("junk"), 4);
  h.begin(a);
  h.update(reinterpret_cast<const uint8_t*>("abc"), 3);
  char out[Hasher::kHexSize];
  TEST_ASSERT_EQUAL_UINT(64, h.hex(out));
  TEST_ASSERT_EQUAL_STRING(kGolden[1].abc, out);
}

static void test_algorithm_names() {
  HashAlgo a = HashAlgo::Md5;
  TEST_ASSERT_TRUE(parseHashAlgo("MURMUR3_128", a));
  TEST_ASSERT_TRUE(a == HashAlgo::Murmur3_128);
  TEST_ASSERT_TRUE(parseHashAlgo("CRC32", a));
  TEST_ASSERT_TRUE(a == HashAlgo::Crc32);
  TEST_ASSERT_FALSE(parseHashAlgo("sha256", a));  // ThingsBoard sends the enum name
  TEST_ASSERT_FALSE(parseHashAlgo("SHA1", a));
  TEST_ASSERT_FALSE(parseHashAlgo("", a));
  TEST_ASSERT_FALSE(parseHashAlgo(nullptr, a));
  TEST_ASSERT_TRUE(a == HashAlgo::Crc32);  // untouched on failure
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_golden_values);
  RUN_TEST(test_streaming_matches_one_shot);
  RUN_TEST(test_begin_resets);
  RUN_TEST(test_algorithm_names);
  return UNITY_END();
}

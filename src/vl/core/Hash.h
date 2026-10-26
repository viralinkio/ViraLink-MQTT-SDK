// Checksums for OTA images, computed while the image streams into flash.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "vl/core/HashAlgos.h"

namespace vl {

// The algorithms ThingsBoard offers for an OTA package (fw_checksum_algorithm).
enum class HashAlgo : uint8_t { Md5, Sha256, Sha384, Sha512, Crc32, Murmur3_32, Murmur3_128 };

// "MD5", "SHA256", "SHA384", "SHA512", "CRC32", "MURMUR3_32", "MURMUR3_128".
// False if name is null or unknown.
bool parseHashAlgo(const char* name, HashAlgo& out);

class Hasher {
 public:
  // SHA-512 as hex plus the NUL.
  static const size_t kHexSize = 129;

  void begin(HashAlgo algo);
  void update(const uint8_t* p, size_t n);
  // Ends the hash and writes the lowercase hex digest the way ThingsBoard
  // prints it (Guava HashCode.toString(): digest bytes in order, so CRC32 and
  // Murmur3 come out little-endian). out needs kHexSize bytes. Returns the
  // number of hex characters. Call begin() before hashing again.
  size_t hex(char* out);

 private:
  HashAlgo algo_ = HashAlgo::Md5;
  union State {
    hash::Md5State md5;
    hash::Sha256State sha256;
    hash::Sha512State sha512;
    uint32_t crc32;
    hash::Murmur32State m32;
    hash::Murmur128State m128;
  } s_;
};

}  // namespace vl

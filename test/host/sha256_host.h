#pragma once
// Host-test stand-in for mbedtls_sha256 using OpenSSL.
#include <openssl/sha.h>
static inline int mbedtls_sha256(const unsigned char* in, size_t len, unsigned char* out, int) {
  SHA256(in, len, out);
  return 0;
}

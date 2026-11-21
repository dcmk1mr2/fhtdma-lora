#include "hopseq.h"
#include <string.h>
#include <ctype.h>

#ifdef ARDUINO
#include <mbedtls/sha256.h>
#else
#include "sha256_host.h"  // test builds only
#endif

namespace hopseq {

static uint8_t s_key[16];
static uint8_t s_perm[M];
static int64_t s_cachedEpoch = -1;

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND             \
  do {                       \
    v0 += v1;                \
    v1 = ROTL(v1, 13);       \
    v1 ^= v0;                \
    v0 = ROTL(v0, 32);       \
    v2 += v3;                \
    v3 = ROTL(v3, 16);       \
    v3 ^= v2;                \
    v0 += v3;                \
    v3 = ROTL(v3, 21);       \
    v3 ^= v0;                \
    v2 += v1;                \
    v1 = ROTL(v1, 17);       \
    v1 ^= v2;                \
    v2 = ROTL(v2, 32);       \
  } while (0)

static uint64_t le64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
  return v;
}

uint64_t siphash24(const uint8_t key[16], const uint8_t* in, size_t inlen) {
  uint64_t k0 = le64(key), k1 = le64(key + 8);
  uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
  uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
  uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
  uint64_t v3 = 0x7465646279746573ULL ^ k1;
  const uint8_t* end = in + inlen - (inlen % 8);
  uint64_t b = ((uint64_t)inlen) << 56;
  for (; in != end; in += 8) {
    uint64_t m = le64(in);
    v3 ^= m;
    SIPROUND;
    SIPROUND;
    v0 ^= m;
  }
  switch (inlen & 7) {
    case 7: b |= ((uint64_t)in[6]) << 48; /* fallthrough */
    case 6: b |= ((uint64_t)in[5]) << 40; /* fallthrough */
    case 5: b |= ((uint64_t)in[4]) << 32; /* fallthrough */
    case 4: b |= ((uint64_t)in[3]) << 24; /* fallthrough */
    case 3: b |= ((uint64_t)in[2]) << 16; /* fallthrough */
    case 2: b |= ((uint64_t)in[1]) << 8;  /* fallthrough */
    case 1: b |= ((uint64_t)in[0]); break;
    case 0: break;
  }
  v3 ^= b;
  SIPROUND;
  SIPROUND;
  v0 ^= b;
  v2 ^= 0xff;
  SIPROUND;
  SIPROUND;
  SIPROUND;
  SIPROUND;
  return v0 ^ v1 ^ v2 ^ v3;
}

void permutation(const uint8_t key[16], uint32_t epoch, uint8_t out[M]) {
  for (int i = 0; i < M; i++) out[i] = (uint8_t)i;
  uint8_t msg[8];
  for (int b = 0; b < 4; b++) msg[b] = (uint8_t)(epoch >> (8 * b));
  for (uint32_t i = M - 1; i > 0; i--) {
    for (int b = 0; b < 4; b++) msg[4 + b] = (uint8_t)(i >> (8 * b));
    uint64_t r = siphash24(key, msg, sizeof(msg));
    uint32_t j = (uint32_t)(r % (i + 1));  // bias < 1e-17, negligible
    uint8_t t = out[i];
    out[i] = out[j];
    out[j] = t;
  }
}

void keyFromCall(const char* call, uint8_t key[16]) {
  char up[16];
  size_t n = 0;
  for (; call[n] && n < sizeof(up) - 1; n++) up[n] = (char)toupper((unsigned char)call[n]);
  up[n] = 0;
  uint8_t digest[32];
  mbedtls_sha256((const unsigned char*)up, n, digest, 0);
  memcpy(key, digest, 16);
}

void setKey(const uint8_t key[16]) {
  memcpy(s_key, key, 16);
  s_cachedEpoch = -1;
}

uint8_t channel(int64_t slot, uint8_t net) {
  if (slot < 0) slot = 0;
  int64_t epoch = slot / M;
  if (epoch != s_cachedEpoch) {
    permutation(s_key, (uint32_t)epoch, s_perm);
    s_cachedEpoch = epoch;
  }
  return (uint8_t)((s_perm[slot % M] + NET_SPACING * net) % M);
}

}  // namespace hopseq

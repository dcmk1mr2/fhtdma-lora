#include "packet.h"

size_t pktEncode(const PktHeader& h, const uint8_t* payload, size_t plen, uint8_t* out, size_t outMax) {
  if (plen > PKT_MAX_LEN - PKT_HEADER_LEN) plen = PKT_MAX_LEN - PKT_HEADER_LEN;
  if (outMax < PKT_HEADER_LEN + plen) return 0;
  out[0] = (uint8_t)((PKT_VERSION << 6) | (h.type & 0x3F));
  out[1] = h.net;
  out[2] = h.src;
  out[3] = h.flags;
  out[4] = (uint8_t)(h.seq & 0xFF);
  out[5] = (uint8_t)(h.seq >> 8);
  if (plen) memcpy(out + PKT_HEADER_LEN, payload, plen);
  return PKT_HEADER_LEN + plen;
}

bool pktDecode(const uint8_t* in, size_t len, PktHeader& h, const uint8_t*& payload, size_t& plen) {
  if (len < PKT_HEADER_LEN || len > PKT_MAX_LEN) return false;
  if ((in[0] >> 6) != PKT_VERSION) return false;
  h.type = in[0] & 0x3F;
  h.net = in[1];
  h.src = in[2];
  h.flags = in[3];
  h.seq = (uint16_t)(in[4] | (in[5] << 8));
  payload = in + PKT_HEADER_LEN;
  plen = len - PKT_HEADER_LEN;
  return true;
}

void putI64(uint8_t* p, int64_t v) {
  uint64_t u = (uint64_t)v;
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(u >> (8 * i));
}

int64_t getI64(const uint8_t* p) {
  uint64_t u = 0;
  for (int i = 0; i < 8; i++) u |= (uint64_t)p[i] << (8 * i);
  return (int64_t)u;
}

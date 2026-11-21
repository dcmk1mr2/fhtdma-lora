#pragma once
#include <Arduino.h>
#include "config.h"

// 6-byte header, then up to 42 bytes of payload (48 bytes on air max).
//   [0] version (2 bits) | type (6 bits)
//   [1] net tag (first byte of SHA-256 of the net call sign)
//   [2] source node id
//   [3] flags (reserved, 0)
//   [4..5] sequence number, little endian

#define PKT_VERSION 1

enum PktType : uint8_t {
  PKT_TEXT = 1,    // broadcast text message
  PKT_TEST = 2,    // test-mode ping: int64 time stamp + filler to 48 bytes
  PKT_BEACON = 3,  // bench time beacon: int64 time stamp
  PKT_NET = 4,     // net manager beacon in slot 0: N, S0, slot map chunk, deputies, join ack
  PKT_JOIN = 5,    // join request in a free slot: permanent ID, name (src = 0xFF)
  PKT_DIR = 6,     // directory: up to 3 x (permanent ID, short address, name)
  PKT_ALIVE = 7,   // header-only keepalive from an idle member
  PKT_VOICE = 8,   // Codec2 voice: header flags = mode<<4 | frames, payload = frames (floor mode)
};

#define ADDR_NONE 0xFF

struct PktHeader {
  uint8_t type;
  uint8_t net;
  uint8_t src;
  uint8_t flags;
  uint16_t seq;
};

size_t pktEncode(const PktHeader& h, const uint8_t* payload, size_t plen, uint8_t* out, size_t outMax);
bool pktDecode(const uint8_t* in, size_t len, PktHeader& h, const uint8_t*& payload, size_t& plen);

void putI64(uint8_t* p, int64_t v);
int64_t getI64(const uint8_t* p);

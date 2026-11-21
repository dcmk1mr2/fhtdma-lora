#pragma once
#include <stdint.h>
#include <stddef.h>

// Callsign-seeded hop sequence (design doc section 2).
//
//   K      = first 16 bytes of SHA-256(upper-case net call sign)
//   e      = floor(s / 127)   hop epoch, s = absolute slot number
//   P(K,e) = Fisher-Yates shuffle of 0..126; the random source is
//            SipHash-2-4 keyed with K over the 8-byte message
//            (e as uint32 LE, step counter as uint32 LE)
//   ch(s)  = (P(K,e)[s mod 127] + 12 * net) mod 127
//
// Every channel is used exactly once per epoch, and every node with the same
// call sign and time computes the same channel with no coordination.
namespace hopseq {

static const int M = 127;          // channels
static const int NET_SPACING = 12; // channels between nets in orth mode

void setKey(const uint8_t key[16]);
void keyFromCall(const char* call, uint8_t key[16]);  // needs SHA-256 (ESP32 or host)

uint64_t siphash24(const uint8_t key[16], const uint8_t* msg, size_t len);
void permutation(const uint8_t key[16], uint32_t epoch, uint8_t out[M]);

// Channel for absolute slot s and net number (0 in indep mode).
uint8_t channel(int64_t slot, uint8_t net = 0);

}  // namespace hopseq

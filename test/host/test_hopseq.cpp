// Host test for the hop sequence. Build and run:
//   g++ -std=c++17 -O2 -I. -I../../src test_hopseq.cpp ../../src/hopseq.cpp -lcrypto -o t && ./t
#include "hopseq.h"
#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main() {
  // SipHash-2-4 reference vector (key 00..0f, message 00..0e)
  uint8_t k[16], m[15];
  for (int i = 0; i < 16; i++) k[i] = i;
  for (int i = 0; i < 15; i++) m[i] = i;
  CHECK(hopseq::siphash24(k, m, 15) == 0xa129ca6149be45e5ULL, "siphash vector");

  uint8_t key[16];
  hopseq::keyFromCall("w6mrr", key);
  hopseq::setKey(key);

  // Each epoch is a permutation; epochs differ.
  uint8_t p0[127], p1[127];
  for (uint32_t e = 0; e < 2000; e++) {
    hopseq::permutation(key, e, p1);
    bool seen[127] = {};
    for (int i = 0; i < 127; i++) { CHECK(p1[i] < 127 && !seen[p1[i]], "epoch %u not a permutation", e); seen[p1[i]] = true; }
    if (e) CHECK(memcmp(p0, p1, 127) != 0, "epoch %u repeats", e);
    memcpy(p0, p1, 127);
  }

  // Dwell: at most 0.4 s on any channel in any 20 s window (T = 120 ms -> 166 slots).
  const int W = 166;
  const int64_t S = 1000000, base = 13000000000LL;  // realistic slot numbers
  std::vector<uint8_t> ch(S);
  for (int64_t s = 0; s < S; s++) ch[s] = hopseq::channel(base + s);
  int count[127] = {}, worst = 0;
  long total[127] = {};
  for (int64_t s = 0; s < S; s++) {
    count[ch[s]]++; total[ch[s]]++;
    if (s >= W) count[ch[s - W]]--;
    for (int c = 0; c < 127; c++) if (count[c] > worst) worst = count[c];
  }
  CHECK(worst * 120 <= 400, "worst dwell %d slots (%d ms) in 20 s", worst, worst * 120);
  long mn = S, mx = 0;
  for (int c = 0; c < 127; c++) { if (total[c] < mn) mn = total[c]; if (total[c] > mx) mx = total[c]; }
  CHECK(mx - mn <= 2, "unequal channel use %ld..%ld", mn, mx);

  // Orth mode: nets 0..9 never co-channel, always >= 12 channels apart (circular).
  for (int64_t s = 0; s < 100000; s++) {
    for (int a = 0; a < 10; a++) for (int b = a + 1; b < 10; b++) {
      int d = (hopseq::channel(base + s, a) - hopseq::channel(base + s, b) + 127) % 127;
      if (d > 63) d = 127 - d;
      if (d < 12) { CHECK(false, "nets %d,%d only %d apart", a, b, d); goto done; }
    }
  }
done:
  // Different call signs give different sequences.
  uint8_t key2[16];
  hopseq::keyFromCall("KD2ABC", key2);
  hopseq::permutation(key2, 5, p1);
  hopseq::permutation(key, 5, p0);
  CHECK(memcmp(p0, p1, 127) != 0, "call signs collide");

  printf("worst dwell in 20 s: %d ms; channel use %ld..%ld per 1e6 slots\n", worst * 120, mn, mx);
  printf("first 10 channels for W6MRR from slot %lld:", (long long)base);
  for (int i = 0; i < 10; i++) printf(" %d", ch[i]);
  printf("\n%s\n", fails ? "FAILED" : "ALL PASSED");
  return fails ? 1 : 0;
}

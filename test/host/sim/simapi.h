#pragma once
#include <cstdint>
#include <cstddef>
extern int64_t g_simTimeUs;
extern bool g_simVerbose;
struct NodeApi {
  void (*init)(uint64_t mac, const char* name, uint16_t n);
  void (*loop)();
  int (*plan)(int64_t s, uint8_t* buf, size_t& len);
  void (*rx)(uint8_t type, uint8_t src, const uint8_t* p, size_t plen, int64_t slot);
  int (*role)();
  uint8_t (*addr)();
  int (*pos)(int64_t s);
  uint16_t (*n)();
  uint32_t (*id)();
  void (*rename)(const char* n);
};

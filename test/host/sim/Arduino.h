#pragma once
// Minimal Arduino/FreeRTOS stand-ins for the host net simulation.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <strings.h>
#include <climits>
#define IRAM_ATTR
struct Print { size_t print(const char* s) { return fputs(s, stdout); }
  size_t printf(const char* f, ...) { va_list a; va_start(a, f); int r = vprintf(f, a); va_end(a); return r; } };
uint32_t millis();
long random(long a, long b);
struct EspClass { uint64_t mac; uint64_t getEfuseMac() { return mac; } };
extern EspClass ESP;
struct portMUX_TYPE { int x; };
#define portMUX_INITIALIZER_UNLOCKED {0}
inline void portENTER_CRITICAL(portMUX_TYPE*) {}
inline void portEXIT_CRITICAL(portMUX_TYPE*) {}

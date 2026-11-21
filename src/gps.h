#pragma once
#include <Arduino.h>

namespace gps {

void begin();
void loop();
void printStatus(Print& out);

bool hasFix();

struct Summary {
  bool data;        // NMEA arriving in the last 3 s
  bool fix;
  uint32_t sats;
  bool timeValid;
  uint8_t hour, minute, second;
  bool ppsRecent;   // PPS edge in the last 2 s
};
Summary summary();
uint32_t ppsCount();

}  // namespace gps

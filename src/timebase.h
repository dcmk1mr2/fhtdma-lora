#pragma once
#include <Arduino.h>

// Network time in microseconds. In GPS mode this is UTC (Unix epoch).
// In bench mode it is the bench master's clock. Hopping and slot timing
// (step 2) will be computed from nowUs().
namespace timebase {

void begin();

int64_t localUs();                 // raw ESP32 clock
int64_t nowUs();                   // network time
int64_t toNetwork(int64_t local);  // convert a local timestamp

bool synced();
uint32_t syncAgeMs();
int64_t lastCorrectionUs();        // size of the last clock step (sync error)

// Called by the bench beacon receiver: remoteUs is the master's time at
// the moment the packet finished, localUs is our RX-done timestamp.
void syncFromBeacon(int64_t remoteUs, int64_t localUs);

// Called by the GPS code: utcUs is the UTC time of the PPS edge that was
// captured at localUs.
void syncFromPps(int64_t utcUs, int64_t localUs);

void reset();

}  // namespace timebase

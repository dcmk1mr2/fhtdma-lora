#pragma once
#include <Arduino.h>

enum class TimeMode : uint8_t {
  Local = 0,        // free-running ESP32 clock, no sync
  Bench = 1,        // follow beacons from a bench master (no GPS needed)
  BenchMaster = 2,  // be the bench time reference and send beacons
  Gps = 3,          // GPS NMEA time disciplined by PPS
};

enum class NetMode : uint8_t {
  Managed = 0,  // net manager assigns slots (step 3)
  Static = 1,   // fixed slot = node id (step 2)
};

struct Settings {
  char call[11];          // net call sign, seeds the hop sequence
  uint8_t nodeId;         // 0..N-1 (slot owner id)
  uint8_t channel;        // 0..126: channel when not hopping, and the bench sync channel
  uint16_t slots;         // N, slots per frame (2..256)
  bool hop;               // TDMA + frequency hopping on/off
  int8_t powerDbm;
  TimeMode timeMode;
  uint16_t testIntervalMs;
  char name[9];           // display name, up to 8 characters
  NetMode netMode;
  uint16_t listenSec;     // how long to listen for a manager before starting a net
  int8_t gpsRx;           // ESP32 pin that receives GPS TX (NMEA in)
  int8_t gpsTx;           // ESP32 pin that drives GPS RX, -1 = not wired
  int8_t gpsPps;          // GPS PPS input, -1 = not wired
  int8_t audioGainDb;     // audio output gain, -40..+30 dB
};

extern Settings g_settings;

void settingsLoad();
void settingsSave();

// First byte of SHA-256(call sign). Packets from other nets are ignored.
// Also loads the hop key (first 16 bytes of the same hash).
uint8_t netTag();
void recomputeNetTag();

// Name of the board function using this GPIO, or nullptr if it is free for the GPS.
const char* pinUse(int pin);

const char* timeModeName(TimeMode m);
bool parseTimeMode(const char* s, TimeMode& out);

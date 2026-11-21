#include "timebase.h"
#include "settings.h"
#include <esp_timer.h>

namespace timebase {

// Read from the radio task (core 0) and written from the main loop (core 1).
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_offset = 0;
static int64_t s_lastSyncLocal = -1;
static int64_t s_lastCorrection = 0;

void begin() { reset(); }

void reset() {
  portENTER_CRITICAL(&s_mux);
  s_offset = 0;
  s_lastSyncLocal = -1;
  s_lastCorrection = 0;
  portEXIT_CRITICAL(&s_mux);
}

int64_t localUs() { return esp_timer_get_time(); }
int64_t toNetwork(int64_t local) {
  portENTER_CRITICAL(&s_mux);
  int64_t off = s_offset;
  portEXIT_CRITICAL(&s_mux);
  return local + off;
}
int64_t nowUs() { return toNetwork(localUs()); }

static void applyOffset(int64_t newOffset, int64_t local) {
  portENTER_CRITICAL(&s_mux);
  s_lastCorrection = (s_lastSyncLocal < 0) ? 0 : (newOffset - s_offset);
  s_offset = newOffset;
  s_lastSyncLocal = local;
  portEXIT_CRITICAL(&s_mux);
}

void syncFromBeacon(int64_t remoteUs, int64_t local) {
  if (g_settings.timeMode != TimeMode::Bench) return;
  applyOffset(remoteUs - local, local);
}

void syncFromPps(int64_t utcUs, int64_t local) {
  if (g_settings.timeMode != TimeMode::Gps) return;
  applyOffset(utcUs - local, local);
}

uint32_t syncAgeMs() {
  portENTER_CRITICAL(&s_mux);
  int64_t last = s_lastSyncLocal;
  portEXIT_CRITICAL(&s_mux);
  if (last < 0) return UINT32_MAX;
  return (uint32_t)((localUs() - last) / 1000);
}

int64_t lastCorrectionUs() { return s_lastCorrection; }

bool synced() {
  switch (g_settings.timeMode) {
    case TimeMode::Local:
    case TimeMode::BenchMaster:
      return true;
    case TimeMode::Bench:
      return syncAgeMs() < 15000;  // beacons every SYNC_PERIOD slots (3.8 s)
    case TimeMode::Gps:
      return syncAgeMs() < 3000;
  }
  return false;
}

}  // namespace timebase

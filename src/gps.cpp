#include "gps.h"
#include "settings.h"
#include "config.h"
#include "timebase.h"
#include <TinyGPSPlus.h>
#include <esp_timer.h>

namespace gps {

static TinyGPSPlus s_gps;
static HardwareSerial& s_uart = Serial2;

static volatile int64_t s_ppsLocalUs = 0;
static volatile uint32_t s_ppsCount = 0;
static uint32_t s_ppsUsed = 0;
static uint32_t s_bytes = 0;
static uint32_t s_lastByteMs = 0;

static void IRAM_ATTR onPps() {
  s_ppsLocalUs = esp_timer_get_time();
  s_ppsCount = s_ppsCount + 1;
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

void begin() {
  s_uart.begin(GPS_BAUD, SERIAL_8N1, g_settings.gpsRx, g_settings.gpsTx);
  if (g_settings.gpsPps >= 0) {
    pinMode(g_settings.gpsPps, INPUT);
    attachInterrupt(digitalPinToInterrupt(g_settings.gpsPps), onPps, RISING);
  }
}

void loop() {
  while (s_uart.available()) {
    s_gps.encode((char)s_uart.read());
    s_bytes++;
    s_lastByteMs = millis();
  }

  // u-blox sends the NMEA time for second S shortly after the PPS edge at S.
  // Pair each new PPS edge with the next valid time sentence.
  if (s_gps.time.isUpdated() && s_gps.time.isValid() && s_gps.date.isValid()) {
    noInterrupts();
    uint32_t count = s_ppsCount;
    int64_t ppsLocal = s_ppsLocalUs;
    interrupts();

    int64_t age = timebase::localUs() - ppsLocal;
    if (count != s_ppsUsed && age >= 0 && age < 900000 && s_gps.date.year() >= 2020) {
      s_ppsUsed = count;
      int64_t days = daysFromCivil(s_gps.date.year(), s_gps.date.month(), s_gps.date.day());
      int64_t secs = days * 86400 + s_gps.time.hour() * 3600 + s_gps.time.minute() * 60 + s_gps.time.second();
      timebase::syncFromPps(secs * 1000000LL, ppsLocal);
    }
  }
}

bool hasFix() { return s_gps.location.isValid() && s_gps.location.age() < 3000; }
uint32_t ppsCount() { return s_ppsCount; }

Summary summary() {
  Summary m = {};
  m.data = s_bytes > 0 && (millis() - s_lastByteMs) < 3000;
  m.fix = hasFix();
  m.sats = s_gps.satellites.isValid() ? s_gps.satellites.value() : 0;
  m.timeValid = s_gps.time.isValid() && s_gps.time.age() < 3000;
  if (m.timeValid) {
    // gps::loop() checks isUpdated() right after parsing, before this runs,
    // so clearing the flag here doesn't make it miss a PPS pairing.
    uint32_t v = s_gps.time.value();  // hhmmsscc
    m.hour = (v / 1000000) % 100;
    m.minute = (v / 10000) % 100;
    m.second = (v / 100) % 100;
  }
  noInterrupts();
  int64_t last = s_ppsLocalUs;
  uint32_t count = s_ppsCount;
  interrupts();
  m.ppsRecent = count > 0 && (timebase::localUs() - last) < 2000000;
  return m;
}

void printStatus(Print& out) {
  out.printf("GPS bytes rx: %lu  sentences ok: %lu  checksum fail: %lu\r\n",
             (unsigned long)s_bytes, (unsigned long)s_gps.passedChecksum(),
             (unsigned long)s_gps.failedChecksum());
  if (s_bytes == 0)
    out.printf("  No data from GPS. Check wiring (GPS TX -> GPIO%d) and baud (%d).\r\n", g_settings.gpsRx, GPS_BAUD);

  if (s_gps.date.isValid() && s_gps.time.isValid()) {
    out.printf("UTC:  %04u-%02u-%02u %02u:%02u:%02u\r\n", s_gps.date.year(), s_gps.date.month(),
               s_gps.date.day(), s_gps.time.hour(), s_gps.time.minute(), s_gps.time.second());
  } else {
    out.print("UTC:  (no time yet)\r\n");
  }
  if (s_gps.location.isValid()) {
    out.printf("Fix:  %s  lat %.6f  lon %.6f  alt %.1f m\r\n", hasFix() ? "yes" : "stale",
               s_gps.location.lat(), s_gps.location.lng(), s_gps.altitude.meters());
  } else {
    out.print("Fix:  no\r\n");
  }
  out.printf("Sats: %lu  HDOP: %.1f\r\n", (unsigned long)s_gps.satellites.value(), s_gps.hdop.hdop());
  uint32_t pc = s_ppsCount;
  if (pc == 0) {
    if (g_settings.gpsPps < 0) out.print("PPS:  not wired (set gps <rx> <tx> <pps> to add it)\r\n");
    else out.printf("PPS:  none seen (needs a fix, and PPS wired to GPIO%d)\r\n", g_settings.gpsPps);
  } else {
    int64_t age = (timebase::localUs() - s_ppsLocalUs) / 1000;
    out.printf("PPS:  %lu pulses, last %lld ms ago\r\n", (unsigned long)pc, (long long)age);
  }
}

}  // namespace gps

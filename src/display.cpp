#include "display.h"
#include "audio.h"
#include "config.h"
#include "gps.h"
#include "net.h"
#include "packet.h"
#include "radio.h"
#include "settings.h"
#include "timebase.h"
#include <SSD1306Wire.h>

namespace display {

static SSD1306Wire s_oled(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64, I2C_ONE, 400000);
static bool s_ok = false;
static uint32_t s_nextMs = 0;

void begin() {
  if (PIN_VEXT >= 0) {
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, LOW);  // power the OLED
    delay(50);
  }
  if (PIN_OLED_RST >= 0) {
    pinMode(PIN_OLED_RST, OUTPUT);
    digitalWrite(PIN_OLED_RST, LOW);
    delay(20);
    digitalWrite(PIN_OLED_RST, HIGH);
    delay(20);
  }
  s_ok = s_oled.init();
  if (!s_ok) return;
  s_oled.flipScreenVertically();
  s_oled.setFont(ArialMT_Plain_10);
  s_oled.setTextAlignment(TEXT_ALIGN_LEFT);
}

void loop() {
  if (!s_ok || (int32_t)(millis() - s_nextMs) < 0) return;
  s_nextMs = millis() + 1000;

  gps::Summary g = gps::summary();
  char line[40];
  s_oled.clear();

  static const char* roleTag[] = {"off", "static", "listen", "joining", "member", "MGR"};
  uint8_t a = net::myAddr();
  if (a == ADDR_NONE) snprintf(line, sizeof(line), "%s %s/-  %s", g_settings.name, g_settings.call,
                               roleTag[(int)net::role()]);
  else snprintf(line, sizeof(line), "%s %s/%u  %s", g_settings.name, g_settings.call, a, roleTag[(int)net::role()]);
  s_oled.drawString(0, 0, line);

  if (!g.data) snprintf(line, sizeof(line), "GPS: no data");
  else snprintf(line, sizeof(line), "GPS: %s  %lu sats", g.fix ? "FIX" : "no fix", (unsigned long)g.sats);
  s_oled.drawString(0, 12, line);
  if (audio::pttDown()) s_oled.drawString(104, 12, "PTT");

  if (g.timeValid)
    snprintf(line, sizeof(line), "%02u:%02u:%02u UTC  PPS %s", g.hour, g.minute, g.second, g.ppsRecent ? "ok" : "--");
  else
    snprintf(line, sizeof(line), "--:--:-- UTC  PPS %s", g.ppsRecent ? "ok" : "--");
  s_oled.drawString(0, 24, line);

  snprintf(line, sizeof(line), "Time: %s %s", timeModeName(g_settings.timeMode),
           timebase::synced() ? "synced" : "NO SYNC");
  s_oled.drawString(0, 36, line);

  if (radio::hopping())
    snprintf(line, sizeof(line), "Hop ch %u  slot %d/%u", radio::currentChannel(),
             net::position(radio::currentSlot()), net::currentN());
  else
    snprintf(line, sizeof(line), "Fixed ch %u", g_settings.channel);
  s_oled.drawString(0, 48, line);

  s_oled.display();
}

}  // namespace display

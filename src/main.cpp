// FH-TDMA LoRa node firmware.
// Step 1: terminal and radio. Step 2: TDMA slots and hopping. Step 3: net manager.
// 0.4: audio front end (INMP441 mic, sigma-delta out, PTT) for Codec2 voice.

#include <Arduino.h>
#include "app.h"
#include "audio.h"
#include "voice.h"
#include "config.h"
#include "display.h"
#include "gps.h"
#include "net.h"
#include "radio.h"
#include "settings.h"
#include "terminal.h"
#include "timebase.h"

// Boot trace: each step prints before it runs, so a hang shows where it stopped.
static void step(const char* what) {
  Serial.printf("boot: %s\r\n", what);
  Serial.flush();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\r\nFH-TDMA %s starting\r\n", FW_VERSION);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  randomSeed(esp_random());

  // Safe mode: hold PRG (GPIO0) through reset to skip GPS and audio setup.
  pinMode(0, INPUT_PULLUP);
  bool safe = digitalRead(0) == LOW;
  if (safe) Serial.print("SAFE MODE (PRG held): GPS and audio not started\r\n");

  step("settings");
  settingsLoad();
  step("net");
  net::begin();
  step("timebase");
  timebase::begin();
  if (!safe) {
    Serial.printf("boot: gps (rx %d, tx %d, pps %d)\r\n", g_settings.gpsRx, g_settings.gpsTx, g_settings.gpsPps);
    Serial.flush();
    gps::begin();
  }
  step("radio");
  if (!radio::begin()) {
    Serial.print("Radio not found. Check the board type and LoRa pins in include/config.h.\r\n");
  }
  step("app");
  app::begin();
  step("display");
  display::begin();
  if (!safe) {
    step("voice");
    voice::begin();
    step("audio");
    audio::begin();
  }
  step("done");
  terminal::begin();
}

void loop() {
  gps::loop();
  radio::loop();
  net::loop();
  app::loop();
  terminal::loop();
  display::loop();
  audio::loop();
  voice::loop();
}

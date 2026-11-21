#include "settings.h"
#include "config.h"
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include "hopseq.h"

Settings g_settings;
static uint8_t s_netTag = 0;
static Preferences prefs;

static void defaults() {
  strlcpy(g_settings.call, "SF-HAB", sizeof(g_settings.call));
  g_settings.nodeId = 1;
  g_settings.channel = CH_DEFAULT;
  g_settings.powerDbm = LORA_POWER_DEFAULT;
  g_settings.timeMode = TimeMode::Local;
  g_settings.testIntervalMs = 1000;
  g_settings.slots = 8;
  g_settings.hop = true;
  g_settings.name[0] = 0;  // net::begin() fills in N-XXXX
  g_settings.netMode = NetMode::Managed;
  g_settings.listenSec = 20;
  g_settings.gpsRx = PIN_GPS_RX;
  g_settings.gpsTx = PIN_GPS_TX;
  g_settings.gpsPps = PIN_GPS_PPS;
  g_settings.audioGainDb = -12;
  g_settings.voiceOn = true;
  g_settings.voiceBitrate = 2400;
}

const char* pinUse(int p) {
  if (p < 0) return nullptr;
  if (p == PIN_LORA_SCK || p == PIN_LORA_MISO || p == PIN_LORA_MOSI || p == PIN_LORA_CS || p == PIN_LORA_RST) return "LoRa";
#ifdef PIN_LORA_BUSY
  if (p == PIN_LORA_BUSY) return "LoRa";
#endif
#ifdef PIN_LORA_DIO0
  if (p == PIN_LORA_DIO0) return "LoRa";
#endif
  if (p == PIN_LORA_DIO1) return "LoRa";
  if (p == PIN_OLED_SDA || p == PIN_OLED_SCL || p == PIN_OLED_RST) return "OLED";
  if (p == PIN_VEXT) return "OLED power (Vext)";
  if (p == PIN_LED) return "LED";
  if (p == PIN_SLOT_DEBUG) return "slot debug output";
#if HAS_AUDIO
  if (p == PIN_MIC_SCK || p == PIN_MIC_WS || p == PIN_MIC_SD) return "INMP441 mic";
  if (p == PIN_AUDIO_OUT) return "audio out";
  if (p == PIN_PTT) return "PTT";
#endif
#if defined(BOARD_HELTEC_V3)
  if (p == 43 || p == 44) return "USB serial (the terminal)";
  if (p == 0) return "PRG button";
  if (p == 3 || p == 45 || p == 46) return "a boot strapping pin";
  if (p == 26 || (p >= 33 && p <= 38)) return "flash/SubSPI (reserved by Heltec)";
  if (p >= 39 && p <= 42) return "JTAG (not recommended)";
  if (p > 48 || (p >= 22 && p <= 32)) return "not a usable GPIO";
#else
  if (p == 1 || p == 3) return "USB serial (the terminal)";
  if (p == 0 || p == 2 || p == 5 || p == 12 || p == 15) return "a boot strapping pin";
  if (p > 39 || (p >= 6 && p <= 11)) return "not a usable GPIO";
#endif
  return nullptr;
}

void settingsLoad() {
  defaults();
  prefs.begin("fhtdma", true);
  prefs.getString("call", g_settings.call, sizeof(g_settings.call));
  if (g_settings.call[0] == 0) strlcpy(g_settings.call, "SF-HAB", sizeof(g_settings.call));
  g_settings.nodeId = prefs.getUChar("id", g_settings.nodeId);
  g_settings.channel = prefs.getUChar("ch", g_settings.channel);
  g_settings.powerDbm = prefs.getChar("pwr", g_settings.powerDbm);
  g_settings.timeMode = (TimeMode)prefs.getUChar("time", (uint8_t)g_settings.timeMode);
  g_settings.testIntervalMs = prefs.getUShort("tint", g_settings.testIntervalMs);
  g_settings.slots = prefs.getUShort("n", g_settings.slots);
  g_settings.hop = prefs.getBool("hop", g_settings.hop);
  prefs.getString("name", g_settings.name, sizeof(g_settings.name));
  g_settings.netMode = (NetMode)prefs.getUChar("nmode", (uint8_t)g_settings.netMode);
  g_settings.listenSec = prefs.getUShort("listen", g_settings.listenSec);
  g_settings.gpsRx = prefs.getChar("grx", g_settings.gpsRx);
  g_settings.gpsTx = prefs.getChar("gtx", g_settings.gpsTx);
  g_settings.gpsPps = prefs.getChar("gpps", g_settings.gpsPps);
  g_settings.audioGainDb = prefs.getChar("again", g_settings.audioGainDb);
  g_settings.voiceOn = prefs.getBool("voice", g_settings.voiceOn);
  g_settings.voiceBitrate = prefs.getUShort("codec", g_settings.voiceBitrate);
  prefs.end();

  if (g_settings.channel >= CH_COUNT) g_settings.channel = CH_DEFAULT;
  if (g_settings.nodeId == 255) g_settings.nodeId = 1;
  if (g_settings.powerDbm < 2 || g_settings.powerDbm > LORA_POWER_MAX) g_settings.powerDbm = LORA_POWER_DEFAULT;
  if ((uint8_t)g_settings.timeMode > (uint8_t)TimeMode::Gps) g_settings.timeMode = TimeMode::Local;
  if (g_settings.testIntervalMs < 200) g_settings.testIntervalMs = 1000;
  if (g_settings.slots < 2 || g_settings.slots > 256) g_settings.slots = 8;
  if ((uint8_t)g_settings.netMode > 1) g_settings.netMode = NetMode::Managed;
  if (g_settings.listenSec < 2 || g_settings.listenSec > 600) g_settings.listenSec = 20;
  if (g_settings.voiceBitrate != 2400 && g_settings.voiceBitrate != 1600 && g_settings.voiceBitrate != 1200)
    g_settings.voiceBitrate = 2400;
  if (g_settings.audioGainDb < -40 || g_settings.audioGainDb > 30) g_settings.audioGainDb = -12;
  // A saved GPS pin that now clashes with a board function falls back to the default.
  if (g_settings.gpsRx < 0 || pinUse(g_settings.gpsRx)) g_settings.gpsRx = PIN_GPS_RX;
  if (pinUse(g_settings.gpsTx)) g_settings.gpsTx = PIN_GPS_TX;
  if (pinUse(g_settings.gpsPps)) g_settings.gpsPps = PIN_GPS_PPS;
  recomputeNetTag();
}

void settingsSave() {
  prefs.begin("fhtdma", false);
  prefs.putString("call", g_settings.call);
  prefs.putUChar("id", g_settings.nodeId);
  prefs.putUChar("ch", g_settings.channel);
  prefs.putChar("pwr", g_settings.powerDbm);
  prefs.putUChar("time", (uint8_t)g_settings.timeMode);
  prefs.putUShort("tint", g_settings.testIntervalMs);
  prefs.putUShort("n", g_settings.slots);
  prefs.putBool("hop", g_settings.hop);
  prefs.putString("name", g_settings.name);
  prefs.putUChar("nmode", (uint8_t)g_settings.netMode);
  prefs.putUShort("listen", g_settings.listenSec);
  prefs.putChar("grx", g_settings.gpsRx);
  prefs.putChar("gtx", g_settings.gpsTx);
  prefs.putChar("gpps", g_settings.gpsPps);
  prefs.putChar("again", g_settings.audioGainDb);
  prefs.putBool("voice", g_settings.voiceOn);
  prefs.putUShort("codec", g_settings.voiceBitrate);
  prefs.end();
}

void recomputeNetTag() {
  uint8_t key[16];
  hopseq::keyFromCall(g_settings.call, key);  // SHA-256 of the upper-case call sign
  s_netTag = key[0];
  hopseq::setKey(key);
}

uint8_t netTag() { return s_netTag; }

const char* timeModeName(TimeMode m) {
  switch (m) {
    case TimeMode::Local: return "local";
    case TimeMode::Bench: return "bench";
    case TimeMode::BenchMaster: return "benchmaster";
    case TimeMode::Gps: return "gps";
  }
  return "?";
}

bool parseTimeMode(const char* s, TimeMode& out) {
  for (uint8_t i = 0; i <= (uint8_t)TimeMode::Gps; i++) {
    if (strcasecmp(s, timeModeName((TimeMode)i)) == 0) { out = (TimeMode)i; return true; }
  }
  return false;
}

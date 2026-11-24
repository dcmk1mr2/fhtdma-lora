#include "terminal.h"
#include "app.h"
#include "audio.h"
#include "voice.h"
#include "config.h"
#include "gps.h"
#include "net.h"
#include "radio.h"
#include "settings.h"
#include "timebase.h"
#include <stdarg.h>

namespace terminal {

enum class Mode { Command, Text };
static Mode s_mode = Mode::Command;
static char s_line[128];
static size_t s_len = 0;
static bool s_lastWasCR = false;

static const char* prompt() { return s_mode == Mode::Text ? "text> " : "> "; }

static void showPrompt() {
  Serial.print(prompt());
  Serial.write((const uint8_t*)s_line, s_len);
}

void printAsync(const char* fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print("\r\x1b[K");  // clear the current input line
  Serial.print(buf);
  Serial.print("\r\n");
  showPrompt();
}

void printMessage(const char* call, uint8_t src, const char* text) {
  printAsync("[%s/%u] %s", call, src, text);
}

static void help() {
  Serial.print(
      "Commands:\r\n"
      "  help                 this list\r\n"
      "  status               settings, radio and time status\r\n"
      "  gps                  GPS time, fix, satellites, PPS\r\n"
      "  text                 enter text mode (type exit to leave)\r\n"
      "  test on|off          test mode: send a ping every interval, log what is heard\r\n"
      "  stats [reset]        per-node packet counts, loss, RSSI, time error\r\n"
      "  who                  nodes on the net: name, address, slot, last heard\r\n"
      "  audio                mic level, PTT and audio test status\r\n"
      "  audio loop on|off|ptt|codec  mic to speaker: always, off, while PTT held, or through Codec2\r\n"
      "  audio tone on|off    1 kHz test tone on the audio output\r\n"
      "  audio meter on|off   print the mic level twice a second\r\n"
      "  audio mic left|right I2S channel for the mic (L/R pin to GND = left)\r\n"
      "  audio gain <dB>      output level for tone and loopback, -40 to 30 (saved)\r\n"
      "  audio micgain <dB>   mic gain before Codec2, -10 to 30 (saved, default 12)\r\n"
      "  set                  show all settings\r\n"
      "  set call <CALL>      net call sign (all nodes in a net must match)\r\n"
      "  set name <name>      display name, 1-8 characters, no spaces (unique on the net)\r\n"
      "  set netmode <mode>   managed (net manager assigns slots) | static (slot = id)\r\n"
      "  set listen <s>       seconds to listen for a manager before starting a net\r\n"
      "  set n <2-256>        starting N if this node starts the net (static: N for all)\r\n"
      "  set id <0..N-1>      static mode only: node ID = slot (unique per node)\r\n"
      "  set hop on|off       TDMA + frequency hopping (needs bench or gps time)\r\n"
      "  set ch <0-126>       fixed channel when hop is off, and bench sync channel\r\n"
      "  set power <dBm>      TX power, 2-17 (SX1276) or 2-22 (SX1262)\r\n"
      "  set interval <ms>    test ping interval when hop is off (200-60000)\r\n"
      "  set time <mode>      local | bench | benchmaster | gps\r\n"
      "  set gps <rx> [tx] [pps]  GPS pins (rx = pin wired to GPS TX); reboot to apply\r\n"
      "  set voice on|off     PTT sends Codec2 voice to the net\r\n"
      "  set codec <rate>     Codec2 bit rate: 3200, 2400 (default), 1600 or 1200\r\n"
      "  pinscan              find which GPIO a signal (e.g. GPS TX) is wired to\r\n"
      "  reboot               restart the board\r\n"
      "Settings are saved automatically.\r\n");
}

// Whole-string integer parse: "8x" or "abc" are rejected instead of read as 8 or 0.
static bool parseNum(const char* s, long lo, long hi, long& out) {
  char* end = nullptr;
  long v = strtol(s, &end, 10);
  if (end == s || *end != 0 || v < lo || v > hi) return false;
  out = v;
  return true;
}

static void showSettings() {
  bool stat = g_settings.netMode == NetMode::Static;
  Serial.printf("  call      %s\r\n", g_settings.call);
  Serial.printf("  name      %s\r\n", g_settings.name);
  Serial.printf("  netmode   %s\r\n", stat ? "static" : "managed");
  Serial.printf("  listen    %u s%s\r\n", g_settings.listenSec, stat ? " (managed mode only)" : "");
  Serial.printf("  n         %u%s\r\n", g_settings.slots, stat ? "" : " (used if this node starts the net)");
  Serial.printf("  id        %u%s\r\n", g_settings.nodeId, stat ? "" : " (static mode only)");
  Serial.printf("  hop       %s\r\n", g_settings.hop ? "on" : "off");
  Serial.printf("  ch        %u\r\n", g_settings.channel);
  Serial.printf("  power     %d dBm\r\n", g_settings.powerDbm);
  Serial.printf("  interval  %u ms\r\n", g_settings.testIntervalMs);
  Serial.printf("  time      %s\r\n", timeModeName(g_settings.timeMode));
  Serial.printf("  voice     %s, codec %u\r\n", g_settings.voiceOn ? "on" : "off", g_settings.voiceBitrate);
  Serial.printf("  gps       rx %d, tx %d, pps %d  (-1 = not wired)\r\n", g_settings.gpsRx, g_settings.gpsTx,
                g_settings.gpsPps);
}

// "none", "-" or "-1" mean not wired.
static bool parsePin(const char* s, bool allowNone, int& out) {
  if (!strcasecmp(s, "none") || !strcmp(s, "-") || !strcmp(s, "-1")) {
    if (!allowNone) return false;
    out = -1;
    return true;
  }
  long v;
  if (!parseNum(s, 0, 48, v)) return false;
  out = (int)v;
  return true;
}

static void cmdSetGps(const char* rx, const char* tx, const char* pps, bool extra) {
  if (!rx || extra) {
    Serial.print("usage: set gps <rx> [tx] [pps]   pins on the ESP32; tx and pps can be none\r\n"
                 "  rx = pin wired to the GPS TX, tx = pin wired to the GPS RX\r\n");
    return;
  }
  int r, t = g_settings.gpsTx, p = g_settings.gpsPps;
  if (!parsePin(rx, false, r) || (tx && !parsePin(tx, true, t)) || (pps && !parsePin(pps, true, p))) {
    Serial.print("pins must be GPIO numbers 0-48 (tx and pps can be none)\r\n");
    return;
  }
  const int pins[3] = {r, t, p};
  const char* names[3] = {"rx", "tx", "pps"};
  for (int i = 0; i < 3; i++) {
    const char* use = pinUse(pins[i]);
    if (use) {
      Serial.printf("GPIO%d is used for %s on this board; pick another pin for gps %s\r\n", pins[i], use, names[i]);
      return;
    }
    for (int j = 0; j < i; j++) {
      if (pins[i] >= 0 && pins[i] == pins[j]) {
        Serial.printf("gps %s and %s can't share GPIO%d\r\n", names[j], names[i], pins[i]);
        return;
      }
    }
  }
  g_settings.gpsRx = (int8_t)r;
  g_settings.gpsTx = (int8_t)t;
  g_settings.gpsPps = (int8_t)p;
  settingsSave();
  Serial.printf("GPS pins: rx %d, tx %d, pps %d. Saved; type reboot to use them.\r\n", r, t, p);
}

static void cmdSet(char* key, char* val, bool extra) {
  if (!key) {
    showSettings();
    return;
  }
  if (!val) {
    Serial.print("usage: set <call|name|netmode|listen|id|n|hop|ch|power|interval|time|gps|voice|codec> <value>  (set alone lists them)\r\n");
    return;
  }
  if (extra) {
    Serial.printf("too many words: set %s takes one value with no spaces\r\n", key);
    return;
  }
  bool isStatic = g_settings.netMode == NetMode::Static;
  bool rejoin = false;  // change affects the schedule or our identity
  if (!strcasecmp(key, "call")) {
    size_t n = strlen(val);
    if (n < 1 || n > 10) {
      Serial.print("call sign must be 1-10 characters\r\n");
      return;
    }
    for (char* p = val; *p; p++) *p = toupper(*p);
    strlcpy(g_settings.call, val, sizeof(g_settings.call));
    recomputeNetTag();
    rejoin = true;
  } else if (!strcasecmp(key, "id")) {
    long v;
    if (!parseNum(val, 0, 254, v)) {
      Serial.print("id must be a number 0-254\r\n");
      return;
    }
    g_settings.nodeId = (uint8_t)v;
    if (!isStatic) Serial.print("note: id is only used in static mode; the net manager assigns addresses\r\n");
    else if (v >= g_settings.slots) Serial.printf("note: id must be below N (%u) to get a slot\r\n", g_settings.slots);
    rejoin = isStatic;
  } else if (!strcasecmp(key, "name")) {
    size_t n = strlen(val);
    if (n < 1 || n > 8) {
      Serial.print("name must be 1-8 characters\r\n");
      return;
    }
    for (const char* p = val; *p; p++) {
      if (*p < 33 || *p > 126) {
        Serial.print("name can use letters, digits and symbols only\r\n");
        return;
      }
    }
    strlcpy(g_settings.name, val, sizeof(g_settings.name));
    net::nameChanged();  // manager renames itself in place; a member rejoins with the new name
  } else if (!strcasecmp(key, "netmode")) {
    if (!strcasecmp(val, "managed")) g_settings.netMode = NetMode::Managed;
    else if (!strcasecmp(val, "static")) g_settings.netMode = NetMode::Static;
    else {
      Serial.print("usage: set netmode managed|static\r\n");
      return;
    }
    rejoin = true;
  } else if (!strcasecmp(key, "listen")) {
    long v;
    if (!parseNum(val, 2, 600, v)) {
      Serial.print("listen must be a number 2-600 (seconds)\r\n");
      return;
    }
    g_settings.listenSec = (uint16_t)v;
  } else if (!strcasecmp(key, "n")) {
    long v;
    if (!parseNum(val, 2, 256, v)) {
      Serial.print("n must be a number 2-256\r\n");
      return;
    }
    g_settings.slots = (uint16_t)v;
    if (!isStatic)
      Serial.print("note: in managed mode this is the starting N if this node starts the net; "
                   "the manager sets N for everyone\r\n");
    else if (g_settings.nodeId >= v)
      Serial.printf("note: this node's id %u is not below N, it won't transmit\r\n", g_settings.nodeId);
    rejoin = isStatic;
  } else if (!strcasecmp(key, "hop")) {
    if (!strcasecmp(val, "on")) g_settings.hop = true;
    else if (!strcasecmp(val, "off")) g_settings.hop = false;
    else {
      Serial.print("usage: set hop on|off\r\n");
      return;
    }
    rejoin = true;
  } else if (!strcasecmp(key, "ch")) {
    long v;
    if (!parseNum(val, 0, CH_COUNT - 1, v) || !radio::setChannel((uint8_t)v)) {
      Serial.print("channel must be a number 0-126\r\n");
      return;
    }
    g_settings.channel = (uint8_t)v;
  } else if (!strcasecmp(key, "power")) {
    long v;
    if (!parseNum(val, 2, LORA_POWER_MAX, v) || !radio::setPower((int8_t)v)) {
      Serial.printf("power must be 2-%d dBm\r\n", LORA_POWER_MAX);
      return;
    }
    g_settings.powerDbm = (int8_t)v;
  } else if (!strcasecmp(key, "interval")) {
    long v;
    if (!parseNum(val, 200, 60000, v)) {
      Serial.print("interval must be 200-60000 ms\r\n");
      return;
    }
    g_settings.testIntervalMs = (uint16_t)v;
  } else if (!strcasecmp(key, "voice")) {
    if (!strcasecmp(val, "on")) g_settings.voiceOn = true;
    else if (!strcasecmp(val, "off")) g_settings.voiceOn = false;
    else {
      Serial.print("voice must be on or off\r\n");
      return;
    }
  } else if (!strcasecmp(key, "codec")) {
    long v;
    if (!parseNum(val, 1200, 3200, v) || (v != 3200 && v != 2400 && v != 1600 && v != 1200)) {
      Serial.print("codec must be 3200, 2400, 1600 or 1200\r\n");
      return;
    }
    if (!voice::setMode((int)v)) {
      Serial.print("can't change the codec now (talking, or no audio on this board)\r\n");
      return;
    }
    g_settings.voiceBitrate = (uint16_t)v;
  } else if (!strcasecmp(key, "time")) {
    TimeMode m;
    if (!parseTimeMode(val, m)) {
      Serial.print("time mode must be local, bench, benchmaster or gps\r\n");
      return;
    }
    g_settings.timeMode = m;
    timebase::reset();
    rejoin = true;
  } else {
    Serial.printf("unknown setting '%s'\r\n", key);
    return;
  }
  settingsSave();
  if (rejoin) net::restart();
  Serial.print("ok\r\n");
}

static void cmdAudio(const char* what, const char* val, bool extra) {
  if (!audio::available()) {
    audio::printStatus(Serial);
    return;
  }
  if (!what) {
    audio::printStatus(Serial);
    return;
  }
  bool on = val && !strcasecmp(val, "on");
  bool off = val && !strcasecmp(val, "off");
  if (extra) {
    Serial.print("too many words. Type help.\r\n");
  } else if (!strcasecmp(what, "loop") && (on || off || (val && (!strcasecmp(val, "ptt") || !strcasecmp(val, "codec"))))) {
    audio::setLoopback(on ? audio::Loop::On : off ? audio::Loop::Off
                       : !strcasecmp(val, "ptt") ? audio::Loop::Ptt : audio::Loop::Codec);
    if (!strcasecmp(val, "codec")) Serial.print("Hold PTT and talk: you hear yourself through Codec2 (no radio). PTT won't transmit until 'audio loop off'.\r\n");
    if (!off) audio::setTone(false);
    Serial.printf("Audio loopback %s.\r\n", val);
  } else if (!strcasecmp(what, "tone") && (on || off)) {
    audio::setTone(on);
    Serial.printf("Test tone %s.\r\n", val);
  } else if (!strcasecmp(what, "meter") && (on || off)) {
    audio::setMeter(on);
    Serial.printf("Mic meter %s.\r\n", val);
  } else if (!strcasecmp(what, "gain") && val) {
    long db;
    if (!parseNum(val, -40, 30, db)) {
      Serial.print("gain must be -40 to 30 dB\r\n");
    } else {
      audio::setGainDb((int)db);
      g_settings.audioGainDb = (int8_t)db;
      settingsSave();
      Serial.printf("Audio gain %ld dB (saved).\r\n", db);
    }
  } else if (!strcasecmp(what, "micgain") && val) {
    long db;
    if (!parseNum(val, -10, 30, db)) {
      Serial.print("mic gain must be -10 to 30 dB\r\n");
    } else {
      audio::setMicGainDb((int)db);
      g_settings.micGainDb = (int8_t)db;
      settingsSave();
      Serial.printf("Mic gain %ld dB (saved).\r\n", db);
    }
  } else if (!strcasecmp(what, "mic") && val && (!strcasecmp(val, "left") || !strcasecmp(val, "right"))) {
    bool ok = audio::setMicRight(!strcasecmp(val, "right"));
    Serial.printf(ok ? "Mic reads the %s channel.\r\n" : "Mic restart failed (%s).\r\n", val);
  } else {
    Serial.print("usage: audio | audio loop on|off|ptt|codec | audio micgain <dB> | audio tone on|off | audio meter on|off | audio mic left|right | audio gain <-40..30>\r\n");
  }
}

// Watch the free GPIOs for 3 s and report which ones toggle. With the GPS TX wired to
// some pad, that pad shows hundreds of edges, which names its GPIO number.
static void pinScan() {
#if defined(BOARD_HELTEC_V3)
  static const int8_t cand[] = {1, 2, 3, 4, 5, 6, 7, 19, 20, 38, 39, 40, 41, 42, 45, 46, 47, 48};
#else
  static const int8_t cand[] = {2, 4, 12, 13, 15, 16, 17, 21, 22, 23, 25, 32, 33, 34, 36, 37, 38, 39};
#endif
  const int n = sizeof(cand);
  bool watch[sizeof(cand)];
  int last[sizeof(cand)];
  uint32_t edges[sizeof(cand)] = {0};
  for (int i = 0; i < n; i++) {
    int p = cand[i];
    // Already inputs: leave their setup alone and just read them.
    bool input = p == g_settings.gpsRx || p == g_settings.gpsPps;
#if HAS_AUDIO
    input = input || p == PIN_PTT || p == PIN_MIC_SD;
    bool driven = p == PIN_AUDIO_OUT || p == PIN_MIC_SCK || p == PIN_MIC_WS;
#else
    bool driven = pinUse(p) != nullptr;
#endif
    // Skip pins this firmware drives (outputs, I2S clocks, GPS TX); strapping and JTAG
    // pins are fine to read after boot.
    driven = driven || p == PIN_SLOT_DEBUG || p == g_settings.gpsTx;
    watch[i] = input || !driven;
    if (watch[i] && !input) pinMode(p, INPUT);
    last[i] = watch[i] ? digitalRead(p) : 0;
  }
  Serial.print("Watching free GPIOs for 3 s...\r\n");
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    for (int i = 0; i < n; i++) {
      if (!watch[i]) continue;
      int v = digitalRead(cand[i]);
      if (v != last[i]) {
        edges[i]++;
        last[i] = v;
      }
    }
  }
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (!watch[i]) continue;
    Serial.printf("  GPIO%-2d %s  %lu edges%s\r\n", cand[i], last[i] ? "high" : "low ", (unsigned long)edges[i],
                  cand[i] == g_settings.gpsRx ? "  (gps rx now)" : cand[i] == g_settings.gpsPps ? "  (gps pps now)" : "");
    if (edges[i] > 50 && (best < 0 || edges[i] > edges[best])) best = i;
  }
  if (best >= 0)
    Serial.printf("GPS TX looks like GPIO%d (serial data makes hundreds of edges). Use: set gps %d none %d\r\n",
                  cand[best], cand[best], g_settings.gpsPps);
  else
    Serial.print("No busy pin found. A GPS sends data every second, so its TX isn't on any of these pins.\r\n");
  Serial.print("PPS shows 2 edges per second, and only once the GPS has a fix. Reboot after changing pins.\r\n");
}

static void runCommand(char* line) {
  char* cmd = strtok(line, " \t");
  if (!cmd) return;
  char* a1 = strtok(nullptr, " \t");
  char* a2 = strtok(nullptr, " \t");
  char* a3 = strtok(nullptr, " \t");
  char* a4 = strtok(nullptr, " \t");

  if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
    help();
  } else if (!strcasecmp(cmd, "status")) {
    app::printStatus(Serial);
  } else if (!strcasecmp(cmd, "who")) {
    net::printWho(Serial);
  } else if (!strcasecmp(cmd, "gps")) {
    gps::printStatus(Serial);
  } else if (!strcasecmp(cmd, "text")) {
    s_mode = Mode::Text;
    Serial.printf("Text mode. Messages go to all nodes in net %s (max %d chars). Type exit to leave.\r\n",
                  g_settings.call, PKT_TEXT_MAX);
  } else if (!strcasecmp(cmd, "test")) {
    if (a1 && !strcasecmp(a1, "on")) {
      app::setTestMode(true);
      Serial.print("Test mode on. Type 'test off' to stop, 'stats' for a summary.\r\n");
    } else if (a1 && !strcasecmp(a1, "off")) {
      app::setTestMode(false);
      Serial.print("Test mode off.\r\n");
    } else {
      Serial.printf("Test mode is %s. usage: test on|off\r\n", app::testMode() ? "on" : "off");
    }
  } else if (!strcasecmp(cmd, "stats")) {
    if (a1 && !strcasecmp(a1, "reset")) {
      app::resetStats();
      Serial.print("Stats cleared.\r\n");
    } else {
      app::printStats(Serial);
    }
  } else if (!strcasecmp(cmd, "audio")) {
    cmdAudio(a1, a2, a3 != nullptr);
  } else if (!strcasecmp(cmd, "set") && a1 && !strcasecmp(a1, "gps")) {
    cmdSetGps(a2, a3, a4, strtok(nullptr, " \t") != nullptr);
  } else if (!strcasecmp(cmd, "set")) {
    cmdSet(a1, a2, a3 != nullptr);
  } else if (!strcasecmp(cmd, "pinscan")) {
    pinScan();
  } else if (!strcasecmp(cmd, "reboot")) {
    Serial.print("Rebooting...\r\n");
    Serial.flush();
    ESP.restart();
  } else {
    Serial.printf("Unknown command '%s'. Type help.\r\n", cmd);
  }
}

static bool isExitWord(const char* s) {
  static const char* words[] = {"/exit", "/quit", "/q", "exit", "quit"};
  for (const char* w : words)
    if (!strcasecmp(s, w)) return true;
  return false;
}

static void handleLine() {
  s_line[s_len] = 0;
  // Trim leading/trailing spaces so " /exit " still works.
  while (s_len > 0 && s_line[s_len - 1] == ' ') s_line[--s_len] = 0;
  size_t lead = 0;
  while (s_line[lead] == ' ') lead++;
  if (lead) {
    memmove(s_line, s_line + lead, s_len - lead + 1);
    s_len -= lead;
  }
  Serial.print("\r\n");
  if (s_mode == Mode::Text) {
    if (isExitWord(s_line)) {
      s_mode = Mode::Command;
      Serial.print("Back to command mode.\r\n");
    } else if (s_len > 0) {
      const char* why;
      if (!app::canSend(why)) {
        // Don't queue: a backlog would all go out at once when the node joins.
        Serial.printf("(not sent: %s - type exit, then status)\r\n", why);
      } else {
        if (s_len > PKT_TEXT_MAX) Serial.printf("(cut to %d chars)\r\n", PKT_TEXT_MAX);
      }
      if (!*why && app::sendText(s_line)) {
        char shown[PKT_TEXT_MAX + 1];
        strlcpy(shown, s_line, sizeof(shown));
        Serial.printf("[me] %s\r\n", shown);
      } else if (!*why) {
        Serial.print("(send queue full, try again)\r\n");
      }
    }
  } else {
    runCommand(s_line);
  }
  s_len = 0;
  showPrompt();
}

void begin() {
  Serial.printf("\r\nFH-TDMA LoRa node %s, net %s, node %u. Type help.\r\n", FW_VERSION, g_settings.call,
                g_settings.nodeId);
  showPrompt();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' && s_lastWasCR) {  // CRLF: already handled the CR
      s_lastWasCR = false;
      continue;
    }
    s_lastWasCR = (c == '\r');
    if (c == '\r' || c == '\n') {
      handleLine();
    } else if (c == 8 || c == 127) {  // backspace
      if (s_len > 0) {
        s_len--;
        Serial.print("\b \b");
      }
    } else if (c == 3) {  // Ctrl-C leaves text mode / clears the line
      s_len = 0;
      if (s_mode == Mode::Text) {
        s_mode = Mode::Command;
        Serial.print("\r\nBack to command mode.");
      }
      Serial.print("\r\n");
      showPrompt();
    } else if (c >= 32 && c <= 126 && s_len < sizeof(s_line) - 1) {
      s_line[s_len++] = c;
      Serial.write(c);
    }
  }
}

}  // namespace terminal

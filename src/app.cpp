#include "app.h"
#include "audio.h"
#include "voice.h"
#include "config.h"
#include "packet.h"
#include "radio.h"
#include "settings.h"
#include "terminal.h"
#include "timebase.h"
#include "net.h"

namespace app {

static const uint32_t BEACON_INTERVAL_MS = 2000;  // unslotted (hop off) bench beacons

static uint16_t s_seq = 0;        // TEXT and TEST packets (used for loss counting)
static uint16_t s_beaconSeq = 0;  // BEACON packets, counted separately
static volatile bool s_testMode = false;
static uint32_t s_nextTestMs = 0;
static uint32_t s_nextBeaconMs = 0;
static uint32_t s_foreignNet = 0;

struct NodeStats {
  uint32_t rx;
  uint32_t missed;
  uint16_t lastSeq;
  bool seen;
  float rssiSum;
  float lastRssi;
  float lastSnr;
  int32_t lastTimeErrUs;
  int32_t maxAbsTimeErrUs;
  bool haveTimeErr;
  uint32_t beacons;
  uint32_t lastHeardMs;
};
static NodeStats s_stats[256];
static uint32_t s_rxByType[9];  // packets of this net received, by PktType

static uint16_t nextSeq() { return __atomic_fetch_add(&s_seq, 1, __ATOMIC_RELAXED); }

static void track(uint8_t src, uint16_t seq, float rssi, float snr) {
  NodeStats& n = s_stats[src];
  if (n.rx > 0) {
    uint16_t gap = (uint16_t)(seq - n.lastSeq);
    if (gap > 1 && gap < 1000) n.missed += gap - 1;
  }
  n.lastSeq = seq;
  n.rx++;
  n.rssiSum += rssi;
  n.lastRssi = rssi;
  n.lastSnr = snr;
}

static bool timeIsShared() { return g_settings.timeMode != TimeMode::Local && timebase::synced(); }

static void onRx(const uint8_t* data, size_t len, const radio::RxInfo& info) {
  PktHeader h;
  const uint8_t* p;
  size_t plen;
  if (!pktDecode(data, len, h, p, plen)) return;
  if (h.net != netTag()) {
    s_foreignNet++;
    return;
  }
  s_rxByType[h.type < 9 ? h.type : 0]++;
  net::onPacket(h.type, h.src, p, plen, info.slot, info.rssi);
  uint8_t me = net::myAddr();
  // Only meaningful once addresses are real: static slots, or joined to a managed net.
  net::Role r = net::role();
  bool addrReal = r == net::Role::Static || r == net::Role::Member || r == net::Role::Manager;
  static uint32_t s_lastDupWarnMs = 0;
  if (addrReal && me != ADDR_NONE && h.src == me && h.type != PKT_JOIN &&
      (s_lastDupWarnMs == 0 || millis() - s_lastDupWarnMs > 30000)) {  // at most every 30 s
    s_lastDupWarnMs = millis();
    terminal::printAsync("! Heard another node using my address %u.%s", h.src,
                         g_settings.netMode == NetMode::Static ? " Give each node its own ID." : "");
  }
  if (h.src == ADDR_NONE) return;  // JOIN from a node without an address
  NodeStats& n = s_stats[h.src];
  n.seen = true;
  n.lastHeardMs = millis();

  char where[40] = "";
  if (info.slot >= 0)
    snprintf(where, sizeof(where), " slot=%3d ch=%3u", net::position(info.slot), info.channel);

  switch (h.type) {
    case PKT_TEXT: {
      track(h.src, h.seq, info.rssi, info.snr);
      char text[PKT_TEXT_MAX + 1];
      size_t k = plen > PKT_TEXT_MAX ? PKT_TEXT_MAX : plen;
      memcpy(text, p, k);
      text[k] = 0;
      for (size_t i = 0; i < k; i++)
        if (text[i] < 32 || text[i] > 126) text[i] = '?';
      const char* name = net::nameFor(h.src);
      if (name) terminal::printAsync("[%s] %s", name, text);
      else terminal::printMessage(g_settings.call, h.src, text);
      break;
    }
    case PKT_TEST: {
      track(h.src, h.seq, info.rssi, info.snr);
      char line[200];
      int l = snprintf(line, sizeof(line), "RX test  src=%3u seq=%5u%s rssi=%6.1f snr=%5.1f", h.src, h.seq, where,
                       info.rssi, info.snr);
      if (plen >= 8 && timeIsShared()) {
        int64_t err = timebase::toNetwork(info.rxDoneLocalUs) - getI64(p);
        n.lastTimeErrUs = (int32_t)err;
        int32_t a = err < 0 ? (int32_t)-err : (int32_t)err;
        if (a > n.maxAbsTimeErrUs) n.maxAbsTimeErrUs = a;
        n.haveTimeErr = true;
        snprintf(line + l, sizeof(line) - l, " dt=%+ld us", (long)err);
      }
      if (s_testMode) terminal::printAsync("%s", line);
      break;
    }
    case PKT_BEACON: {
      n.beacons++;
      if (g_settings.timeMode == TimeMode::BenchMaster) {
        // Two bench masters split the bench into two clocks that never line up.
        // The lower address stays master; the higher one drops to bench (saved).
        uint8_t me = net::myAddr();
        if (h.src < me) {
          g_settings.timeMode = TimeMode::Bench;
          settingsSave();
          timebase::reset();
          terminal::printAsync("! Another bench master (node %u, lower ID) heard: this node is now time bench (saved)", h.src);
          net::restart();
          // fall through: sync to this beacon right away
        } else {
          static uint32_t lastWarn = 0;
          uint32_t now = millis();
          if (lastWarn == 0 || now - lastWarn > 10000) {
            lastWarn = now ? now : 1;
            if (h.src == me)
              terminal::printAsync("! Another bench master is using my ID %u. Give each node its own ID", me);
            else
              terminal::printAsync("! Another bench master (node %u) heard; it will switch to bench", h.src);
          }
        }
      }
      if (g_settings.timeMode == TimeMode::Bench) {
        // Beacons from two different masters: the bench is split into two clocks.
        static uint8_t lastSrc = 0xFF;
        static uint32_t lastMs = 0, lastWarn = 0;
        uint32_t now = millis();
        if (lastSrc != 0xFF && lastSrc != h.src && now - lastMs < 20000 && (lastWarn == 0 || now - lastWarn > 10000)) {
          lastWarn = now ? now : 1;
          terminal::printAsync("! Time beacons from two bench masters (node %u and node %u). Only one node should be benchmaster",
                               lastSrc, h.src);
        }
        lastSrc = h.src;
        lastMs = now;
      }
      if (plen >= 8 && g_settings.timeMode == TimeMode::Bench) {
        bool first = !timebase::synced();
        timebase::syncFromBeacon(getI64(p), info.rxDoneLocalUs);
        if (first) terminal::printAsync("* Bench time synced to node %u", h.src);
        else if (s_testMode)
          terminal::printAsync("RX beacon src=%3u correction=%+lld us", h.src,
                               (long long)timebase::lastCorrectionUs());
      }
      break;
    }
    case PKT_VOICE:
      voice::onPacket(h.src, h.seq, h.flags, p, plen);
      break;
    default:
      break;
  }
}

// Builds packets for the radio task (runs on core 0).
static size_t buildPacket(uint8_t type, uint16_t seq, const uint8_t* payload, size_t plen, uint8_t* out,
                          size_t max) {
  PktHeader h = {type, netTag(), net::myAddr(), 0, seq};
  return pktEncode(h, payload, plen, out, max);
}

static size_t buildTest(uint8_t* out, size_t max, int& stampOffset) {
  // Full 48-byte text-size packet so airtime matches real text traffic.
  uint8_t payload[PKT_TEXT_MAX];
  putI64(payload, 0);
  for (size_t i = 8; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
  stampOffset = PKT_HEADER_LEN;
  return buildPacket(PKT_TEST, nextSeq(), payload, sizeof(payload), out, max);
}

static size_t buildBeacon(uint8_t* out, size_t max, int& stampOffset) {
  uint8_t payload[8] = {0};
  stampOffset = PKT_HEADER_LEN;
  return buildPacket(PKT_BEACON, s_beaconSeq++, payload, sizeof(payload), out, max);
}

static size_t slotFiller(radio::SlotKind kind, int64_t slot, uint8_t* buf, size_t max, int& stampOffset) {
  if (kind == radio::SlotKind::Sync) return buildBeacon(buf, max, stampOffset);
  if (s_testMode) return buildTest(buf, max, stampOffset);
  // Idle member: keepalive every 10 frames so the manager keeps our slot.
  if (g_settings.netMode == NetMode::Managed &&
      slot - radio::lastOwnTxSlot() >= (int64_t)10 * net::currentN()) {
    return buildPacket(PKT_ALIVE, 0, nullptr, 0, buf, max);
  }
  return 0;
}

void begin() {
  radio::onReceive(onRx);
  radio::setSlotFiller(slotFiller);
  resetStats();
}

bool canSend(const char*& why) {
  why = "";
  bool unslotted = !g_settings.hop || g_settings.timeMode == TimeMode::Local;
  if (unslotted) return true;
  if (!timebase::synced()) {
    why = g_settings.timeMode == TimeMode::Gps ? "waiting for GPS time" : "waiting for the bench time beacon";
    return false;
  }
  net::Role r = net::role();
  if (r == net::Role::Static || r == net::Role::Member || r == net::Role::Manager) return true;
  why = r == net::Role::Joining ? "still joining the net" : "no net yet, listening for a manager";
  return false;
}

bool sendText(const char* text) {
  size_t n = strlen(text);
  if (n > PKT_TEXT_MAX) n = PKT_TEXT_MAX;
  uint8_t buf[PKT_MAX_LEN];
  size_t len = buildPacket(PKT_TEXT, nextSeq(), (const uint8_t*)text, n, buf, sizeof(buf));
  return len && radio::send(buf, len);
}

void loop() {
  // With TDMA running, test pings and beacons come from the slot filler.
  // Unslotted (hop off, or local time) they are sent on timers instead.
  if (radio::hopping()) return;
  bool unslottedAllowed = !g_settings.hop || g_settings.timeMode == TimeMode::Local;
  uint32_t now = millis();

  if (s_testMode && unslottedAllowed && (int32_t)(now - s_nextTestMs) >= 0) {
    uint8_t buf[PKT_MAX_LEN];
    int stamp = -1;
    size_t len = buildTest(buf, sizeof(buf), stamp);
    radio::send(buf, len, stamp);
    // Random jitter keeps unsynced nodes from colliding every time.
    s_nextTestMs = now + g_settings.testIntervalMs + random(0, 200);
  }

  if (g_settings.timeMode == TimeMode::BenchMaster && !g_settings.hop && (int32_t)(now - s_nextBeaconMs) >= 0) {
    uint8_t buf[PKT_MAX_LEN];
    int stamp = -1;
    size_t len = buildBeacon(buf, sizeof(buf), stamp);
    radio::send(buf, len, stamp);
    s_nextBeaconMs = now + BEACON_INTERVAL_MS;
  }
}

void setTestMode(bool on) {
  s_testMode = on;
  s_nextTestMs = millis() + random(0, 500);
}

bool testMode() { return s_testMode; }

void resetStats() {
  memset(s_stats, 0, sizeof(s_stats));
  s_foreignNet = 0;
}

void printStats(Print& out) {
  out.print("addr name        rx  missed  loss%  avgRSSI  lastSNR  lastDt(us)  max|dt|(us)  beacons  heard(s ago)\r\n");
  int shown = 0;
  for (int i = 0; i < 256; i++) {
    const NodeStats& n = s_stats[i];
    if (!n.seen) continue;
    float loss = (n.rx + n.missed) ? 100.0f * n.missed / (float)(n.rx + n.missed) : 0.0f;
    const char* nm = net::nameFor((uint8_t)i);
    out.printf("%4d %-8s %5lu  %6lu  %5.1f  %7.1f  %7.1f  ", i, nm ? nm : "", (unsigned long)n.rx, (unsigned long)n.missed, loss,
               n.rx ? n.rssiSum / n.rx : 0.0f, n.lastSnr);
    if (n.haveTimeErr) out.printf("%+10ld  %11ld", (long)n.lastTimeErrUs, (long)n.maxAbsTimeErrUs);
    else out.print("         -            -");
    out.printf("  %7lu  %12lu\r\n", (unsigned long)n.beacons, (unsigned long)((millis() - n.lastHeardMs) / 1000));
    shown++;
  }
  if (!shown) out.print("(no nodes heard yet)\r\n");
  const radio::Counters& c = radio::counters();
  out.printf("radio: tx %lu, tx fail %lu, late %lu, overrun %lu, rx %lu, crc err %lu, queue full %lu, other nets %lu\r\n",
             (unsigned long)c.txOk, (unsigned long)c.txFail, (unsigned long)c.txLate, (unsigned long)c.txOverrun,
             (unsigned long)c.rxOk, (unsigned long)c.rxCrc, (unsigned long)c.queueFull, (unsigned long)s_foreignNet);
  static const char* tn[9] = {"?", "text", "test", "timebcn", "netbcn", "join", "dir", "alive", "voice"};
  out.print("by type:   ");
  for (int i = 1; i < 9; i++) out.printf(" %s %lu/%lu", tn[i], (unsigned long)c.txByType[i], (unsigned long)s_rxByType[i]);
  out.print("  (tx/rx)\r\n");
}

void printStatus(Print& out) {
  out.printf("Firmware:  %s\r\n", FW_VERSION);
  out.printf("Net call:  %s (tag 0x%02X)\r\n", g_settings.call, netTag());
  if (g_settings.netMode == NetMode::Static) {
    out.printf("Node ID:   %u of N=%u (static slots)", g_settings.nodeId, g_settings.slots);
    if (g_settings.nodeId >= g_settings.slots) out.print("  ** id must be below N, this node never transmits **");
    out.print("\r\n");
  }
  net::printStatus(out);
  out.printf("Power:     %d dBm\r\n", g_settings.powerDbm);
  out.printf("Radio:     SF%d BW%.0f kHz CR4/%d, preamble %d, %d-byte airtime %.1f ms, TX latency %ld us\r\n",
             LORA_SF, LORA_BW_KHZ, LORA_CR, LORA_PREAMBLE, PKT_MAX_LEN, radio::airtimeUs(PKT_MAX_LEN) / 1000.0f,
             (long)radio::txLatencyUs());
  out.printf("Time mode: %s, %s", timeModeName(g_settings.timeMode), timebase::synced() ? "synced" : "NOT synced");
  if (g_settings.timeMode == TimeMode::Bench || g_settings.timeMode == TimeMode::Gps) {
    uint32_t age = timebase::syncAgeMs();
    if (age != UINT32_MAX)
      out.printf(", last sync %lu ms ago, last correction %+lld us", (unsigned long)age,
                 (long long)timebase::lastCorrectionUs());
  }
  out.print("\r\n");
  int64_t t = timebase::nowUs();
  out.printf("Net time:  %lld.%06lld s\r\n", (long long)(t / 1000000), (long long)(t % 1000000));
  if (radio::hopping()) {
    int64_t s = radio::currentSlot();
    out.printf("Hopping:   yes, slot %lld (frame position %d of %u), channel %u (%.1f MHz)\r\n", (long long)s,
               net::position(s), net::currentN(), radio::currentChannel(),
               radio::channelMHz(radio::currentChannel()));
    out.printf("TDMA:      T=%d ms, G=%d ms, frame %.2f s", SLOT_US / 1000, GUARD_US / 1000,
               net::currentN() * SLOT_US / 1e6);
    if (g_settings.timeMode != TimeMode::Gps) out.printf(", bench sync slot every %d slots on ch %u", SYNC_PERIOD,
                                                          g_settings.channel);
    out.print("\r\n");
  } else if (g_settings.hop && g_settings.timeMode != TimeMode::Local) {
    out.printf("Hopping:   waiting for time sync, listening on ch %u, not transmitting\r\n", g_settings.channel);
  } else {
    out.printf("Hopping:   off, fixed channel %u (%.1f MHz)%s\r\n", g_settings.channel,
               radio::channelMHz(g_settings.channel),
               g_settings.hop ? " (hop needs time mode bench, benchmaster or gps)" : "");
  }
  out.printf("Test mode: %s\r\n", s_testMode ? "on" : "off");
  audio::printStatus(out);
  voice::printStatus(out);
}

}  // namespace app

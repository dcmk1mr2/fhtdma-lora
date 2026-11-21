// Compiles one private copy of net.cpp per simulated node.
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define g_settings CAT(g_settings_, NODE)
#define net CAT(net_, NODE)
#define terminal CAT(terminal_, NODE)
#define ESP CAT(ESP_, NODE)
#include "../../../src/net.cpp"
#include "simapi.h"

Settings g_settings;
EspClass ESP;
namespace terminal {
void printAsync(const char* fmt, ...) {
  if (!g_simVerbose) return;
  va_list a;
  va_start(a, fmt);
  printf("%8.2fs %-6s ", g_simTimeUs / 1e6, g_settings.name);
  vprintf(fmt, a);
  printf("\n");
  va_end(a);
}
void printMessage(const char*, uint8_t, const char*) {}
}  // namespace terminal

static void apiInit(uint64_t mac, const char* name, uint16_t n) {
  memset(&g_settings, 0, sizeof(g_settings));
  strlcpy(g_settings.call, "W6MRR", sizeof(g_settings.call));
  strlcpy(g_settings.name, name, sizeof(g_settings.name));
  g_settings.slots = n;
  g_settings.hop = true;
  g_settings.timeMode = TimeMode::Gps;
  g_settings.netMode = NetMode::Managed;
  g_settings.listenSec = 20;
  ESP.mac = mac;
  net::begin();
}
static int apiPlan(int64_t s, uint8_t* buf, size_t& len) { return (int)net::planSlot(s, buf, PKT_MAX_LEN, len); }
static void apiRx(uint8_t type, uint8_t src, const uint8_t* p, size_t plen, int64_t slot) {
  net::onPacket(type, src, p, plen, slot, -50);
}
static int apiRole() { return (int)net::role(); }
static uint8_t apiAddr() { return net::myAddr(); }
static int apiPos(int64_t s) { return net::position(s); }
static uint16_t apiN() { return net::currentN(); }
static uint32_t apiId() { return net::permId(); }
static void apiRename(const char* n) { strlcpy(g_settings.name, n, sizeof(g_settings.name)); net::restart(); }

NodeApi CAT(api_, NODE) = {apiInit, net::loop, apiPlan, apiRx, apiRole, apiAddr, apiPos, apiN, apiId, apiRename};

// Host simulation of the net manager: several nodes share a perfect channel
// (one transmitter per slot is heard by everyone, two or more collide).
// Build and run from this folder:
//   for f in node_*.cpp sim_main.cpp ../../../src/packet.cpp; do g++ -std=gnu++17 -I. -I../../../include -c $f; done
//   g++ *.o -o sim && ./sim
#include "Arduino.h"
#include "simapi.h"
#include "../../../src/packet.h"
#include "../../../src/settings.h"
#include "../../../src/timebase.h"
#include <vector>
#include <set>
#include <map>

int64_t g_simTimeUs = 0;
bool g_simVerbose = true;
uint32_t millis() { return (uint32_t)(g_simTimeUs / 1000); }
long random(long a, long b) { return a + rand() % (b - a); }

// Shared stand-ins for modules not under test.
uint8_t netTag() { return 0x5A; }
const char* timeModeName(TimeMode) { return "gps"; }
namespace timebase {
int64_t localUs() { return g_simTimeUs; }
int64_t nowUs() { return g_simTimeUs; }
int64_t toNetwork(int64_t l) { return l; }
bool synced() { return true; }
}  // namespace timebase

#define X(n) extern NodeApi api_##n;
X(A) X(B) X(C) X(D) X(E) X(F) X(G) X(H) X(I) X(J) X(K) X(L)
#undef X
static NodeApi* apis[] = {&api_A, &api_B, &api_C, &api_D, &api_E, &api_F,
                          &api_G, &api_H, &api_I, &api_J, &api_K, &api_L};
static const char* names[] = {"Alpha", "Bravo", "Charlie", "Delta", "Echo", "Fox",
                              "Golf", "Hotel", "India", "Juliet", "Kilo", "Bravo"};
static const int NN = 12;
static bool alive[NN];
static int group[NN];  // nodes only hear others in the same group
static const int64_t T = 120000;
static int fails = 0;
static uint32_t collisions = 0;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
enum { OFF, STATIC, LISTEN, JOINING, MEMBER, MANAGER };

static void runUntil(double sec) {
  int64_t end = (int64_t)(sec * 1e6);
  while (g_simTimeUs < end) {
    int64_t s = g_simTimeUs / T;
    // slot start (+1 ms): every node runs its main loop, then plans the slot
    g_simTimeUs = s * T + 1000;
    for (int i = 0; i < NN; i++) if (alive[i]) apis[i]->loop();
    uint8_t buf[NN][PKT_MAX_LEN];
    size_t len[NN];
    std::vector<int> tx;
    for (int i = 0; i < NN; i++) {
      if (!alive[i]) continue;
      int use = apis[i]->plan(s, buf[i], len[i]);
      if (use == 1) {  // own slot: send a keepalive-sized packet, like an idle member
        PktHeader h = {PKT_ALIVE, 0x5A, apis[i]->addr(), 0, 0};
        len[i] = pktEncode(h, nullptr, 0, buf[i], PKT_MAX_LEN);
      }
      if (use != 0 && len[i] > 0) tx.push_back(i);
    }
    // delivery at ~100 ms into the slot
    g_simTimeUs = s * T + 100000;
    for (int r = 0; r < NN; r++) {
      if (!alive[r]) continue;
      int heard = -1, count = 0;
      for (int t : tx) if (t != r && group[t] == group[r]) { heard = t; count++; }
      if (count > 1) { collisions++; continue; }
      if (count == 1) {
        PktHeader h; const uint8_t* p; size_t pl;
        if (pktDecode(buf[heard], len[heard], h, p, pl)) apis[r]->rx(h.type, h.src, p, pl, s);
      }
    }
    for (int i = 0; i < NN; i++) if (alive[i]) apis[i]->loop();
    g_simTimeUs = (s + 1) * T;
  }
}

// expectMgr = -1: any one node may be manager, but exactly one.
static int checkNet(const char* when, std::vector<int> expectMembers, int expectMgr) {
  int64_t s = g_simTimeUs / T;
  std::set<int> addrs, poss;
  int mgrs = 0, mgr = -1;
  for (int i : expectMembers) if (apis[i]->role() == MANAGER) { mgrs++; mgr = i; }
  CHECK(mgrs == 1, "%s: %d managers", when, mgrs);
  if (expectMgr >= 0) CHECK(mgr == expectMgr, "%s: manager is %s, expected %s", when, mgr >= 0 ? names[mgr] : "none", names[expectMgr]);
  for (int i : expectMembers) {
    int role = apis[i]->role();
    if (i != mgr) CHECK(role == MEMBER, "%s: %s should be member, role %d", when, names[i], role);
    uint8_t a = apis[i]->addr();
    CHECK(a != 0xFF && !addrs.count(a), "%s: %s bad/duplicate address %u", when, names[i], a);
    addrs.insert(a);
  }
  printf("  %-16s t=%6.1fs mgr=%s N=%u, addresses:", when, g_simTimeUs / 1e6, mgr >= 0 ? names[mgr] : "-",
         mgr >= 0 ? apis[mgr]->n() : 0);
  for (int i : expectMembers) printf(" %s=%u", names[i], apis[i]->addr());
  printf("\n");
  (void)s; (void)poss;
  return mgr;
}

int main() {
  srand(7);
  for (int i = 0; i < NN; i++) {
    apis[i]->init(0x10000000ULL * (i + 1) + 0x1234, names[i], 8);
    alive[i] = false;
    group[i] = 0;
  }
  g_simVerbose = getenv("SIM_QUIET") == nullptr;

  printf("== 1. Four nodes power up, first one starts the net\n");
  alive[0] = true; runUntil(5);
  alive[1] = true; runUntil(10);
  alive[2] = true; alive[3] = true; runUntil(70);
  checkNet("4 nodes", {0, 1, 2, 3}, 0);

  printf("== 2. Seven more join: N must grow from 8 to 16\n");
  for (int i = 4; i < 11; i++) alive[i] = true;
  runUntil(150);
  checkNet("11 nodes", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, 0);
  CHECK(apis[0]->n() >= 16, "N did not grow (N=%u)", apis[0]->n());

  printf("== 3. Duplicate name is refused\n");
  alive[11] = true;  // second "Bravo"
  runUntil(175);
  CHECK(apis[11]->role() == JOINING, "duplicate-name node should stay joining, role %d", apis[11]->role());
  apis[11]->rename("Lima");
  names[11] = "Lima";
  runUntil(205);
  CHECK(apis[11]->role() == MEMBER, "renamed node should join, role %d", apis[11]->role());

  printf("== 4. Manager dies: deputy 1 takes over, nobody rejoins\n");
  uint8_t before[NN];
  for (int i = 0; i < NN; i++) before[i] = apis[i]->addr();
  alive[0] = false;
  runUntil(215);
  int m4 = checkNet("after failover", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, -1);
  for (int i = 1; i < NN; i++) CHECK(apis[i]->addr() == before[i], "%s changed address %u -> %u", names[i], before[i], apis[i]->addr());
  runUntil(260);  // old manager's slot reclaimed after 30 silent frames
  checkNet("after reclaim", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, m4);

  printf("== 5. Two separate nets meet: lower ID manager keeps the role\n");
  // Split off five nodes that don't include the manager.
  std::vector<int> side;
  for (int i = 1; i < NN && side.size() < 5; i++) if (i != m4) side.push_back(i);
  for (int i : side) group[i] = 1;
  runUntil(300);
  int mgr1 = -1;
  for (int i : side) if (apis[i]->role() == MANAGER) mgr1 = i;
  CHECK(mgr1 >= 0, "split group has no manager");
  printf("  split group manager: %s, main manager: %s\n", mgr1 >= 0 ? names[mgr1] : "none", names[m4]);
  int winner = (mgr1 >= 0 && apis[mgr1]->id() < apis[m4]->id()) ? mgr1 : m4;
  for (int i : side) group[i] = 0;  // groups meet again
  runUntil(400);
  checkNet("after merge", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, winner);

  printf("collisions (two senders in one slot): %u\n", collisions);
  printf("%s\n", fails ? "FAILED" : "ALL PASSED");
  return fails ? 1 : 0;
}

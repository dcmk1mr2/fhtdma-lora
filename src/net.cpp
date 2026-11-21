#include "net.h"
#include "config.h"
#include "packet.h"
#include "radio.h"
#include "settings.h"
#include "terminal.h"
#include "timebase.h"

namespace net {

// ---------------------------------------------------------------------------
// Constants

static const uint8_t OWNER_FREE = 0xFF;
static const int MAX_DIR = 64;
static const int ENTRIES_PER_CHUNK = 4;
static const uint32_t SILENT_FRAMES = 30;      // manager frees a node after this
static const uint32_t ALIVE_FRAMES = 10;       // idle member keepalive period
static const uint32_t MISSED_PER_DEPUTY = 3;   // deputy k takes over after 3k missed beacons
static const uint32_t MISSED_GIVE_UP = 12;     // non-deputy goes back to listening
static const uint32_t N_CHANGE_LEAD = 3;       // frames of notice before N changes
static const uint16_t N_MIN = 4;
static const uint16_t N_MAX = 256;

// Beacon payload layout (max 42 bytes)
enum : int {
  B_MGRID = 0,    // 4  manager permanent ID
  B_N = 4,        // 2  current N
  B_S0 = 6,       // 4  current S0, low 32 bits
  B_NEXTN = 10,   // 2  next N (0 = no change pending)
  B_NEXTS0 = 12,  // 4  slot where next N starts, low 32 bits
  B_VER = 16,     // 1  slot map version
  B_CHUNK = 17,   // 1  chunk index
  B_CHUNKS = 18,  // 1  chunk count
  B_DEP = 19,     // 3  deputy short addresses
  B_FLAGS = 22,   // 1
  B_ACKID = 23,   // 4  join ack: permanent ID
  B_ACKADDR = 27, // 1  join ack: short address (ADDR_NONE = no ack, 0 = name taken)
  B_ENTRIES = 28, // 4 x 3: short address, first position, count
  B_LEN = 40,
};

// ---------------------------------------------------------------------------
// Shared schedule (read by the radio task, written by the main loop)

struct Schedule {
  bool active;      // we know N and S0
  bool manager;
  bool staticMode;
  uint16_t n;
  int64_t s0;
  uint16_t nextN;   // 0 = none
  int64_t nextS0;
  uint8_t myAddr;
  uint8_t owner[256];  // owner short address per frame position
  uint8_t beacon[PKT_MAX_LEN];
  uint8_t beaconLen;
  int64_t schedSlot;   // one-shot packet (JOIN or DIRECTORY) for this slot
  uint8_t sched[PKT_MAX_LEN];
  uint8_t schedLen;
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static Schedule s_sch;

// ---------------------------------------------------------------------------
// Main-loop state

struct DirEntry {
  uint32_t permId;
  uint8_t addr;
  char name[9];
  uint32_t lastHeardMs;
  uint32_t lastHeardFrame;  // manager only
  uint32_t joinOrder;
  bool active;
};

static Role s_role = Role::Off;
static uint32_t s_permId = 0;
static uint16_t s_seq = 0;
static DirEntry s_dir[MAX_DIR];
static uint32_t s_joinCounter = 0;

// timing
static int64_t s_lastFrame = -1;
static uint32_t s_frameCount = 0;
static uint32_t s_listenUntilMs = 0;
static uint32_t s_claimAtMs = 0;
static uint32_t s_missedBeacons = 0;
static bool s_beaconThisFrame = false;

// member
static uint32_t s_mgrId = 0;
static uint8_t s_mgrAddr = ADDR_NONE;
static uint8_t s_deputies[3] = {ADDR_NONE, ADDR_NONE, ADDR_NONE};
static uint8_t s_mapVer = 0;
static bool s_haveMap = false;
static uint8_t s_pendVer = 0;
static uint8_t s_pendChunks = 0;
static uint8_t s_pendGot[32];  // bitmap of chunks received for s_pendVer
static uint8_t s_pendOwner[256];
static uint32_t s_joinNextFrame = 0;
static bool s_nameRejected = false;

// manager
static uint8_t s_chunkIdx = 0;
struct Ack {
  uint32_t permId;
  uint8_t addr;
  uint8_t repeats;
};
static Ack s_acks[8];
static int s_ackCount = 0;
static int s_dirCursor = 0;

// ---------------------------------------------------------------------------
// Helpers

static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static uint16_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int64_t fromLow32(uint32_t low, int64_t near) {
  int64_t v = (near & ~(int64_t)0xFFFFFFFFLL) | low;
  if (v > near + 0x80000000LL) v -= 0x100000000LL;
  else if (v < near - 0x80000000LL) v += 0x100000000LL;
  return v;
}

static int64_t nowSlot() { return timebase::nowUs() / SLOT_US; }

static int positionLocked(int64_t s) {
  if (!s_sch.active || s_sch.n == 0) return -1;
  uint16_t n = s_sch.n;
  int64_t s0 = s_sch.s0;
  if (s_sch.nextN && s >= s_sch.nextS0) {
    n = s_sch.nextN;
    s0 = s_sch.nextS0;
  }
  int64_t d = (s - s0) % n;
  if (d < 0) d += n;
  return (int)d;
}

static size_t buildPkt(uint8_t type, uint8_t src, const uint8_t* payload, size_t plen, uint8_t* out) {
  PktHeader h = {type, netTag(), src, 0, s_seq++};
  return pktEncode(h, payload, plen, out, PKT_MAX_LEN);
}

static DirEntry* dirFind(uint32_t permId) {
  for (auto& e : s_dir)
    if (e.permId == permId && e.permId) return &e;
  return nullptr;
}

static DirEntry* dirByAddr(uint8_t addr) {
  for (auto& e : s_dir)
    if (e.permId && e.addr == addr && e.active) return &e;
  return nullptr;
}

static DirEntry* dirUpsert(uint32_t permId, uint8_t addr, const char* name) {
  DirEntry* e = dirFind(permId);
  if (!e) {
    for (auto& x : s_dir)
      if (!x.permId) { e = &x; break; }
    if (!e) {  // full: reuse the oldest inactive entry
      for (auto& x : s_dir)
        if (!x.active) { e = &x; break; }
    }
    if (!e) return nullptr;
    memset(e, 0, sizeof(*e));
    e->permId = permId;
    e->joinOrder = ++s_joinCounter;
  }
  // Another entry may still claim this address from an earlier assignment.
  for (auto& x : s_dir)
    if (&x != e && x.addr == addr && x.active) x.active = false;
  e->addr = addr;
  e->active = true;
  if (name && name[0]) strlcpy(e->name, name, sizeof(e->name));
  e->lastHeardMs = millis();
  e->lastHeardFrame = s_frameCount;
  return e;
}

static bool addrInMap(uint8_t addr) {
  bool used = false;
  portENTER_CRITICAL(&s_mux);
  for (int p = 1; p < 256; p++)
    if (s_sch.owner[p] == addr) used = true;
  portEXIT_CRITICAL(&s_mux);
  return used;
}

static void setRole(Role r) {
  if (r == s_role) return;
  s_role = r;
  terminal::printAsync("* Net: %s", roleName(r));
}

static void resetSchedule() {
  portENTER_CRITICAL(&s_mux);
  s_sch.active = false;
  s_sch.manager = false;
  s_sch.myAddr = ADDR_NONE;
  s_sch.nextN = 0;
  s_sch.beaconLen = 0;
  s_sch.schedLen = 0;
  memset(s_sch.owner, OWNER_FREE, sizeof(s_sch.owner));
  portEXIT_CRITICAL(&s_mux);
  s_haveMap = false;
  s_pendChunks = 0;
}

static void startListening() {
  resetSchedule();
  s_missedBeacons = 0;
  s_lastFrame = -1;
  s_listenUntilMs = millis() + (uint32_t)g_settings.listenSec * 1000;
  s_claimAtMs = 0;
  setRole(Role::Listen);
}

// Pick a random free position in the next frame and schedule pkt there.
static bool scheduleInFreeSlot(const uint8_t* pkt, size_t len) {
  int64_t s = nowSlot();
  portENTER_CRITICAL(&s_mux);
  uint16_t n = s_sch.n;
  static int freePos[256];
  int nf = 0;
  for (int p = 1; p < n; p++)
    if (s_sch.owner[p] == OWNER_FREE) freePos[nf++] = p;
  portEXIT_CRITICAL(&s_mux);
  if (!nf || !n) return false;
  int p = freePos[random(0, nf)];
  // First absolute slot after s+1 with position p.
  portENTER_CRITICAL(&s_mux);
  int cur = positionLocked(s + 2);
  int64_t target = s + 2 + ((p - cur) % n + n) % n;
  s_sch.schedSlot = target;
  memcpy(s_sch.sched, pkt, len);
  s_sch.schedLen = (uint8_t)len;
  portEXIT_CRITICAL(&s_mux);
  return true;
}

// ---------------------------------------------------------------------------
// Manager

static void becomeManager(bool keepMap) {
  int64_t s = nowSlot();
  portENTER_CRITICAL(&s_mux);
  if (!keepMap || !s_sch.active) {
    s_sch.n = g_settings.slots < N_MIN ? N_MIN : g_settings.slots;
    // S0 odd keeps the bench sync slot (every 32nd) off position 0 for even N.
    int64_t s0 = s + 2;
    if ((s0 & 1) == 0) s0++;
    s_sch.s0 = s0;
    s_sch.nextN = 0;
    memset(s_sch.owner, OWNER_FREE, sizeof(s_sch.owner));
  }
  s_sch.active = true;
  s_sch.manager = true;
  uint8_t me = s_sch.myAddr;
  portEXIT_CRITICAL(&s_mux);

  if (!keepMap) {
    for (auto& e : s_dir) e.active = false;
  } else {
    // Taking over: make sure every slot owner has a directory entry, so
    // silent ones can be reclaimed later. Unknown names get a placeholder.
    for (int p = 1; p < 256; p++) {
      uint8_t a = s_sch.owner[p];
      if (a == OWNER_FREE || dirByAddr(a)) continue;
      char nm[9];
      snprintf(nm, sizeof(nm), "addr%u", a);
      dirUpsert(0xFF000000u | a, a, nm);
    }
    for (auto& e : s_dir) e.lastHeardFrame = s_frameCount;
  }
  if (me == ADDR_NONE) {
    // Take address 1 and position 1 if free, otherwise the first free ones.
    me = 1;
    while (dirByAddr(me) && me < 254) me++;
    portENTER_CRITICAL(&s_mux);
    s_sch.myAddr = me;
    for (int p = 1; p < s_sch.n; p++)
      if (s_sch.owner[p] == OWNER_FREE) { s_sch.owner[p] = me; break; }
    portEXIT_CRITICAL(&s_mux);
  }
  dirUpsert(s_permId, me, g_settings.name);
  s_mapVer++;
  s_missedBeacons = 0;
  s_ackCount = 0;
  setRole(Role::Manager);
}

static void managerAllocate(uint32_t permId, const char* name) {
  // Name must be unique among active nodes.
  for (auto& e : s_dir) {
    if (e.active && e.permId != permId && name[0] && !strncasecmp(e.name, name, 8)) {
      if (s_ackCount < 8) s_acks[s_ackCount++] = {permId, 0, 3};
      return;
    }
  }
  DirEntry* existing = dirFind(permId);
  uint8_t addr = (existing && existing->active) ? existing->addr : ADDR_NONE;
  portENTER_CRITICAL(&s_mux);
  bool hasSlot = false;
  if (addr != ADDR_NONE)
    for (int p = 1; p < s_sch.n; p++)
      if (s_sch.owner[p] == addr) hasSlot = true;
  int freeP = -1;
  if (!hasSlot)
    for (int p = 1; p < s_sch.n; p++)
      if (s_sch.owner[p] == OWNER_FREE) { freeP = p; break; }
  portEXIT_CRITICAL(&s_mux);

  if (!hasSlot && freeP < 0) {
    // Net full: grow N (doubling) at a frame boundary N_CHANGE_LEAD frames ahead.
    portENTER_CRITICAL(&s_mux);
    if (!s_sch.nextN && s_sch.n < N_MAX) {
      int64_t s = nowSlot();
      int64_t frames = (s - s_sch.s0) / s_sch.n + 1 + N_CHANGE_LEAD;
      s_sch.nextS0 = s_sch.s0 + frames * s_sch.n;
      s_sch.nextN = (uint16_t)(s_sch.n * 2 > N_MAX ? N_MAX : s_sch.n * 2);
    }
    portEXIT_CRITICAL(&s_mux);
    terminal::printAsync("* Net full, growing N at the next change (node will retry)");
    return;
  }
  if (addr == ADDR_NONE) {
    addr = 2;
    while ((dirByAddr(addr) || addr == s_sch.myAddr || addrInMap(addr)) && addr < 254) addr++;
  }
  if (!hasSlot) {
    portENTER_CRITICAL(&s_mux);
    s_sch.owner[freeP] = addr;
    portEXIT_CRITICAL(&s_mux);
  }
  DirEntry* e = dirUpsert(permId, addr, name);
  if (e) e->lastHeardFrame = s_frameCount;
  s_mapVer++;
  bool queued = false;
  for (int i = 0; i < s_ackCount; i++)
    if (s_acks[i].permId == permId) { s_acks[i] = {permId, addr, 3}; queued = true; }
  if (!queued && s_ackCount < 8) s_acks[s_ackCount++] = {permId, addr, 3};
  terminal::printAsync("* %s joined as address %u", name[0] ? name : "node", addr);
}

static void managerReclaim() {
  bool changed = false;
  for (auto& e : s_dir) {
    if (!e.active || e.addr == s_sch.myAddr) continue;
    if (s_frameCount - e.lastHeardFrame > SILENT_FRAMES) {
      e.active = false;
      portENTER_CRITICAL(&s_mux);
      for (int p = 1; p < 256; p++)
        if (s_sch.owner[p] == e.addr) s_sch.owner[p] = OWNER_FREE;
      portEXIT_CRITICAL(&s_mux);
      changed = true;
      terminal::printAsync("* %s (address %u) silent for %lu frames, slot freed", e.name, e.addr,
                           (unsigned long)SILENT_FRAMES);
    }
  }
  if (changed) s_mapVer++;
}

static void managerPickDeputies() {
  uint8_t best[3] = {ADDR_NONE, ADDR_NONE, ADDR_NONE};
  uint32_t order[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
  for (auto& e : s_dir) {
    if (!e.active || e.addr == s_sch.myAddr) continue;
    for (int k = 0; k < 3; k++) {
      if (e.joinOrder < order[k]) {
        for (int j = 2; j > k; j--) { order[j] = order[j - 1]; best[j] = best[j - 1]; }
        order[k] = e.joinOrder;
        best[k] = e.addr;
        break;
      }
    }
  }
  memcpy(s_deputies, best, 3);
}

static void managerBuildBeacon() {
  uint8_t pl[B_LEN];
  memset(pl, 0, sizeof(pl));
  put32(pl + B_MGRID, s_permId);

  // Collect allocations (address, position, count=1) for the map chunks.
  uint8_t entries[256][3];
  int ne = 0;
  portENTER_CRITICAL(&s_mux);
  uint16_t n = s_sch.n;
  put16(pl + B_N, n);
  put32(pl + B_S0, (uint32_t)s_sch.s0);
  put16(pl + B_NEXTN, s_sch.nextN);
  put32(pl + B_NEXTS0, (uint32_t)s_sch.nextS0);
  for (int p = 1; p < 256; p++) {
    if (s_sch.owner[p] != OWNER_FREE) {
      entries[ne][0] = s_sch.owner[p];
      entries[ne][1] = (uint8_t)p;
      entries[ne][2] = 1;
      ne++;
    }
  }
  portEXIT_CRITICAL(&s_mux);

  int chunks = (ne + ENTRIES_PER_CHUNK - 1) / ENTRIES_PER_CHUNK;
  if (chunks == 0) chunks = 1;
  if (s_chunkIdx >= chunks) s_chunkIdx = 0;
  pl[B_VER] = s_mapVer;
  pl[B_CHUNK] = s_chunkIdx;
  pl[B_CHUNKS] = (uint8_t)chunks;
  memcpy(pl + B_DEP, s_deputies, 3);
  pl[B_FLAGS] = 0;
  pl[B_ACKADDR] = ADDR_NONE;
  if (s_ackCount) {
    Ack& a = s_acks[0];
    put32(pl + B_ACKID, a.permId);
    pl[B_ACKADDR] = a.addr;
    if (--a.repeats == 0) {
      memmove(&s_acks[0], &s_acks[1], sizeof(Ack) * (s_ackCount - 1));
      s_ackCount--;
    } else if (s_ackCount > 1) {  // rotate so every pending ack gets airtime
      Ack t = s_acks[0];
      memmove(&s_acks[0], &s_acks[1], sizeof(Ack) * (s_ackCount - 1));
      s_acks[s_ackCount - 1] = t;
    }
  }
  memset(pl + B_ENTRIES, 0xFF, 12);
  for (int i = 0; i < ENTRIES_PER_CHUNK; i++) {
    int k = s_chunkIdx * ENTRIES_PER_CHUNK + i;
    if (k < ne) memcpy(pl + B_ENTRIES + 3 * i, entries[k], 3);
  }
  s_chunkIdx = (uint8_t)((s_chunkIdx + 1) % chunks);

  uint8_t pkt[PKT_MAX_LEN];
  size_t len = buildPkt(PKT_NET, s_sch.myAddr, pl, B_LEN, pkt);
  portENTER_CRITICAL(&s_mux);
  memcpy(s_sch.beacon, pkt, len);
  s_sch.beaconLen = (uint8_t)len;
  portEXIT_CRITICAL(&s_mux);
}

static void managerSendDirectory() {
  int active = 0;
  for (auto& e : s_dir) if (e.active) active++;
  if (!active) return;
  uint8_t pl[39];
  int k = 0;
  for (int tries = 0; tries < MAX_DIR && k < 3; tries++) {
    DirEntry& e = s_dir[s_dirCursor];
    s_dirCursor = (s_dirCursor + 1) % MAX_DIR;
    if (!e.active) continue;
    put32(pl + 13 * k, e.permId);
    pl[13 * k + 4] = e.addr;
    memset(pl + 13 * k + 5, 0, 8);
    memcpy(pl + 13 * k + 5, e.name, strnlen(e.name, 8));
    k++;
  }
  uint8_t pkt[PKT_MAX_LEN];
  size_t len = buildPkt(PKT_DIR, s_sch.myAddr, pl, 13 * k, pkt);
  scheduleInFreeSlot(pkt, len);
}

static void managerOnFrame() {
  // Apply a pending N change once it has taken effect.
  int64_t s = nowSlot();
  portENTER_CRITICAL(&s_mux);
  if (s_sch.nextN && s >= s_sch.nextS0) {
    s_sch.n = s_sch.nextN;
    s_sch.s0 = s_sch.nextS0;
    s_sch.nextN = 0;
  }
  portEXIT_CRITICAL(&s_mux);
  DirEntry* me = dirFind(s_permId);
  if (me) { me->lastHeardFrame = s_frameCount; me->lastHeardMs = millis(); }
  managerReclaim();
  managerPickDeputies();
  managerBuildBeacon();
  if (s_frameCount % 2 == 0) managerSendDirectory();
}

// ---------------------------------------------------------------------------
// Member / joining

static void sendJoin() {
  uint8_t pl[12];
  put32(pl, s_permId);
  memset(pl + 4, 0, 8);
  memcpy(pl + 4, g_settings.name, strnlen(g_settings.name, 8));
  uint8_t pkt[PKT_MAX_LEN];
  size_t len = buildPkt(PKT_JOIN, ADDR_NONE, pl, sizeof(pl), pkt);
  scheduleInFreeSlot(pkt, len);
}

static int myDeputyRank() {
  uint8_t me = s_sch.myAddr;
  if (me == ADDR_NONE) return 0;
  for (int k = 0; k < 3; k++)
    if (s_deputies[k] == me) return k + 1;
  return 0;
}

static void memberOnFrame() {
  // Pending N change.
  int64_t s = nowSlot();
  portENTER_CRITICAL(&s_mux);
  if (s_sch.nextN && s >= s_sch.nextS0) {
    s_sch.n = s_sch.nextN;
    s_sch.s0 = s_sch.nextS0;
    s_sch.nextN = 0;
  }
  portEXIT_CRITICAL(&s_mux);

  if (s_beaconThisFrame) s_missedBeacons = 0;
  else s_missedBeacons++;
  s_beaconThisFrame = false;

  int rank = myDeputyRank();
  if (s_role == Role::Member && rank && s_missedBeacons >= MISSED_PER_DEPUTY * rank) {
    terminal::printAsync("* No beacon for %lu frames, deputy %d taking over as manager",
                         (unsigned long)s_missedBeacons, rank);
    becomeManager(true);
    return;
  }
  if (s_missedBeacons >= MISSED_GIVE_UP) {
    terminal::printAsync("* Lost the net manager, listening again");
    startListening();
    return;
  }
  if (s_role == Role::Joining && s_haveMap && !s_nameRejected && s_frameCount >= s_joinNextFrame) {
    sendJoin();
    s_joinNextFrame = s_frameCount + 2 + random(0, 4);  // wait for ack, then random backoff
  }
}

static void onBeacon(uint8_t src, const uint8_t* p, size_t plen, int64_t slot) {
  if (plen < B_LEN) return;
  uint32_t mgrId = get32(p + B_MGRID);

  if (s_role == Role::Manager) {
    if (mgrId == s_permId) return;
    if (mgrId < s_permId) {
      terminal::printAsync("* Heard manager %08lX with a lower ID, stepping down", (unsigned long)mgrId);
      startListening();
    }
    return;  // otherwise the other manager steps down
  }

  int64_t ref = slot >= 0 ? slot : nowSlot();
  uint16_t n = get16(p + B_N);
  if (n < 2) return;
  portENTER_CRITICAL(&s_mux);
  s_sch.active = true;
  s_sch.manager = false;
  s_sch.n = n;
  s_sch.s0 = fromLow32(get32(p + B_S0), ref);
  s_sch.nextN = get16(p + B_NEXTN);
  s_sch.nextS0 = fromLow32(get32(p + B_NEXTS0), ref);
  portEXIT_CRITICAL(&s_mux);

  s_mgrId = mgrId;
  s_mgrAddr = src;
  memcpy(s_deputies, p + B_DEP, 3);
  s_beaconThisFrame = true;
  s_missedBeacons = 0;

  // Slot map chunks.
  uint8_t ver = p[B_VER], idx = p[B_CHUNK], cnt = p[B_CHUNKS];
  if (cnt == 0 || idx >= cnt) cnt = 1, idx = 0;
  if (ver != s_pendVer || cnt != s_pendChunks) {
    s_pendVer = ver;
    s_pendChunks = cnt;
    memset(s_pendGot, 0, sizeof(s_pendGot));
    memset(s_pendOwner, OWNER_FREE, sizeof(s_pendOwner));
  }
  for (int i = 0; i < ENTRIES_PER_CHUNK; i++) {
    const uint8_t* e = p + B_ENTRIES + 3 * i;
    if (e[0] == OWNER_FREE) continue;
    for (int k = 0; k < e[2] && e[1] + k < 256; k++) s_pendOwner[e[1] + k] = e[0];
  }
  s_pendGot[idx / 8] |= 1 << (idx % 8);
  bool complete = true;
  for (int i = 0; i < cnt; i++)
    if (!(s_pendGot[i / 8] & (1 << (i % 8)))) complete = false;
  if (complete && (!s_haveMap || s_mapVer != ver)) {
    portENTER_CRITICAL(&s_mux);
    memcpy(s_sch.owner, s_pendOwner, sizeof(s_sch.owner));
    portEXIT_CRITICAL(&s_mux);
    s_mapVer = ver;
    s_haveMap = true;
    // Dropped from the map while we thought we were a member?
    if (s_role == Role::Member) {
      bool mine = false;
      for (int q = 1; q < 256; q++)
        if (s_pendOwner[q] == s_sch.myAddr) mine = true;
      if (!mine) {
        terminal::printAsync("* My slot was taken back by the manager, rejoining");
        portENTER_CRITICAL(&s_mux);
        s_sch.myAddr = ADDR_NONE;
        portEXIT_CRITICAL(&s_mux);
        setRole(Role::Joining);
        s_joinNextFrame = s_frameCount;
      }
    }
  }

  // Join ack.
  uint8_t ackAddr = p[B_ACKADDR];
  if (ackAddr != ADDR_NONE && get32(p + B_ACKID) == s_permId && s_role == Role::Joining) {
    if (ackAddr == 0) {
      if (!s_nameRejected)
        terminal::printAsync("! The name %s is already used on this net. Pick another with: set name <name>",
                             g_settings.name);
      s_nameRejected = true;
    } else {
      portENTER_CRITICAL(&s_mux);
      s_sch.myAddr = ackAddr;
      portEXIT_CRITICAL(&s_mux);
      dirUpsert(s_permId, ackAddr, g_settings.name);
      setRole(Role::Member);
    }
  }

  if (s_role == Role::Listen) {
    setRole(Role::Joining);
    s_joinNextFrame = s_frameCount;
    s_nameRejected = false;
  }
}

// ---------------------------------------------------------------------------
// Public

void begin() {
  uint64_t mac = ESP.getEfuseMac();
  s_permId = (uint32_t)(mac >> 16) ^ (uint32_t)mac;  // fold all 48 bits into 32
  if (!s_permId) s_permId = 1;
  if (!g_settings.name[0]) snprintf(g_settings.name, sizeof(g_settings.name), "N-%04X", (unsigned)(s_permId & 0xFFFF));
  memset(&s_sch, 0, sizeof(s_sch));
  memset(s_sch.owner, OWNER_FREE, sizeof(s_sch.owner));
  s_sch.myAddr = ADDR_NONE;
  s_sch.schedSlot = -1;
}

void nameChanged() {
  if (s_role == Role::Manager) {
    // Rename in place: the directory goes out with the new name, nobody rejoins.
    dirUpsert(s_permId, s_sch.myAddr, g_settings.name);
  } else if (s_role == Role::Member || s_role == Role::Joining) {
    restart();  // rejoin so the manager checks the new name for duplicates
  }
  s_nameRejected = false;
}

void restart() {
  s_role = Role::Off;  // loop() picks the right state again
  resetSchedule();
}

void loop() {
  bool slotted = g_settings.hop && g_settings.timeMode != TimeMode::Local && timebase::synced();
  if (!slotted) {
    if (s_role != Role::Off) {
      resetSchedule();
      setRole(Role::Off);
    }
    return;
  }
  if (g_settings.netMode == NetMode::Static) {
    if (s_role != Role::Static) {
      portENTER_CRITICAL(&s_mux);
      s_sch.staticMode = true;
      s_sch.active = true;
      s_sch.n = g_settings.slots;
      s_sch.s0 = 0;
      s_sch.nextN = 0;
      s_sch.myAddr = g_settings.nodeId;
      portEXIT_CRITICAL(&s_mux);
      setRole(Role::Static);
    }
    return;
  }
  if (s_role == Role::Static || s_role == Role::Off) {
    portENTER_CRITICAL(&s_mux);
    s_sch.staticMode = false;
    portEXIT_CRITICAL(&s_mux);
    startListening();
  }

  if (s_role == Role::Listen) {
    uint32_t now = millis();
    if ((int32_t)(now - s_listenUntilMs) >= 0) {
      if (!s_claimAtMs) {
        // Random 1-5 frames (of 8 slots) so two nodes starting together don't both claim.
        s_claimAtMs = now + (uint32_t)random(1, 6) * 8 * (SLOT_US / 1000);
      } else if ((int32_t)(now - s_claimAtMs) >= 0) {
        terminal::printAsync("* No net manager heard, starting the net as manager");
        becomeManager(false);
      }
    }
    return;
  }

  // Frame boundaries.
  int64_t s = nowSlot();
  portENTER_CRITICAL(&s_mux);
  int pos = positionLocked(s);
  portEXIT_CRITICAL(&s_mux);
  if (pos < 0) return;
  int64_t frame = s - pos;  // absolute slot where this frame started
  if (frame != s_lastFrame) {
    s_lastFrame = frame;
    s_frameCount++;
    if (s_role == Role::Manager) managerOnFrame();
    else memberOnFrame();
  }
}

void onPacket(uint8_t type, uint8_t src, const uint8_t* p, size_t plen, int64_t slot, float rssi) {
  (void)rssi;
  if (s_role == Role::Static || s_role == Role::Off) return;
  switch (type) {
    case PKT_NET:
      onBeacon(src, p, plen, slot);
      break;
    case PKT_JOIN:
      if (s_role == Role::Manager && plen >= 12) {
        char name[9];
        memcpy(name, p + 4, 8);
        name[8] = 0;
        managerAllocate(get32(p), name);
      }
      break;
    case PKT_DIR:
      for (size_t k = 0; k + 13 <= plen; k += 13) {
        char name[9];
        memcpy(name, p + k + 5, 8);
        name[8] = 0;
        if (get32(p + k) == s_permId) continue;
        dirUpsert(get32(p + k), p[k + 4], name);
      }
      break;
    default:
      break;
  }
  if (src != ADDR_NONE) {
    DirEntry* e = dirByAddr(src);
    if (e) {
      e->lastHeardMs = millis();
      e->lastHeardFrame = s_frameCount;
    }
  }
}

SlotUse planSlot(int64_t s, uint8_t* buf, size_t max, size_t& len) {
  SlotUse use = SlotUse::None;
  len = 0;
  portENTER_CRITICAL(&s_mux);
  if (s_sch.active) {
    if (s_sch.staticMode) {
      if (s % s_sch.n == s_sch.myAddr) use = SlotUse::Own;
    } else {
      int pos = positionLocked(s);
      if (pos == 0) {
        if (s_sch.manager && s_sch.beaconLen && s_sch.beaconLen <= max) {
          memcpy(buf, s_sch.beacon, s_sch.beaconLen);
          len = s_sch.beaconLen;
          use = SlotUse::Beacon;
        }
      } else if (pos > 0 && s_sch.myAddr != ADDR_NONE && s_sch.owner[pos] == s_sch.myAddr) {
        use = SlotUse::Own;
      }
    }
  }
  if (use == SlotUse::None && s_sch.schedLen && s_sch.schedSlot == s && s_sch.schedLen <= max) {
    memcpy(buf, s_sch.sched, s_sch.schedLen);
    len = s_sch.schedLen;
    s_sch.schedLen = 0;
    use = SlotUse::Scheduled;
  }
  portEXIT_CRITICAL(&s_mux);
  return use;
}

Role role() { return s_role; }

const char* roleName(Role r) {
  switch (r) {
    case Role::Off: return "off (not slotted)";
    case Role::Static: return "static slots";
    case Role::Listen: return "listening for a net manager";
    case Role::Joining: return "joining";
    case Role::Member: return "member";
    case Role::Manager: return "net manager";
  }
  return "?";
}

uint8_t myAddr() {
  if (g_settings.netMode == NetMode::Static || s_role == Role::Off) return g_settings.nodeId;
  return s_sch.myAddr;
}

uint16_t currentN() { return s_sch.active ? s_sch.n : g_settings.slots; }
int64_t currentS0() { return s_sch.s0; }

int position(int64_t s) {
  portENTER_CRITICAL(&s_mux);
  int p = s_sch.staticMode ? (int)(s % s_sch.n) : positionLocked(s);
  portEXIT_CRITICAL(&s_mux);
  return p;
}

uint32_t permId() { return s_permId; }

const char* nameFor(uint8_t addr) {
  DirEntry* e = dirByAddr(addr);
  return (e && e->name[0]) ? e->name : nullptr;
}

void printStatus(Print& out) {
  out.printf("Net role:  %s, mode %s, ID %08lX, name %s\r\n", roleName(s_role),
             g_settings.netMode == NetMode::Static ? "static" : "managed", (unsigned long)s_permId, g_settings.name);
  if (s_role == Role::Member || s_role == Role::Manager || s_role == Role::Joining) {
    out.printf("           address %u, N=%u, S0=%lld", s_sch.myAddr == ADDR_NONE ? 0 : s_sch.myAddr, s_sch.n,
               (long long)s_sch.s0);
    if (s_sch.nextN) out.printf(", N -> %u at slot %lld", s_sch.nextN, (long long)s_sch.nextS0);
    out.print("\r\n           my positions:");
    for (int p = 1; p < s_sch.n; p++)
      if (s_sch.owner[p] == s_sch.myAddr && s_sch.myAddr != ADDR_NONE) out.printf(" %d", p);
    out.printf("  map v%u", s_mapVer);
    out.print("  deputies:");
    for (int k = 0; k < 3; k++)
      if (s_deputies[k] != ADDR_NONE) out.printf(" %u", s_deputies[k]);
    if (s_role != Role::Manager) out.printf("  missed beacons %lu", (unsigned long)s_missedBeacons);
    out.print("\r\n");
  }
}

void printWho(Print& out) {
  out.print("name      addr  slot  role      heard(s ago)  ID\r\n");
  int shown = 0;
  for (auto& e : s_dir) {
    if (!e.permId || !e.active) continue;
    int pos = -1;
    for (int p = 1; p < s_sch.n; p++)
      if (s_sch.owner[p] == e.addr) { pos = p; break; }
    const char* role = "";
    if (e.permId == s_mgrId || (s_role == Role::Manager && e.permId == s_permId)) role = "manager";
    for (int k = 0; k < 3; k++)
      if (s_deputies[k] == e.addr) role = k == 0 ? "deputy1" : k == 1 ? "deputy2" : "deputy3";
    out.printf("%-8s  %4u  %4d  %-8s  %12lu  %08lX%s\r\n", e.name, e.addr, pos, role,
               (unsigned long)((millis() - e.lastHeardMs) / 1000), (unsigned long)e.permId,
               e.permId == s_permId ? " (me)" : "");
    shown++;
  }
  if (!shown) out.print("(directory empty)\r\n");
}

}  // namespace net

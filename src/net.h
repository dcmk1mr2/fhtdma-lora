#pragma once
#include <Arduino.h>

// Net manager and dynamic slot allocation (design doc section 5).
//
// Managed mode (default):
//   Listen  - hop with the net and wait for a manager beacon (set listen <s>).
//             If none is heard, wait a random 1-5 frames, then become manager.
//   Joining - send JOIN (permanent ID + name) in a random free slot until the
//             manager's beacon acknowledges it with a short address.
//   Member  - transmit in the slot the manager granted. Idle members send a
//             keepalive every 10 frames. After 3k frames with no beacon,
//             deputy k takes over as manager with the same map.
//   Manager - beacon in position 0 of every frame: N, S0, slot map (in
//             chunks of 4), deputies and one join ack. Grows N (doubling)
//             when the net is full, announced 3 frames ahead. Frees nodes
//             silent for 30 frames. Sends DIRECTORY packets in free slots.
//
// Frame position of absolute slot s: p = (s - S0) mod N. p = 0 is the beacon.
//
// Static mode (set netmode static): step 2 behaviour, slot = s mod N == id.
namespace net {

enum class Role : uint8_t { Off, Static, Listen, Joining, Member, Manager };

void begin();
void loop();

// Called from the main loop for every received packet of this net.
void onPacket(uint8_t type, uint8_t src, const uint8_t* p, size_t plen, int64_t slot, float rssi);

// --- Radio task interface (thread safe) ---
enum class SlotUse { None, Own, Beacon, Scheduled };
// Decide what this node does in absolute slot s. For Beacon and Scheduled,
// the packet to send is copied into buf/len.
SlotUse planSlot(int64_t s, uint8_t* buf, size_t max, size_t& len);

// --- Status ---
Role role();
const char* roleName(Role r);
uint8_t myAddr();         // short address used as packet source (ADDR_NONE until joined)
uint16_t currentN();
int64_t currentS0();
int position(int64_t s);  // frame position of slot s, -1 if unknown
uint32_t permId();
const char* nameFor(uint8_t addr);  // display name, nullptr if unknown
void printStatus(Print& out);
void printWho(Print& out);
void restart();           // drop out and listen again (after settings change)
void nameChanged();       // apply a new display name with as little disruption as possible

}  // namespace net

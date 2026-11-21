#pragma once
#include <Arduino.h>

// Radio and TDMA scheduler. A dedicated task on core 0 owns the radio.
//
// Hop on (and time synced): every T = 120 ms slot the radio retunes to the
// hop channel for that slot and listens. In slots this node owns
// (slot mod N == node id) it transmits G/2 = 5 ms after slot start: first a
// queued packet, otherwise whatever the slot filler supplies (test pings).
// In bench time mode, every SYNC_PERIOD-th slot is a sync slot on the fixed
// channel, where the bench master sends its time beacon.
//
// Hop off, or time not synced: stays on the fixed channel, sends queued
// packets whenever the radio is free (step 1 behaviour).
namespace radio {

struct RxInfo {
  float rssi;
  float snr;
  int64_t rxDoneLocalUs;
  int64_t slot;  // -1 when not hopping
  uint8_t channel;
};

using RxHandler = void (*)(const uint8_t* data, size_t len, const RxInfo& info);

enum class SlotKind { Own, Sync };
// Called from the radio task when a slot has nothing queued. Fill buf and
// return its length (0 = stay silent). Set stampOffset to the byte offset of
// an int64 time stamp to be filled in at transmit time, or leave it -1.
using SlotFiller = size_t (*)(SlotKind kind, int64_t slot, uint8_t* buf, size_t max, int& stampOffset);

bool begin();
void loop();  // call from the main loop: delivers received packets

// Queue a packet. If stampOffset >= 0, an int64 network time stamp of the
// expected end of transmission is written there just before sending.
bool send(const uint8_t* data, size_t len, int stampOffset = -1);

void onReceive(RxHandler h);
void setSlotFiller(SlotFiller f);

bool setChannel(uint8_t ch);  // fixed / sync channel
bool setPower(int8_t dbm);
float channelMHz(uint8_t ch);
uint32_t airtimeUs(size_t len);

bool hopping();             // currently running the TDMA/hop schedule
int64_t currentSlot();
uint8_t currentChannel();
int32_t txLatencyUs();      // measured time from stamp to TX start, beyond airtime
int64_t lastOwnTxSlot();    // last slot this node transmitted in one of its own slots

struct Counters {
  uint32_t txOk, txFail, rxOk, rxCrc, queueFull, txLate, txOverrun;
  uint32_t txByType[9];  // packets started, by PktType (index 0 = unknown)
};
const Counters& counters();

}  // namespace radio

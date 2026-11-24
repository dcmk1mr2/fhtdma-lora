#pragma once
#include <Arduino.h>

// Push-to-talk Codec2 voice in "floor mode": while PTT is held and nobody else is
// talking, this node sends a voice packet in every slot except the net manager's
// beacon slot and the bench sync slot. Everyone else stays quiet, plays the
// voice, and resumes normal traffic once the talker stops.
//
// Codec2 2400 (default): 6 bytes per 20 ms frame, up to 7 frames (140 ms) per slot.
namespace voice {

void begin();
void loop();                       // main loop: PTT -> start/stop talking

// Audio task (core 1), once per 20 ms block of 160 samples.
void onMicBlock(const int16_t* pcm, int n);
bool playBlock(int16_t* out, int n); // false = nothing to play

// Radio task (core 0).
bool transmitting();               // this node holds the floor
size_t fillSlot(uint8_t* buf, size_t max);  // voice packet for this slot, 0 = none ready
bool floorBusy();                  // someone else is talking: stay off the air

// Main loop: a received voice packet.
void onPacket(uint8_t src, uint16_t seq, uint8_t flags, const uint8_t* p, size_t plen);

bool activeRecently();             // voice sent or heard in the last 2 s
bool setMode(int bitrate);         // 3200 (loopback only), 2400, 1600 or 1200
void printStatus(Print& out);

}  // namespace voice

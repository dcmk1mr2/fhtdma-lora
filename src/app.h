#pragma once
#include <Arduino.h>

// Message handling: text, test mode, bench beacons, per-node statistics.
namespace app {

void begin();
void loop();

bool sendText(const char* text);
// True when this node can transmit now (in a net, static slots, or unslotted).
// If false, why is set to a short reason.
bool canSend(const char*& why);

void setTestMode(bool on);
bool testMode();

void printStats(Print& out);
void resetStats();
void printStatus(Print& out);

}  // namespace app

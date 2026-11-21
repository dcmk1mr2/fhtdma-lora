#pragma once

// Status on the board's 128x64 OLED: net and node, GPS fix/satellites/PPS,
// UTC time, time sync and the current hop channel. Refreshes once a second.
namespace display {

void begin();
void loop();

}  // namespace display

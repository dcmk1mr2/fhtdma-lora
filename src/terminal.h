#pragma once
#include <Arduino.h>

// USB serial terminal. Two modes:
//   command mode: "> " prompt, see "help"
//   text mode:    "text> " prompt, each line is sent as a broadcast message,
//                 "/exit" returns to command mode
namespace terminal {

void begin();
void loop();

// Print a line without disturbing what the user is typing.
void printAsync(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void printMessage(const char* call, uint8_t src, const char* text);

}  // namespace terminal

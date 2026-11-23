#pragma once
#include <Arduino.h>

// Audio front end (Heltec V3 only): INMP441 I2S mic in, MAX98357A I2S amp out
// (or sigma-delta on PIN_AUDIO_OUT into an RC low-pass), PTT switch. 8 kHz, 16-bit PCM.
// This is the capture/playback layer for Codec2 voice; the radio does not
// carry audio yet. Test modes check the hardware on the bench.
namespace audio {

// Codec: while PTT is held, mic -> Codec2 encode -> decode -> speaker, no radio.
enum class Loop : uint8_t { Off, On, Ptt, Codec };

void begin();
void loop();                 // PTT debounce and meter output (main loop)

bool available();            // false on boards without audio pins
bool pttDown();              // debounced PTT state

void setLoopback(Loop mode); // mic -> speaker: off, always, or only while PTT is held
void setTone(bool on);       // 1 kHz test tone to the speaker
void setMeter(bool on);      // print mic level twice a second
bool setMicRight(bool right);// swap I2S channel if the mic reads silence
void setMicGainDb(int db);   // mic gain before the meter, loopback and Codec2, -10..+30 dB
bool codecLoop();            // Loop::Codec selected
void setGainDb(int db);      // output gain for tone and loopback, -40..+30 dB

void printStatus(Print& out);

}  // namespace audio

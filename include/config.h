#pragma once
// Board pins and radio defaults. Check the pins against your board revision.

#if defined(BOARD_HELTEC_V3)
// ---- Heltec WiFi LoRa 32 V3 / V3.2 (ESP32-S3 + SX1262) ----
#define RADIO_SX1262
#define PIN_LORA_SCK   9
#define PIN_LORA_MISO  11
#define PIN_LORA_MOSI  10
#define PIN_LORA_CS    8
#define PIN_LORA_RST   12
#define PIN_LORA_BUSY  13
#define PIN_LORA_DIO1  14
#define LORA_TCXO_V    1.8f
#define PIN_LED        35
// GPS TX -> GPIO1, GPS PPS -> GPIO7. GPS RX not wired (the GPS needs no commands).
// GPIO1 also feeds the battery divider, a light load the GPS TX drives easily.
// GPIO47/48 also work. The `pinscan` command shows which GPIO a wire is on.
// These are defaults: `set gps <rx> <tx> <pps>` changes them at run time (reboot to apply).
#define PIN_GPS_RX     1
#define PIN_GPS_TX     -1
#define PIN_GPS_PPS    7
#define PIN_SLOT_DEBUG 19           // high at slot start, low once retuned (scope test)
#define PIN_OLED_SDA   17
#define PIN_OLED_SCL   18
#define PIN_OLED_RST   21
#define PIN_VEXT       36           // LOW powers the OLED
// External pins follow Heltec's V3 guide: GPIO 1, 2, 4, 5, 6, 7, 19, 20, 47, 48.
// GPIO26 and 33-38 are flash/SubSPI, 43/44 are USB serial, 46 is a strapping pin.
// Audio: INMP441 mic VDD 3V3, GND, L/R -> GND (left channel).
#define HAS_AUDIO      1
#define PIN_MIC_SCK    4            // INMP441 SCK (I2S bit clock)
#define PIN_MIC_WS     5            // INMP441 WS (word select)
#define PIN_MIC_SD     2            // INMP441 SD (data out of the mic)
// Audio out: MAX98357A I2S amp sharing the mic's clocks (BCLK = GPIO4, LRC = GPIO5,
// DIN = PIN_AUDIO_OUT). Set AUDIO_OUT_I2S to 0 for sigma-delta on PIN_AUDIO_OUT into an RC filter.
#define AUDIO_OUT_I2S  1
#define PIN_AUDIO_OUT  6            // MAX98357A DIN (or sigma-delta out with AUDIO_OUT_I2S 0)
#define PIN_PTT        20           // push-to-talk switch to GND (internal pull-up)
#elif defined(BOARD_HELTEC_V2)
// ---- Heltec WiFi LoRa 32 V2 (ESP32 + SX1276) ----
#define PIN_LORA_SCK   5
#define PIN_LORA_MISO  19
#define PIN_LORA_MOSI  27
#define PIN_LORA_CS    18
#define PIN_LORA_RST   14
#define PIN_LORA_DIO0  26
#define PIN_LORA_DIO1  35
#define PIN_LED        25
// GPS TX -> GPIO23, ESP32 GPIO17 -> GPS RX, GPS PPS -> GPIO39.
// GPIO 4/15/16 are the OLED and 34/35 are LoRa DIO lines, so they are avoided.
#define PIN_GPS_RX     23
#define PIN_GPS_TX     17
#define PIN_GPS_PPS    39
#define PIN_SLOT_DEBUG 13
#define PIN_OLED_SDA   4
#define PIN_OLED_SCL   15
#define PIN_OLED_RST   16
#define PIN_VEXT       21           // LOW powers the OLED
#else
// ---- LilyGO T3 LoRa32 V2.1_1.6 (ESP32 + SX1276) ----
#define PIN_LORA_SCK   5
#define PIN_LORA_MISO  19
#define PIN_LORA_MOSI  27
#define PIN_LORA_CS    18
#define PIN_LORA_RST   23
#define PIN_LORA_DIO0  26
#define PIN_LORA_DIO1  33
#define PIN_LED        25
// GPS TX -> ESP32 GPIO34 (input only), ESP32 GPIO4 -> GPS RX, GPS PPS -> GPIO39.
// GPIO 2/13/14/15 are used by the SD card slot on V2.1, so they are avoided.
#define PIN_GPS_RX     34
#define PIN_GPS_TX     4
#define PIN_GPS_PPS    39
#define PIN_SLOT_DEBUG 12
#define PIN_OLED_SDA   21
#define PIN_OLED_SCL   22
#define PIN_OLED_RST   -1
#define PIN_VEXT       -1
#endif
#ifndef HAS_AUDIO
#define HAS_AUDIO      0
#endif
#ifndef AUDIO_OUT_I2S
#define AUDIO_OUT_I2S  0
#endif
#define GPS_BAUD       9600

// ---- Radio parameters (design doc defaults) ----
#define LORA_BW_KHZ        125.0f
#define LORA_SF            7
#define LORA_CR            5        // 4/5
#define LORA_SYNC_WORD     0x12     // private network sync word
#define LORA_PREAMBLE      6
#define LORA_POWER_DEFAULT 2        // dBm; keep low on a bench
#ifdef RADIO_SX1262
#define LORA_POWER_MAX     22
#else
#define LORA_POWER_MAX     17
#endif

// Channel plan: 127 channels, 902.3 + 0.2*k MHz (k = 0..126)
#define CH_COUNT       127
#define CH_BASE_MHZ    902.3f
#define CH_STEP_MHZ    0.2f
#define CH_DEFAULT     63           // 914.9 MHz: fixed channel with hop off, bench sync channel

// ---- TDMA timing (design doc defaults) ----
#define SLOT_US        140000       // T: slot time (140 ms since 0.7.0, to fit Codec2 3200)
#define GUARD_US       5000         // G: guard time; TX starts G/2 into the slot
// Bench time mode only: every SYNC_PERIOD slots all nodes meet on the fixed
// channel and the bench master sends its time beacon. Not used with GPS.
#define SYNC_PERIOD    32

// ---- Packet sizes ----
#define PKT_HEADER_LEN 6
#define PKT_TEXT_MAX   42           // text messages stay 42 characters (48-byte packets)
// Largest payload (voice). A 70-byte packet is 126 ms on air at SF7/125 kHz: it fits a
// 140 ms slot with G = 5 ms, and 3 hits on one channel in 20 s stay under 400 ms.
#define PKT_PAYLOAD_MAX 64
#define PKT_MAX_LEN    (PKT_HEADER_LEN + PKT_PAYLOAD_MAX)   // 70 bytes

#define FW_VERSION     "0.7.3-voice"

# FH-TDMA LoRa node firmware

Firmware for a frequency-hopped TDMA LoRa network on the US 915 MHz band
(FCC Part 15.247), with an external u-blox NEO-6M GPS. Supported boards:

| Board | Radio | PlatformIO env |
|-------|-------|----------------|
| Heltec WiFi LoRa 32 V3 / V3.2 (ESP32-S3) | SX1262 | `heltec_v3` (default) |
| LilyGO T3 LoRa32 V2.1_1.6 | SX1276 | `lora32_v21` |
| Heltec WiFi LoRa 32 V2 | SX1276 | `heltec_v2` |

All boards use the same packet format and settings, so they can share a net. Design doc:
https://claude.ai/artifact/E8x5DL42hH3cXBXfDEWVhU

## Status

**0.7.2:** only one bench master: a bench master that hears another master with a lower ID switches itself to `time bench` (saved) and prints a notice; the lower ID keeps going and prints a warning. The `status` airtime label now shows the real largest packet (70 bytes).

**0.7.1:** slot timing pulse on GPIO19: the radio releases the S3's native USB pads (GPIO19/20) at startup, not only when audio starts, and the pulse is at least 100 us wide.

**0.7.0:** slot time T is now 140 ms (was 120) and voice packets can carry 64 bytes (70-byte packets, 126 ms on air), so Codec2 3200 works over the air: 8 frames (160 ms of audio) per 140 ms slot. Text messages are still 42 characters. **All nodes must run 0.7.0**, because older firmware uses 120 ms slots and won't line up.

**0.6.4:** `set codec 3200` for the `audio loop codec` test only (too fast for a slot; PTT on air refuses it).

**0.6.3:** voice quality checks: `audio micgain <dB>` (default +12 dB, saved) and `audio loop codec`, which runs the mic through Codec2 encode and decode locally with no radio while PTT is held.

**0.6.2:** PTT on GPIO20: release the S3 native-USB pad so the button press reaches the GPIO input.

**0.6.1:** audio task stack raised to 64 KB (Codec2 overflowed 24 KB); `audio` shows unused stack.

**0.6.0: Codec2 push-to-talk voice.** Hold PTT and talk; every node in the net plays
it. See "Voice" below. Host-tested with the real Codec2 library, not yet on hardware.

**0.5.1:** `audio gain` goes down to -40 dB, applies to the test tone too, and is saved (default -12 dB).

**0.5.0:** MAX98357A I2S amp output. The amp shares the mic's BCLK (GPIO4) and LRC (GPIO5); DIN on GPIO6. New `audio gain`. `audio mic left|right` no longer restarts the driver.

**0.4.8:** `stats` adds a per-type tx/rx count (text, test, time beacon, net beacon, join, dir, alive) to debug the net manager on the bench.

**0.4.7:** new `pinscan` command: watches the free GPIOs for 3 s and names the one the GPS TX is on. Many pinout cards online show the Heltec V2 (ESP32) labels, which don't match the V3.

**0.4.6:** Heltec V3 GPS input moved to GPIO1; GPS RX is left unwired. Slot debug moved to GPIO19.

**0.4.5:** the `gps` command names the GPS pins actually in use (it still said GPIO34/39 from the LilyGO board).

**0.4.4:** all external pins moved onto the GPIOs Heltec recommends for the V3 (1, 2, 4, 5, 6, 7, 19, 20, 47, 48). Audio out GPIO26 -> GPIO6, PTT GPIO33 -> GPIO20, mic WS back to GPIO5, slot debug -> GPIO1. GPIO26 and 33-38 are flash/SubSPI lines and are now refused by `set gps`.

**0.4.3:** `set gps` refuses GPIO43/44 (USB serial, the terminal) and the boot strapping pins; saved GPS pins on those revert to the defaults at boot. Slot debug output moved from GPIO6 to GPIO38 so GPIO5/6/7 are free for the GPS.

**0.4.2:** startup prints a boot trace (`boot: settings`, `boot: gps`, ...) so a hang shows where it stopped. Hold PRG through reset for safe mode, which skips GPS and audio setup.

**0.4.1:** mic WS moved from GPIO5 to GPIO34 so GPIO5 is free for the GPS, and GPS pins can be set with `set gps`.

**0.4.0:** guard time G = 5 ms, default net call **SF-HAB**, and an audio front end on
Heltec V3 (INMP441 mic, audio out on GPIO26, PTT on GPIO33). Audio is local only for
now: loopback, test tone and level meter to check the hardware. Codec2 voice over the
radio is the next step. A board that already has a call saved keeps it until you type
`set call SF-HAB`.

**Step 3 (this version): net manager.** Steps 1 and 2 ran on a 3-node Heltec V3.2
bench (step 2: 0% loss with hopping, timing within 0.5 ms). Step 3 is tested in a
host simulation with 12 nodes but not yet on hardware.

How a net forms (design doc section 5):

1. A node that's synced and hopping **listens** for a manager beacon (`set listen`, default 20 s).
2. If it hears none, it waits a random 1-5 frames and **starts the net as manager**
   with N = `set n` (default 8). Its beacon goes in frame position 0 of every frame.
3. Other nodes hear the beacon and **join**: they send a JOIN with their ID and name in
   a random free slot. The manager gives each a short address and one slot, and
   acknowledges in its beacon. The slot map rides in the beacon, 4 entries at a time.
4. When there are no free slots, the manager **doubles N**, announced 3 frames ahead.
5. Idle members send a small keepalive every 10 frames. A node silent for 30 frames
   loses its slot.
6. The manager names the 3 longest-joined nodes as **deputies**. If beacons stop,
   deputy 1 takes over after 3 frames (deputy 2 after 6, deputy 3 after 9) with the
   same slot map, so nobody has to rejoin.
7. If two managers hear each other, the one with the **lower ID** keeps the role and
   the other rejoins.
8. **Names:** `set name` (up to 8 characters, default N-XXXX). The manager refuses a
   duplicate and sends the directory in free slots. Messages show the sender's name,
   and `who` lists the net.

`set netmode static` brings back the step 2 behaviour (fixed slot = `set id`, no manager).

Step 2 features still apply: 140 ms slots (120 ms before 0.7.0) with 5 ms guard (TX starts 2.5 ms into the slot), callsign-seeded hopping
over 127 channels, radio in its own task on core 0, TX latency self-calibration,
OLED status screen, and `set hop off` for a fixed channel with no slots.

## Build and flash

Install PlatformIO (VS Code extension or `pip install platformio`), then:

```
pio run -e heltec_v3 -t upload    # build and flash (pick your board's env)
pio device monitor                # open the terminal at 115200 baud
```

**Flash every board at once (Windows):** plug all boards in, close any serial
monitors, and double-click `upload_all.bat` (or run `.\upload_all.ps1`). It builds
once, finds every CP210x board and uploads to each. Use `-Ports COM3,COM5` to pick
ports, or `-Env lora32_v21` for another board type.

If an upload stalls at "Connecting...", hold the PRG/BOOT button until it starts.
On Windows the boards need the CP210x USB driver.

## Heltec V3 pin map (GPIO numbers, not header positions)

Heltec's pin map image shows where each GPIO sits on the two headers:
https://resource.heltec.cn/download/WiFi_LoRa_32_V3/Wi-Fi_LoRa32_V3.2_Pinmap.png

| GPIO | Used for              | Wire to                    |
|------|-----------------------|----------------------------|
| 1    | GPS in (ESP32 RX)     | GPS TX                     |
| -    | GPS RX                | leave unconnected          |
| 7    | GPS PPS               | GPS PPS                    |
| 4    | mic bit clock         | INMP441 SCK                |
| 5    | mic word select       | INMP441 WS                 |
| 2    | mic data              | INMP441 SD                 |
| 6    | audio out             | MAX98357A DIN              |
| 20   | PTT                   | switch to GND              |
| 19   | slot debug (optional) | scope probe                |

Don't wire anything to GPIO43/44 (USB serial: the header pins labeled U0TXD/U0RXD),
GPIO26 or 33-38 (flash), GPIO0, 3, 45 or 46 (boot strapping), or RST.

## Wiring the GPS

| GPS pin | Heltec V3 | LilyGO V2.1 | Heltec V2 |
|---------|-----------|-------------|-----------|
| TX      | GPIO1     | GPIO34      | GPIO23    |
| RX      | not wired | GPIO4       | GPIO17    |
| PPS     | GPIO7     | GPIO39      | GPIO39    |
| VCC/GND | 3.3 V/GND | 3.3 V/GND   | 3.3 V/GND |

These avoid the pins used by each board's radio, OLED, USB and SD card.
If your GPS is wired differently, set the pins from the terminal and reboot, for example
`set gps 5 6 7` (rx = ESP32 pin wired to the GPS TX, tx = pin wired to the GPS RX, pps).
Use `none` for tx or pps if they aren't wired. Pins used by the LoRa radio, OLED, USB,
LED or audio are refused. `set` shows the current GPS pins.

## Wiring the audio (Heltec V3 only)

The mic and the MAX98357A amp share one I2S bus: same bit clock and word select,
separate data lines.

| Part                 | Pin                  | Heltec V3            |
|----------------------|----------------------|----------------------|
| INMP441 VDD / GND    | power                | 3V3 / GND            |
| INMP441 SCK          | I2S bit clock        | GPIO4                |
| INMP441 WS           | I2S word select      | GPIO5                |
| INMP441 SD           | data from mic        | GPIO2                |
| INMP441 L/R          | channel select       | GND (left channel)   |
| MAX98357A VIN / GND  | power                | 5V (louder) or 3V3 / GND |
| MAX98357A BCLK       | I2S bit clock        | GPIO4 (shared with mic SCK) |
| MAX98357A LRC        | I2S word select      | GPIO5 (shared with mic WS)  |
| MAX98357A DIN        | data to amp          | GPIO6                |
| MAX98357A SD, GAIN   | leave unconnected    | (on, 9 dB gain)      |
| Speaker              | 4-8 ohm              | amp + and - only, neither to GND |
| PTT switch           | to GND when pressed  | GPIO20 (internal pull-up, 20 ms debounce) |

I2S runs as master at 8 kHz with 32-bit slots, both directions on one port. The mic
sample is shifted right 14 bits to 16-bit PCM with the DC removed. Each output sample
goes in both slots, so the amp plays it whatever its SD pin selects. Amp gain: GAIN
unconnected = 9 dB, to GND = 12 dB, 100k to GND = 15 dB, to VIN = 6 dB, 100k to VIN = 3 dB. `audio gain <dB>` sets the
output level for the tone and loopback, -40 to +30 dB, default -12, saved in flash.

Without an amp board, set `AUDIO_OUT_I2S 0` in `include/config.h`: GPIO6 then puts out
8-bit sigma-delta at 312 kHz for a 1k/10nF RC low-pass into a small powered speaker.

Bench checks:

1. `audio meter on`, then talk: the level should rise from about -70 dBFS to -30 or
   higher. If it stays near -120 or never moves, try `audio mic right`.
2. `audio tone on`: a 1 kHz tone on the output. `audio tone off` to stop.
3. `audio loop ptt`: hold PTT and talk to hear yourself (keep the speaker away from
   the mic). `[PTT down]` / `[PTT up]` print on the terminal and PTT shows on the OLED.

## Voice (Heltec V3)

Hold PTT and talk. The talker gets the floor: it sends a voice packet in every slot
except the net manager's beacon slot (and the bench sync slot in bench time mode).
Everyone else stays off the air while voice is heard and plays it through the
MAX98357A. When PTT is released the talker flushes what's queued and normal traffic
resumes. If someone is already talking, PTT prints "Channel busy" and waits.

| Codec2 | Frame          | Per slot (140 ms)         | Notes |
|--------|----------------|---------------------------|-------|
| 3200   | 8 bytes, 20 ms | up to 8 frames, 64 bytes  | best quality; 160 ms of audio per slot, so it can skip about 1 slot in 8 |
| 2400   | 6 bytes, 20 ms | up to 10 frames, 60 bytes | default |
| 1600   | 8 bytes, 40 ms | up to 8 frames, 64 bytes  | |
| 1200   | 6 bytes, 40 ms | up to 10 frames, 60 bytes | lower quality |

At 3200, managed mode with a very small net (N = 4) skips every 4th slot for the beacon,
which is more than 3200 can make up; use 2400 there or let N grow.

Codec mode and frame count ride in the packet header's flags byte, so listeners
follow whatever the talker sends. Playback starts with about 210 ms buffered, so
mouth-to-ear delay is roughly 300-400 ms. The net manager doesn't free silent nodes'
slots while voice is active. Voice needs hop on and time sync (bench or gps).

Commands: `set voice on|off`, `set codec 3200|2400|1600|1200`, and `status` shows voice
counters (packets, lost, encode/decode time). `audio gain` sets playback volume.

Codec2 comes from https://github.com/meshtastic/ESP32_Codec2 (pinned in platformio.ini,
heltec_v3 env only). It downloads on the first build.

## Terminal commands

```
help                 command list
status               settings, radio, time, current slot and channel
gps                  GPS time, fix, satellites, PPS
text                 enter text mode (type exit or /exit to leave, or press RST)
test on|off          ping in every own slot (hop on) or every interval (hop off)
stats [reset]        per-node packet counts, loss, RSSI, time error
who                  nodes on the net: name, address, slot, role, last heard
audio                mic level, PTT state, audio test status
audio loop on|off|ptt  mic to speaker: always, off, or only while PTT is held
audio loop codec       while PTT is held: mic -> Codec2 -> speaker, no radio
audio micgain <dB>     mic gain -10..30 dB (default 12); aim for speech peaks near -12 dBFS
audio tone on|off    1 kHz test tone on the audio output
audio gain <dB>      output level, -40 to 30 dB (default -12, saved)
audio meter on|off   print the mic level twice a second
audio mic left|right I2S channel for the mic (L/R to GND = left)
set                  show all current settings
set call <CALL>      net call sign (all nodes in a net must match)
set name <name>      display name, 1-8 characters, no spaces, unique on the net
set netmode <mode>   managed (default) | static
set listen <s>       how long to listen for a manager before starting a net
set n <2-256>        starting N if this node starts the net (static mode: N for all)
set id <0..N-1>      static mode only: node ID = slot
set hop on|off       TDMA + hopping (default on; needs bench or gps time)
set ch <0-126>       fixed channel when hop is off; also the bench sync channel
set power <dBm>      TX power, 2-17 (SX1276) or 2-22 (SX1262), default 2
set interval <ms>    test ping interval when hop is off
set time <mode>      local | bench | benchmaster | gps
set gps <rx> [tx] [pps]  GPS pins on the ESP32 (reboot to apply)
set voice on|off     PTT sends Codec2 voice (default on)
set codec <rate>     Codec2 3200, 2400 (default), 1600 or 1200
pinscan              find which GPIO a signal (e.g. GPS TX) is wired to
reboot
```

Settings are saved to flash when changed. Numbers must be whole numbers in range
(`set ch 5x` is refused rather than read as 5). Changing `call`, `netmode`, `time`
or `hop` makes the node rejoin the net. `set name` on the manager renames it in place;
on a member it rejoins so the manager can check the name. `set n` and `set id` only
cause a rejoin in static mode.

## Bench test with the net manager (before GPS boards arrive)

1. Flash every board. Keep `set call` the same on all and the bench time setup from
   step 2 (one `benchmaster`, the rest `bench`).
2. Give each board a name: `set name Base1`, `set name Base2`, ...
3. Reset all boards. Within about 25 s one of them prints "starting the net as manager",
   and the others print "Net: joining" and then "Net: member".
4. `who` on any board lists the net with names, addresses, slots, manager and deputies.
   `status` shows this node's role, address, N and slot position.
5. `text` on any board: messages show up as `[Base2] hello` on the others.
6. `test on` everywhere, then `stats`: loss near 0, as in step 2.
7. Failover: unplug the manager. Within about 3 frames (3 s at N = 8), deputy 1 prints
   "taking over as manager" and traffic carries on. About 30 frames later the old
   manager's slot is freed. Plug it back in and it rejoins as a member.
8. Growth: with N = 4 (`set n 4` on the board that will start the net), a fourth
   member makes the manager grow N to 8.

The bench time master and the net manager don't have to be the same board.

**Bench sync slots:** in bench time mode, every 32nd slot (3.84 s) all nodes
meet on the fixed channel (`set ch`, default 63) and the master sends its time
beacon there. That is about 0.2 s per 20 s on that channel. GPS mode has no
sync slots and hops on every slot.

**Scope check (optional):** each board pulses a spare GPIO at every slot start
(high while retuning). Heltec V3: GPIO1, LilyGO V2.1: GPIO12, Heltec V2: GPIO13.
On two boards the pulses should line up within about 1 ms.

Keep power at 2 dBm and boards a metre or more apart so receivers don't saturate.

## GPS time

`set time gps` pairs each PPS edge with the NMEA time sentence that follows it,
giving UTC in microseconds. `gps` shows whether data, a fix and PPS pulses are
coming in. The design doc mentions UBX NAV-TIMEGPS; NMEA is used for now because
it works with the NEO-6M default config.

## Code layout

```
include/config.h   pins, radio parameters, channel plan
src/main.cpp       setup and main loop
src/settings.*     config saved in flash (NVS), net tag
src/timebase.*     network time: local, bench beacon, GPS PPS
src/gps.*          NMEA parsing, PPS interrupt, gps command
src/radio.*        radio task: TDMA slot scheduler, retune, TX queue, RX timestamps
src/hopseq.*       call-sign hop sequence (SHA-256 key, SipHash, Fisher-Yates)
src/packet.*       packet header encode/decode
src/app.*          text, test mode, beacons, per-node stats
src/terminal.*     serial terminal
src/display.*      OLED status screen
src/audio.*        INMP441 I2S mic, MAX98357A I2S out (or sigma-delta), PTT, audio tests
src/voice.*        Codec2 push-to-talk: encode, floor control, jitter buffer, decode
src/net.*          net manager: listen, join, slot map, N growth, names, failover
test/host/         host test for the hop sequence:
                   g++ -std=c++17 -I. -I../../src test_hopseq.cpp ../../src/hopseq.cpp -lcrypto
test/host/sim/     12-node simulation of the net manager (join, growth, duplicate
                   names, failover, two nets merging); see sim_main.cpp for the build
```

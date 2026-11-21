#include "audio.h"
#include "config.h"
#include "settings.h"

#if HAS_AUDIO
#include <driver/i2s.h>
#include <math.h>

namespace audio {

static const int RATE = 8000;
static const int BLOCK = 160;              // 20 ms at 8 kHz
static const int MIC_SHIFT = 14;           // 24-bit sample in a 32-bit word -> 16-bit PCM
static const i2s_port_t PORT = I2S_NUM_0;  // mic in, and the MAX98357A out when AUDIO_OUT_I2S
static const uint32_t PTT_DEBOUNCE_MS = 20;

#if !AUDIO_OUT_I2S
// Sigma-delta output: audio task fills a ring, an 8 kHz timer ISR drains it.
static const int SD_CHANNEL = 0;
static const uint16_t RING = 1024;         // 128 ms
static volatile int16_t s_ring[RING];
static volatile uint16_t s_head = 0, s_tail = 0;
static hw_timer_t* s_timer = nullptr;
#endif
static volatile uint32_t s_underruns = 0;

static bool s_ok = false;
static bool s_started = false;             // begin() ran (skipped in safe mode)
static volatile bool s_micRight = false;
static volatile Loop s_loop = Loop::Off;
static volatile bool s_tone = false;
static volatile int32_t s_gainQ8 = 256;    // output gain, Q8 (256 = 0 dB); set from settings in begin()
static int s_gainDb = 0;
static bool s_meter = false;
static uint32_t s_meterMs = 0;

// Mic level over the last block (written by the audio task, read by the main loop).
static volatile float s_rmsDb = -120.0f, s_peakDb = -120.0f;
static volatile uint32_t s_blocks = 0, s_micErrors = 0;

// PTT debounce state (main loop only).
static bool s_pttStable = false, s_pttRaw = false;
static uint32_t s_pttChangeMs = 0;
static volatile bool s_pttDown = false;

#if !AUDIO_OUT_I2S
static void onSampleTimer() {
  uint8_t duty = 128;
  if (s_tail != s_head) {
    int16_t s = s_ring[s_tail];
    s_tail = (s_tail + 1) % RING;
    duty = (uint8_t)((s >> 8) + 128);
  } else if (s_loop != Loop::Off || s_tone) {
    s_underruns++;
  }
  sigmaDeltaWrite(SD_CHANNEL, duty);
}

static void pushOut(const int16_t* pcm, int n) {
  for (int i = 0; i < n; i++) {
    uint16_t next = (s_head + 1) % RING;
    if (next == s_tail) return;            // full: drop the rest of this block
    s_ring[s_head] = pcm[i];
    s_head = next;
  }
}
#endif

// Both slots are read (and written), and the mic slot is picked in software, so
// `audio mic left|right` needs no driver restart. With AUDIO_OUT_I2S the same port
// also transmits: the MAX98357A shares the mic's bit clock and word select.
static bool i2sInstall() {
  i2s_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | (AUDIO_OUT_I2S ? I2S_MODE_TX : 0));
  cfg.sample_rate = RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = BLOCK;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;           // play silence if the task falls behind
  if (i2s_driver_install(PORT, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins;
  memset(&pins, 0, sizeof(pins));
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_MIC_SCK;
  pins.ws_io_num = PIN_MIC_WS;
  pins.data_out_num = AUDIO_OUT_I2S ? PIN_AUDIO_OUT : I2S_PIN_NO_CHANGE;
  pins.data_in_num = PIN_MIC_SD;
  if (i2s_set_pin(PORT, &pins) != ESP_OK) {
    i2s_driver_uninstall(PORT);
    return false;
  }
  i2s_zero_dma_buffer(PORT);
  return true;
}

static inline int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

static void audioTask(void*) {
  static int32_t raw[2 * BLOCK];
  static int16_t pcm[BLOCK];
  static int16_t out[BLOCK];
#if AUDIO_OUT_I2S
  static int32_t tx[2 * BLOCK];
#endif
  int32_t dc = 0;                          // DC estimate, Q8
  uint32_t phase = 0;                      // tone phase, 1/8 cycle steps
  static const int16_t SINE8[8] = {0, 5792, 8191, 5792, 0, -5792, -8191, -5792};  // 1 kHz at 8 kHz, -12 dBFS
  for (;;) {
    size_t got = 0;
    if (i2s_read(PORT, raw, sizeof(raw), &got, pdMS_TO_TICKS(100)) != ESP_OK || got == 0) {
      s_micErrors++;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    int n = got / (2 * sizeof(int32_t));   // stereo frames
    int slot = s_micRight ? 1 : 0;
    double sumSq = 0;
    int peak = 0;
    for (int i = 0; i < n; i++) {
      int32_t v = raw[2 * i + slot] >> MIC_SHIFT;
      dc += ((v << 8) - dc) >> 9;          // slow high-pass, ~2.5 Hz corner
      v -= dc >> 8;
      pcm[i] = sat16(v);
      sumSq += (double)pcm[i] * pcm[i];
      int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
      if (a > peak) peak = a;
    }
    s_rmsDb = n ? 10.0f * log10f((float)(sumSq / n) / (32768.0f * 32768.0f) + 1e-12f) : -120.0f;
    s_peakDb = 20.0f * log10f(peak / 32768.0f + 1e-6f);
    s_blocks++;

    bool play = true;
    int32_t g = s_gainQ8;
    if (s_tone) {
      for (int i = 0; i < n; i++) out[i] = sat16((SINE8[(phase++) & 7] * g) >> 8);
    } else {
      Loop m = s_loop;
      if (m == Loop::On || (m == Loop::Ptt && s_pttDown)) {
        for (int i = 0; i < n; i++) out[i] = sat16((pcm[i] * g) >> 8);
      } else {
        play = false;
        memset(out, 0, sizeof(out));
      }
    }
#if AUDIO_OUT_I2S
    // Same sample in both slots, so the amp plays it in any SD_MODE (left, right or mix).
    for (int i = 0; i < n; i++) tx[2 * i] = tx[2 * i + 1] = (int32_t)out[i] << 16;
    size_t wrote = 0;
    i2s_write(PORT, tx, n * 2 * sizeof(int32_t), &wrote, pdMS_TO_TICKS(50));
    if (play && wrote < (size_t)n * 2 * sizeof(int32_t)) s_underruns++;
#else
    if (play) pushOut(out, n);
#endif
  }
}

void begin() {
  pinMode(PIN_PTT, INPUT_PULLUP);
  s_started = true;
  s_pttRaw = s_pttStable = (digitalRead(PIN_PTT) == LOW);
  s_pttDown = s_pttStable;
  s_pttChangeMs = millis();
  setGainDb(g_settings.audioGainDb);

#if !AUDIO_OUT_I2S
  sigmaDeltaSetup(PIN_AUDIO_OUT, SD_CHANNEL, 312500);
  sigmaDeltaWrite(SD_CHANNEL, 128);        // mid-scale = silence
#endif
  if (!i2sInstall()) {
    Serial.print("Audio: I2S driver failed to start.\r\n");
    return;
  }
#if !AUDIO_OUT_I2S
  // 8 kHz sample clock: 80 MHz APB / 80 = 1 MHz tick, alarm every 125 ticks.
  s_timer = timerBegin(1, 80, true);
  timerAttachInterrupt(s_timer, &onSampleTimer, true);
  timerAlarmWrite(s_timer, 1000000 / RATE, true);
  timerAlarmEnable(s_timer);
#endif

  // Core 1 with the main loop; the radio task owns core 0.
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 2, nullptr, 1);
  s_ok = true;
}

void loop() {
  if (!s_started) return;
  bool raw = (digitalRead(PIN_PTT) == LOW);
  uint32_t now = millis();
  if (raw != s_pttRaw) {
    s_pttRaw = raw;
    s_pttChangeMs = now;
  } else if (raw != s_pttStable && now - s_pttChangeMs >= PTT_DEBOUNCE_MS) {
    s_pttStable = raw;
    s_pttDown = raw;
    Serial.print(raw ? "[PTT down]\r\n" : "[PTT up]\r\n");
  }
  if (s_meter && s_ok && now - s_meterMs >= 500) {
    s_meterMs = now;
    float r = s_rmsDb;
    int bars = (int)((r + 90.0f) / 3.0f);  // -90 dBFS .. 0 dBFS -> 0..30
    if (bars < 0) bars = 0;
    if (bars > 30) bars = 30;
    char bar[31];
    memset(bar, '#', bars);
    bar[bars] = 0;
    Serial.printf("mic %6.1f dBFS rms %6.1f peak %s%s\r\n", r, (float)s_peakDb, s_pttDown ? "PTT " : "", bar);
  }
}

bool available() { return s_ok; }
bool pttDown() { return s_pttDown; }
void setLoopback(Loop mode) { s_loop = mode; }
void setTone(bool on) { s_tone = on; }
void setMeter(bool on) { s_meter = on; s_meterMs = 0; }
bool setMicRight(bool right) {
  s_micRight = right;
  return s_ok;
}
void setGainDb(int db) {
  s_gainDb = db;
  s_gainQ8 = (int32_t)(256.0f * powf(10.0f, db / 20.0f) + 0.5f);
}

void printStatus(Print& out) {
  static const char* loopName[] = {"off", "on", "ptt"};
  out.printf("Audio:     mic %s (SCK %d, WS %d, SD %d, %s channel), PTT GPIO%d %s\r\n",
             s_ok ? "running" : "FAILED", PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD, s_micRight ? "right" : "left",
             PIN_PTT, s_pttDown ? "DOWN" : "up");
#if AUDIO_OUT_I2S
  out.printf("           out: MAX98357A, BCLK GPIO%d, LRC GPIO%d, DIN GPIO%d\r\n", PIN_MIC_SCK, PIN_MIC_WS, PIN_AUDIO_OUT);
#else
  out.printf("           out: sigma-delta on GPIO%d (RC low-pass)\r\n", PIN_AUDIO_OUT);
#endif
  out.printf("           level %.1f dBFS rms, %.1f peak; loop %s, gain %d dB, tone %s; blocks %lu, read errors %lu, underruns %lu\r\n",
             (float)s_rmsDb, (float)s_peakDb, loopName[(int)s_loop], s_gainDb, s_tone ? "on" : "off",
             (unsigned long)s_blocks, (unsigned long)s_micErrors, (unsigned long)s_underruns);
}

}  // namespace audio

#else  // no audio on this board

namespace audio {
void begin() {}
void loop() {}
bool available() { return false; }
bool pttDown() { return false; }
void setLoopback(Loop) {}
void setTone(bool) {}
void setMeter(bool) {}
bool setMicRight(bool) { return false; }
void setGainDb(int) {}
void printStatus(Print& out) { out.print("Audio:     not available on this board\r\n"); }
}  // namespace audio

#endif

#include "voice.h"
#include "audio.h"
#include "config.h"
#include "net.h"
#include "radio.h"
#include "packet.h"
#include "settings.h"
#include "terminal.h"

#if HAS_AUDIO
#include <codec2.h>

namespace voice {

// Header flags byte: codec mode (high 4 bits) | frame count (low 4 bits). Payload: the
// frames only, so Codec2 2400 fits 7 frames (140 ms) in a slot. That margin lets the
// talker catch up after the slots it has to skip (beacon, bench sync).
static const int SLOT_MS = SLOT_US / 1000;
static const uint32_t BUSY_MS = 400;        // floor stays busy this long after the last packet
static const uint32_t HANG_MS = 300;        // keep sending this long after PTT up (flush)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Encoder side (audio task) -> radio task.
static struct CODEC2* s_enc = nullptr;
static int s_mode = CODEC2_MODE_2400;
static int s_spf = 160;                     // samples per codec frame
static int s_bpf = 6;                       // bytes per codec frame
static int16_t s_encBuf[320];
static int s_encFill = 0;
static const int TXQ = 64 * 8;              // bytes
static uint8_t s_txq[TXQ];
static int s_txHead = 0, s_txLen = 0;       // byte ring
static volatile bool s_tx = false;          // holding the floor
static volatile bool s_wantTx = false;      // PTT says talk

// Decoder side: main loop (packets) -> audio task (decode, play).
static struct CODEC2* s_dec = nullptr;
static int s_decMode = -1, s_decSpf = 160, s_decBpf = 6;
static const int RXQ = 64 * 8;
static uint8_t s_rxq[RXQ];
static int s_rxHead = 0, s_rxLen = 0;
static volatile int s_rxMode = CODEC2_MODE_2400;
static int16_t s_pcm[640];
static int s_pcmLen = 0, s_pcmPos = 0;
static bool s_playing = false;
static volatile uint32_t s_lastRxMs = 0, s_lastTxMs = 0;
static volatile bool s_heardAny = false;
static volatile uint8_t s_talker = ADDR_NONE;
static uint16_t s_lastSeq = 0;
static bool s_haveSeq = false;

// Counters.
static volatile uint32_t s_pktTx = 0, s_pktRx = 0, s_lost = 0, s_framesDropped = 0, s_encUs = 0, s_decUs = 0;

static int bytesPerFrame(struct CODEC2* c) { return (codec2_bits_per_frame(c) + 7) / 8; }
static int modeBitrate(int m) {
  return m == CODEC2_MODE_3200 ? 3200 : m == CODEC2_MODE_2400 ? 2400 : m == CODEC2_MODE_1600 ? 1600 : m == CODEC2_MODE_1200 ? 1200 : 0;
}
static int bitrateMode(int b) {
  return b == 3200 ? CODEC2_MODE_3200 : b == 2400 ? CODEC2_MODE_2400 : b == 1600 ? CODEC2_MODE_1600 : b == 1200 ? CODEC2_MODE_1200 : -1;
}
// Codec frames per slot: 120 ms of audio.
static int framesPerSlot(int spf) { return SLOT_MS * 8 / spf; }

static void ringPush(uint8_t* q, int cap, int& head, int& len, const uint8_t* d, int n, int frame) {
  // Drop whole frames from the front to make room (keeps latency bounded).
  while (len + n > cap) {
    head = (head + frame) % cap;
    len -= frame;
    s_framesDropped++;
  }
  for (int i = 0; i < n; i++) q[(head + len + i) % cap] = d[i];
  len += n;
}
static void ringPop(uint8_t* q, int cap, int& head, int& len, uint8_t* d, int n) {
  for (int i = 0; i < n; i++) d[i] = q[(head + i) % cap];
  head = (head + n) % cap;
  len -= n;
}

void begin() {
  s_mode = bitrateMode(g_settings.voiceBitrate);
  if (s_mode < 0) s_mode = CODEC2_MODE_2400;
  s_enc = codec2_create(s_mode);
  if (!s_enc) {
    Serial.print("Voice: codec2_create failed (out of memory?)\r\n");
    return;
  }
  codec2_set_lpc_post_filter(s_enc, 1, 0, 0.8f, 0.2f);
  s_spf = codec2_samples_per_frame(s_enc);
  s_bpf = bytesPerFrame(s_enc);
}

bool setMode(int bitrate) {
  int m = bitrateMode(bitrate);
  if (m < 0 || s_tx) return false;
  struct CODEC2* c = codec2_create(m);
  if (!c) return false;
  codec2_set_lpc_post_filter(c, 1, 0, 0.8f, 0.2f);
  struct CODEC2* old;
  portENTER_CRITICAL(&s_mux);
  old = s_enc;
  s_enc = c;
  s_mode = m;
  s_spf = codec2_samples_per_frame(c);
  s_bpf = bytesPerFrame(c);
  s_encFill = 0;
  s_txLen = 0;
  portEXIT_CRITICAL(&s_mux);
  // The audio task only touches s_enc while s_tx is set, and s_tx is false here.
  if (old) codec2_destroy(old);
  return true;
}

void loop() {
  if (!s_enc || !audio::available()) return;
  uint32_t now = millis();
  bool ptt = audio::pttDown() && g_settings.voiceOn && !audio::codecLoop();
  if (ptt && !s_wantTx && s_mode == CODEC2_MODE_3200) {
    // 3200 needs 8 bytes per 20 ms: 5 frames (100 ms) per 120 ms slot can't keep up.
    static uint32_t lastWarn = 0;
    if (now - lastWarn > 5000) {
      lastWarn = now;
      terminal::printAsync("! Codec2 3200 is for audio loop codec only; set codec 2400 to talk on air");
    }
    return;
  }
  if (ptt && !s_wantTx && !radio::hopping()) {
    static uint32_t lastWarn = 0;
    if (now - lastWarn > 5000) {
      lastWarn = now;
      terminal::printAsync("! Voice needs slots: hop on and time synced (bench or gps)");
    }
    return;
  }
  if (ptt && !s_wantTx) {
    if (floorBusy()) {
      static uint32_t lastWarn = 0;
      if (now - lastWarn > 2000) {
        lastWarn = now;
        terminal::printAsync("! Channel busy: someone else is talking");
      }
      return;  // try again next loop while PTT is held
    }
    portENTER_CRITICAL(&s_mux);
    s_encFill = 0;
    s_txLen = 0;
    portEXIT_CRITICAL(&s_mux);
    s_wantTx = true;
    s_tx = true;
    s_lastTxMs = now;
    terminal::printAsync("[TX voice, Codec2 %d]", modeBitrate(s_mode));
  } else if (!ptt && s_wantTx) {
    s_wantTx = false;  // radio flushes what's queued, then s_tx drops
  }
  if (s_tx && !s_wantTx) {
    portENTER_CRITICAL(&s_mux);
    int left = s_txLen;
    portEXIT_CRITICAL(&s_mux);
    if (left < s_bpf || now - s_lastTxMs > HANG_MS + 500) {
      s_tx = false;
      terminal::printAsync("[TX voice end, %lu packets]", (unsigned long)s_pktTx);
    }
  }
}

void onMicBlock(const int16_t* pcm, int n) {
  // Codec loopback test: encode while PTT is held and feed our own decoder.
  bool codecLoop = audio::codecLoop() && audio::pttDown();
  if (!s_enc || (!codecLoop && (!s_tx || !s_wantTx))) {
    s_encFill = 0;
    return;
  }
  for (int i = 0; i < n; i++) {
    s_encBuf[s_encFill++] = pcm[i];
    if (s_encFill >= s_spf) {
      uint8_t bits[16];
      uint32_t t0 = micros();
      codec2_encode(s_enc, bits, s_encBuf);
      s_encUs = micros() - t0;
      s_encFill = 0;
      if (codecLoop) {
        portENTER_CRITICAL(&s_mux);
        s_rxMode = s_mode;
        ringPush(s_rxq, RXQ, s_rxHead, s_rxLen, bits, s_bpf, s_bpf);
        portEXIT_CRITICAL(&s_mux);
        continue;
      }
      portENTER_CRITICAL(&s_mux);
      // Keep at most three slots of audio queued.
      int cap = 3 * framesPerSlot(s_spf) * s_bpf;
      if (cap > TXQ) cap = TXQ;
      while (s_txLen + s_bpf > cap) {
        s_txHead = (s_txHead + s_bpf) % TXQ;
        s_txLen -= s_bpf;
        s_framesDropped++;
      }
      for (int k = 0; k < s_bpf; k++) s_txq[(s_txHead + s_txLen + k) % TXQ] = bits[k];
      s_txLen += s_bpf;
      portEXIT_CRITICAL(&s_mux);
    }
  }
}

bool transmitting() { return s_tx; }

size_t fillSlot(uint8_t* buf, size_t max) {
  if (!s_tx) return 0;
  static uint16_t seq = 0;
  uint8_t pl[PKT_TEXT_MAX];
  portENTER_CRITICAL(&s_mux);
  int fps = framesPerSlot(s_spf);
  int bpf = s_bpf;
  int have = s_txLen / bpf;
  int maxN = (int)sizeof(pl) / bpf;
  if (maxN > 15) maxN = 15;
  int n = have < maxN ? have : maxN;
  // Wait for a full slot of audio while talking; flush the rest after PTT up.
  if (s_wantTx && have < fps) n = 0;
  if (n > 0) {
    for (int i = 0; i < n * bpf; i++) pl[i] = s_txq[(s_txHead + i) % TXQ];
    s_txHead = (s_txHead + n * bpf) % TXQ;
    s_txLen -= n * bpf;
  }
  portEXIT_CRITICAL(&s_mux);
  if (n <= 0) return 0;
  PktHeader h = {PKT_VOICE, netTag(), 0, (uint8_t)((s_mode << 4) | n), seq++};  // src set by the radio
  size_t len = pktEncode(h, pl, n * bpf, buf, max);
  s_pktTx++;
  s_lastTxMs = millis();
  return len;
}

bool floorBusy() { return s_heardAny && !s_tx && (millis() - s_lastRxMs) < BUSY_MS; }

bool activeRecently() {
  uint32_t now = millis();
  return s_tx || (s_heardAny && now - s_lastRxMs < 2000) || (s_pktTx && now - s_lastTxMs < 2000);
}

void onPacket(uint8_t src, uint16_t seq, uint8_t flags, const uint8_t* p, size_t plen) {
  int mode = flags >> 4, n = flags & 0x0F;
  if (n == 0) return;
  if (modeBitrate(mode) == 0) return;
  uint32_t now = millis();
  if (s_tx) {
    // Both started at once: the lower address keeps the floor.
    if (src < net::myAddr()) {
      s_tx = false;
      s_wantTx = false;
      terminal::printAsync("! Voice collision: address %u has the floor", src);
    } else {
      return;
    }
  }
  bool newTalk = !s_heardAny || now - s_lastRxMs > BUSY_MS || src != s_talker;
  s_lastRxMs = now;
  s_heardAny = true;
  s_talker = src;
  s_pktRx++;
  if (!newTalk && s_haveSeq) {
    uint16_t gap = (uint16_t)(seq - s_lastSeq);
    if (gap > 1 && gap < 50) s_lost += gap - 1;
  }
  s_lastSeq = seq;
  s_haveSeq = true;
  if (newTalk) terminal::printAsync("[RX voice from address %u, Codec2 %d]", src, modeBitrate(mode));

  portENTER_CRITICAL(&s_mux);
  if (mode != s_rxMode) {
    s_rxMode = mode;
    s_rxLen = 0;  // decoder switches on the next frame
  }
  int bpf = (mode == CODEC2_MODE_1600 || mode == CODEC2_MODE_3200) ? 8 : 6;
  if ((int)plen >= n * bpf) ringPush(s_rxq, RXQ, s_rxHead, s_rxLen, p, n * bpf, bpf);
  portEXIT_CRITICAL(&s_mux);
}

bool playBlock(int16_t* out, int n) {
  int mode = s_rxMode;
  if (mode != s_decMode) {
    if (s_dec) codec2_destroy(s_dec);
    s_dec = codec2_create(mode);
    s_decMode = mode;
    if (!s_dec) return false;
    codec2_set_lpc_post_filter(s_dec, 1, 0, 0.8f, 0.2f);
    s_decSpf = codec2_samples_per_frame(s_dec);
    s_decBpf = bytesPerFrame(s_dec);
    s_pcmLen = s_pcmPos = 0;
    s_playing = false;
  }
  if (!s_dec) return false;
  portENTER_CRITICAL(&s_mux);
  int queued = s_rxLen / s_decBpf;
  portEXIT_CRITICAL(&s_mux);
  // Start once 1.5 slots of audio are buffered: covers the skipped slots (manager
  // beacon, bench sync) between packets.
  if (!s_playing) {
    int need = (SLOT_MS * 8 * 3 / 2 + s_decSpf - 1) / s_decSpf;  // >= 180 ms of audio
    if (queued < need) return false;
    s_playing = true;
  }
  int done = 0;
  while (done < n) {
    if (s_pcmPos >= s_pcmLen) {
      uint8_t bits[16];
      bool got = false;
      portENTER_CRITICAL(&s_mux);
      if (s_rxLen >= s_decBpf) {
        ringPop(s_rxq, RXQ, s_rxHead, s_rxLen, bits, s_decBpf);
        got = true;
      }
      portEXIT_CRITICAL(&s_mux);
      if (!got) break;
      uint32_t t0 = micros();
      codec2_decode(s_dec, s_pcm, bits);
      s_decUs = micros() - t0;
      s_pcmLen = s_decSpf;
      s_pcmPos = 0;
    }
    int take = s_pcmLen - s_pcmPos;
    if (take > n - done) take = n - done;
    memcpy(out + done, s_pcm + s_pcmPos, take * sizeof(int16_t));
    s_pcmPos += take;
    done += take;
  }
  if (done < n) {
    memset(out + done, 0, (n - done) * sizeof(int16_t));
    if (done == 0) {
      // Ran dry: stop and wait for a full slot again (end of talk or a lost packet).
      s_playing = false;
      return false;
    }
  }
  return true;
}

void printStatus(Print& out) {
  out.printf("Voice:     %s, Codec2 %d (%d bytes/%d ms frame, %d frames per slot); %s\r\n",
             g_settings.voiceOn ? "on (PTT talks)" : "off", modeBitrate(s_mode), s_bpf, s_spf / 8,
             framesPerSlot(s_spf), s_tx ? "TALKING" : floorBusy() ? "listening" : "idle");
  out.printf("           packets tx %lu, rx %lu, lost %lu, frames dropped %lu; encode %lu us, decode %lu us\r\n",
             (unsigned long)s_pktTx, (unsigned long)s_pktRx, (unsigned long)s_lost,
             (unsigned long)s_framesDropped, (unsigned long)s_encUs, (unsigned long)s_decUs);
}

}  // namespace voice

#else  // no audio

namespace voice {
void begin() {}
void loop() {}
void onMicBlock(const int16_t*, int) {}
bool playBlock(int16_t*, int) { return false; }
bool transmitting() { return false; }
size_t fillSlot(uint8_t*, size_t) { return 0; }
bool floorBusy() { return false; }
void onPacket(uint8_t, uint16_t, uint8_t, const uint8_t*, size_t) {}
bool activeRecently() { return false; }
bool setMode(int) { return false; }
void printStatus(Print& out) { out.print("Voice:     not available on this board\r\n"); }
}  // namespace voice

#endif

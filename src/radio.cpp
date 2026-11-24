#include "radio.h"
#include "voice.h"
#include "config.h"
#include "hopseq.h"
#include "net.h"
#include "packet.h"
#include "settings.h"
#include "timebase.h"
#include <RadioLib.h>
#include <SPI.h>
#include <esp_timer.h>
#if defined(BOARD_HELTEC_V3)
#include <soc/soc.h>
#include <soc/usb_serial_jtag_reg.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace radio {

#ifdef RADIO_SX1262
static SX1262 s_radio = new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
#else
static SX1276 s_radio = new Module(PIN_LORA_CS, PIN_LORA_DIO0, PIN_LORA_RST, PIN_LORA_DIO1);
#endif

// ---------------------------------------------------------------------------
// Shared state

struct TxItem {
  uint8_t buf[PKT_MAX_LEN];
  uint8_t len;
  int8_t stampOffset;
};

struct RxItem {
  uint8_t buf[PKT_MAX_LEN];
  uint8_t len;
  RxInfo info;
};

static QueueHandle_t s_txQueue;
static QueueHandle_t s_rxQueue;
static TaskHandle_t s_task;

static volatile int64_t s_irqLocalUs = 0;
static volatile bool s_irq = false;

static RxHandler s_handler = nullptr;
static volatile SlotFiller s_filler = nullptr;
static Counters s_counters = {};

static volatile int16_t s_pendingChannel = -1;  // set from main loop, applied by task
static volatile int16_t s_pendingPower = -100;

static volatile bool s_hopping = false;
static volatile int64_t s_curSlot = -1;
static volatile uint8_t s_curChannel = 0;
static volatile int32_t s_txLatencyUs = 0;
static bool s_haveLatency = false;
static volatile int64_t s_lastOwnTxSlot = -1000000;

// ---------------------------------------------------------------------------
// Radio helpers (radio task only)

enum class State { Idle, Rx, Tx };
static State s_state = State::Idle;
static int64_t s_txStampLocal = 0;
static uint8_t s_txLen = 0;

static void IRAM_ATTR onRadioIrq() {
  s_irqLocalUs = esp_timer_get_time();
  s_irq = true;
  BaseType_t woken = pdFALSE;
  if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
  if (woken) portYIELD_FROM_ISR();
}

float channelMHz(uint8_t ch) { return CH_BASE_MHZ + CH_STEP_MHZ * ch; }

static void startRx() {
  int st = s_radio.startReceive();
  s_state = (st == RADIOLIB_ERR_NONE) ? State::Rx : State::Idle;
}

static void tune(uint8_t ch) {
#ifdef RADIO_SX1262
  s_radio.setFrequency(channelMHz(ch), true);  // image calibrated once at boot
#else
  s_radio.setFrequency(channelMHz(ch));
#endif
  s_curChannel = ch;
}

uint32_t airtimeUs(size_t len) { return (uint32_t)s_radio.getTimeOnAir(len); }

static void startTx(TxItem& it) {
  int64_t now = timebase::localUs();
  if (it.stampOffset >= 0 && it.stampOffset + 8 <= it.len) {
    int64_t endLocal = now + airtimeUs(it.len) + s_txLatencyUs;
    putI64(it.buf + it.stampOffset, timebase::toNetwork(endLocal));
  }
  s_txStampLocal = now;
  s_txLen = it.len;
  int st = s_radio.startTransmit(it.buf, it.len);
  if (st == RADIOLIB_ERR_NONE) {
    s_state = State::Tx;
    digitalWrite(PIN_LED, HIGH);
    uint8_t ty = it.len ? (it.buf[0] & 0x3F) : 0;
    s_counters.txByType[ty < 9 ? ty : 0]++;
  } else {
    s_counters.txFail++;
    startRx();
  }
}

static void handleIrq() {
  if (!s_irq) return;
  s_irq = false;
  int64_t t = s_irqLocalUs;
  if (s_state == State::Tx) {
    s_radio.finishTransmit();
    digitalWrite(PIN_LED, LOW);
    s_counters.txOk++;
    // Self-calibrate: how much longer than the computed airtime did it take
    // from stamping to TX done? Covers SPI, mode switching and PA ramp.
    int32_t measured = (int32_t)(t - s_txStampLocal - airtimeUs(s_txLen));
    if (measured > -2000 && measured < 20000) {
      if (!s_haveLatency) {
        s_txLatencyUs = measured;
        s_haveLatency = true;
      } else {
        s_txLatencyUs = s_txLatencyUs + (measured - s_txLatencyUs) / 4;
      }
    }
    startRx();
  } else if (s_state == State::Rx) {
    RxItem it;
    size_t len = s_radio.getPacketLength();
    if (len > sizeof(it.buf)) len = sizeof(it.buf);
    int st = s_radio.readData(it.buf, len);
    it.len = (uint8_t)len;
    it.info.rssi = s_radio.getRSSI();
    it.info.snr = s_radio.getSNR();
    it.info.rxDoneLocalUs = t;
    it.info.slot = s_hopping ? s_curSlot : -1;
    it.info.channel = s_curChannel;
    startRx();
    if (st == RADIOLIB_ERR_NONE) {
      s_counters.rxOk++;
      xQueueSend(s_rxQueue, &it, 0);
    } else {
      s_counters.rxCrc++;
    }
  }
}

static void applyPending() {
  int16_t ch = s_pendingChannel;
  if (ch >= 0) {
    s_pendingChannel = -1;
    if (!s_hopping) {
      tune((uint8_t)ch);
      startRx();
    }
  }
  int16_t p = s_pendingPower;
  if (p != -100) {
    s_pendingPower = -100;
    s_radio.setOutputPower((int8_t)p);
    if (s_state != State::Tx) startRx();
  }
}

// Wait until the radio interrupts or the local clock reaches untilLocal.
static void waitUntil(int64_t untilLocal) {
  int64_t left = untilLocal - timebase::localUs();
  if (left > 2500) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((left - 1500) / 1000));
    return;
  }
  while (!s_irq && timebase::localUs() < untilLocal) {
  }
}

static bool fillFromQueueOrFiller(SlotKind kind, int64_t slot, TxItem& it, bool& fromQueue) {
  fromQueue = false;
  if (kind == SlotKind::Own && xQueueReceive(s_txQueue, &it, 0) == pdTRUE) {
    fromQueue = true;
    return true;
  }
  SlotFiller f = s_filler;
  if (!f) return false;
  int stamp = -1;
  size_t n = f(kind, slot, it.buf, sizeof(it.buf), stamp);
  if (!n) return false;
  it.len = (uint8_t)n;
  it.stampOffset = (int8_t)stamp;
  return true;
}

// ---------------------------------------------------------------------------
// The TDMA task

static void runUnslotted() {
  if (s_hopping) {
    s_hopping = false;
    s_curSlot = -1;
    tune(g_settings.channel);
    startRx();
  }
  if (s_state == State::Idle) startRx();
  // With hopping on, a node waiting for time sync only listens.
  bool mayTx = !g_settings.hop || g_settings.timeMode == TimeMode::Local;
  if (s_state == State::Rx && mayTx) {
    TxItem it;
    if (xQueueReceive(s_txQueue, &it, 0) == pdTRUE) startTx(it);
  }
  ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
}

static void runSlotted() {
  static TxItem pending;
  static bool havePending = false;
  static bool pendingFromQueue = false;
  static bool pendingOwn = false;
  static int64_t txAtNet = 0;

  int64_t now = timebase::nowUs();
  int64_t slot = now / SLOT_US;
  int64_t slotStart = slot * SLOT_US;

  if (!s_hopping || slot != s_curSlot) {
    s_hopping = true;
    s_curSlot = slot;
    digitalWrite(PIN_SLOT_DEBUG, HIGH);
    int64_t pulseStart = esp_timer_get_time();
    if (s_state == State::Tx) {  // previous TX ran past the slot end
      s_counters.txOverrun++;
      digitalWrite(PIN_LED, LOW);
    }
    bool benchSync = (g_settings.timeMode == TimeMode::Bench || g_settings.timeMode == TimeMode::BenchMaster) &&
                     (slot % SYNC_PERIOD == 0);
    tune(benchSync ? g_settings.channel : hopseq::channel(slot));
    startRx();
    while (esp_timer_get_time() - pulseStart < 100) {}  // at least 100 us wide, easy to catch
    digitalWrite(PIN_SLOT_DEBUG, LOW);

    havePending = false;
    pendingFromQueue = false;
    pendingOwn = false;
    if (benchSync) {
      if (g_settings.timeMode == TimeMode::BenchMaster)
        havePending = fillFromQueueOrFiller(SlotKind::Sync, slot, pending, pendingFromQueue);
    } else {
      size_t len = 0;
      net::SlotUse use = net::planSlot(slot, pending.buf, sizeof(pending.buf), len);
      // Voice floor: the talker sends in every slot except the manager's beacon
      // slot (position 0 in a managed net); everyone else stays quiet meanwhile.
      bool beaconSlot = use == net::SlotUse::Beacon ||
                        (net::role() != net::Role::Static && net::position(slot) == 0);
      if (voice::transmitting() && !beaconSlot) {
        size_t vlen = voice::fillSlot(pending.buf, sizeof(pending.buf));
        if (vlen >= PKT_HEADER_LEN) pending.buf[2] = net::myAddr();
        pending.len = (uint8_t)vlen;
        pending.stampOffset = -1;
        havePending = vlen > 0;
        use = net::SlotUse::None;
      } else if (voice::floorBusy() && use != net::SlotUse::Beacon) {
        use = net::SlotUse::None;
      }
      if (use == net::SlotUse::Own) {
        havePending = fillFromQueueOrFiller(SlotKind::Own, slot, pending, pendingFromQueue);
        if (havePending && pending.len >= PKT_HEADER_LEN) {
          pending.buf[2] = net::myAddr();  // source = the address we hold now
          pendingOwn = true;
        }
      } else if (use == net::SlotUse::Beacon || use == net::SlotUse::Scheduled) {
        pending.len = (uint8_t)len;
        pending.stampOffset = -1;
        havePending = len > 0;
      }
    }
    txAtNet = slotStart + GUARD_US / 2;
  }

  if (havePending && now >= txAtNet && s_state == State::Rx) {
    // Must finish G/2 before the slot ends.
    int64_t latest = slotStart + SLOT_US - GUARD_US / 2 - airtimeUs(pending.len) - s_txLatencyUs;
    if (now <= latest) {
      startTx(pending);
      if (pendingOwn) s_lastOwnTxSlot = slot;
    } else {
      s_counters.txLate++;
      if (pendingFromQueue) xQueueSendToFront(s_txQueue, &pending, 0);  // retry text next own slot
    }
    havePending = false;
  }

  int64_t nextNet = havePending ? txAtNet : (slot + 1) * SLOT_US;
  waitUntil(timebase::localUs() + (nextNet - timebase::nowUs()));
}

static void radioTask(void*) {
  for (;;) {
    handleIrq();
    applyPending();
    bool slotted = g_settings.hop && timebase::synced() && g_settings.timeMode != TimeMode::Local;
    if (slotted) runSlotted();
    else runUnslotted();
  }
}

// ---------------------------------------------------------------------------
// Public API (main loop side)

bool begin() {
#if defined(BOARD_HELTEC_V3)
  // GPIO19/20 are the S3's native USB pads (the Heltec V3 uses its CP2102 for USB).
  // The USB PHY keeps them after boot, so the slot pulse never reached GPIO19.
  // Hand both pads to the GPIO matrix (this used to happen only in audio::begin,
  // which safe mode skips).
  if (PIN_SLOT_DEBUG == 19 || PIN_SLOT_DEBUG == 20)
    CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
#endif
  pinMode(PIN_SLOT_DEBUG, OUTPUT);
  digitalWrite(PIN_SLOT_DEBUG, LOW);
  SPI.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_CS);
#ifdef RADIO_SX1262
  // Sync word 0x12 maps to 0x1424 on the SX126x, which talks to SX127x private networks.
  int st = s_radio.begin(channelMHz(g_settings.channel), LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_SYNC_WORD,
                         g_settings.powerDbm, LORA_PREAMBLE, LORA_TCXO_V);
#else
  int st = s_radio.begin(channelMHz(g_settings.channel), LORA_BW_KHZ, LORA_SF, LORA_CR, LORA_SYNC_WORD,
                         g_settings.powerDbm, LORA_PREAMBLE);
#endif
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("Radio init failed, code %d\r\n", st);
    return false;
  }
#ifdef RADIO_SX1262
  s_radio.setCRC(2);
  s_radio.setDio1Action(onRadioIrq);  // DIO1 signals both TX done and RX done
#else
  s_radio.setCRC(true);
  s_radio.setDio0Action(onRadioIrq, RISING);
#endif
  s_curChannel = g_settings.channel;
  s_txQueue = xQueueCreate(8, sizeof(TxItem));
  s_rxQueue = xQueueCreate(8, sizeof(RxItem));
  startRx();
  // High priority on core 0; the Arduino loop and terminal run on core 1.
  xTaskCreatePinnedToCore(radioTask, "tdma", 6144, nullptr, configMAX_PRIORITIES - 2, &s_task, 0);
  return true;
}

void loop() {
  RxItem it;
  while (s_rxQueue && xQueueReceive(s_rxQueue, &it, 0) == pdTRUE) {
    if (s_handler) s_handler(it.buf, it.len, it.info);
  }
}

bool send(const uint8_t* data, size_t len, int stampOffset) {
  if (len > PKT_MAX_LEN || !s_txQueue) return false;
  TxItem it;
  memcpy(it.buf, data, len);
  it.len = (uint8_t)len;
  it.stampOffset = (int8_t)stampOffset;
  if (xQueueSend(s_txQueue, &it, 0) != pdTRUE) {
    s_counters.queueFull++;
    return false;
  }
  if (s_task) xTaskNotifyGive(s_task);
  return true;
}

void onReceive(RxHandler h) { s_handler = h; }
void setSlotFiller(SlotFiller f) { s_filler = f; }

bool setChannel(uint8_t ch) {
  if (ch >= CH_COUNT) return false;
  s_pendingChannel = ch;
  return true;
}

bool setPower(int8_t dbm) {
  if (dbm < 2 || dbm > LORA_POWER_MAX) return false;
  s_pendingPower = dbm;
  return true;
}

bool hopping() { return s_hopping; }
int64_t currentSlot() { return s_curSlot; }
uint8_t currentChannel() { return s_curChannel; }
int32_t txLatencyUs() { return s_txLatencyUs; }
int64_t lastOwnTxSlot() { return s_lastOwnTxSlot; }
const Counters& counters() { return s_counters; }

}  // namespace radio

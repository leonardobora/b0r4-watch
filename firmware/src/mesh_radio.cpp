#include "mesh_radio.h"

#include <Arduino.h>
#include <LilyGoLib.h>
#include <RadioLib.h>

#include "mesh_wire.h"   // kSyncWord, kPreambleLength, kBandwidthKHz, ...

namespace meshradio {
namespace {

constexpr size_t kTxQueueLen = 6;
constexpr size_t kRxQueueLen = 8;

QueueHandle_t    s_txQ = nullptr;
QueueHandle_t    s_rxQ = nullptr;
TaskHandle_t     s_task = nullptr;
SemaphoreHandle_t s_mx = nullptr;     // held around every SX1262 transaction

float         s_tcxo = 0.0f;
volatile bool s_ready = false;
volatile bool s_rxOn = false;
volatile bool s_running = false;
float         s_freq = 0.0f;
Stats         s_stats;

// The receive loop. Deliberately a task and not a DIO1 ISR, and deliberately
// POLLING the radio's IRQ line rather than using RadioLib's blocking receive():
// that one spins on the HAL's yield(), and yield() does not let the priority-0
// idle task run, so a priority-2 task on core 0 starves it and trips the task
// watchdog. vTaskDelay at the bottom of every pass is what keeps IDLE0 alive.
void radioTask(void *)
{
    uint8_t buf[255];

    for (;;) {
        if (!s_running) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Outgoing first, so a queued message does not wait out a receive dwell.
        Frame out;
        if (xQueueReceive(s_txQ, &out, 0) == pdTRUE) {
            int16_t st = RADIOLIB_ERR_UNKNOWN;
            if (xSemaphoreTake(s_mx, portMAX_DELAY) == pdTRUE) {
                st = radio.transmit(out.data, out.len);
                if (s_rxOn) radio.startReceive();
                xSemaphoreGive(s_mx);
            }
            if (st == RADIOLIB_ERR_NONE) s_stats.txOk++;
            else                         s_stats.txFail++;
            continue;
        }

        if (s_rxOn && digitalRead(LORA_IRQ)) {
            if (xSemaphoreTake(s_mx, portMAX_DELAY) == pdTRUE) {
                // The length register has to be read before the payload.
                size_t len = radio.getPacketLength();
                if (len > sizeof(buf)) len = sizeof(buf);

                const int16_t st = radio.readData(buf, len);
                if (st == RADIOLIB_ERR_NONE) {
                    Frame f;
                    memcpy(f.data, buf, len);
                    f.len = len;
                    f.rssi = (int16_t)radio.getRSSI();      // the packet's RSSI
                    f.snr = radio.getSNR();
                    s_stats.rxFrames++;
                    s_stats.lastRssi = f.rssi;
                    s_stats.lastSnr = f.snr;
                    // A full queue drops the frame rather than stalling the
                    // radio; the counter above still shows it arrived.
                    xQueueSend(s_rxQ, &f, 0);
                } else {
                    s_stats.rxErrors++;                    // CRC or header error
                }

                radio.finishReceive();                     // clears the IRQ flags
                if (s_rxOn) radio.startReceive();
                xSemaphoreGive(s_mx);
            }
        }

        // Always yield: this is the line that keeps the idle task scheduled.
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

}  // namespace

int16_t begin(float freqMHz, float tcxoVoltage)
{
    if (!s_mx)  s_mx  = xSemaphoreCreateMutex();
    if (!s_txQ) s_txQ = xQueueCreate(kTxQueueLen, sizeof(Frame));
    if (!s_rxQ) s_rxQ = xQueueCreate(kRxQueueLen, sizeof(Frame));
    if (!s_mx || !s_txQ || !s_rxQ) return RADIOLIB_ERR_UNKNOWN;

    instance.lockSPI();

    int16_t st = radio.begin(freqMHz, mesh::kBandwidthKHz, mesh::kSpreadingFactor,
                             mesh::kCodingRate, mesh::kSyncWord,
                             mesh::kTxPowerDbm, mesh::kPreambleLength,
                             tcxoVoltage, false);
    if (st == RADIOLIB_ERR_NONE) {
        // Meshtastic's packet config: explicit header, CRC on, LDRO automatic.
        radio.explicitHeader();
        radio.setCRC(mesh::kCrcEnabled);
        radio.setCurrentLimit(140);          // SX126x PA, as LilyGo's pager does
        radio.setDio2AsRfSwitch(true);
    }

    instance.unlockSPI();

    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[mesh] radio.begin failed: %d\n", (int)st);
        return st;
    }

    // RadioLib has no frequency getter, so the value we set is the value we
    // track. A second source of truth here would only ever disagree.
    s_freq = freqMHz;
    s_tcxo = tcxoVoltage;
    s_ready = true;
    s_running = true;
    s_rxOn = true;
    radio.startReceive();

    if (!s_task) {
        // Core 0: the UI/LVGL work lives on the Arduino core, so the radio gets
        // its own core and never blocks the screen.
        xTaskCreatePinnedToCore(radioTask, "meshradio", 4096, nullptr, 2, &s_task, 0);
    }

    Serial.printf("[mesh] SX1262 ready: %.3f MHz  BW %.1f  SF%u  CR4/%u  sync 0x%02x  "
                  "preamble %u  power %d dBm  tcxo %.1f V\n",
                  (double)s_freq, (double)mesh::kBandwidthKHz, mesh::kSpreadingFactor,
                  mesh::kCodingRate, mesh::kSyncWord, mesh::kPreambleLength,
                  (int)mesh::kTxPowerDbm, (double)tcxoVoltage);
    return RADIOLIB_ERR_NONE;
}

bool ready() { return s_ready; }

int16_t retune(float freqMHz)
{
    if (!s_ready) return RADIOLIB_ERR_UNKNOWN;

    int16_t st = RADIOLIB_ERR_UNKNOWN;
    if (xSemaphoreTake(s_mx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        // Wake first: a sleeping SX1262 ignores SPI writes entirely, so
        // setFrequency() on a slept chip returns -705 and changes nothing.
        radio.standby();
        st = radio.setFrequency(freqMHz);
        if (st == RADIOLIB_ERR_NONE) {
            s_freq = freqMHz;
            if (s_rxOn) radio.startReceive();
        }
        xSemaphoreGive(s_mx);
    }
    if (st == RADIOLIB_ERR_NONE) Serial.printf("[mesh] frequency -> %.3f MHz\n", (double)s_freq);
    else                         Serial.printf("[mesh] setFrequency(%.3f) failed: %d\n",
                                                (double)freqMHz, (int)st);
    return st;
}

float frequency() { return s_freq; }

void startReceive()
{
    s_rxOn = true;
    s_running = true;
    if (s_mx && xSemaphoreTake(s_mx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        radio.startReceive();
        xSemaphoreGive(s_mx);
    }
}

void sleep()
{
    s_rxOn = false;
    s_running = false;
    if (s_mx && xSemaphoreTake(s_mx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        radio.sleep();
        xSemaphoreGive(s_mx);
    }
    Serial.println("[mesh] radio asleep");
}

bool receiving() { return s_rxOn; }

int16_t setTcxo(float volts)
{
    if (!s_ready) return RADIOLIB_ERR_UNKNOWN;

    int16_t st = RADIOLIB_ERR_UNKNOWN;
    if (xSemaphoreTake(s_mx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        radio.standby();
        st = radio.setTCXO(volts);
        if (st == RADIOLIB_ERR_NONE) {
            // A new TCXO voltage changes the clock, so the image rejection
            // calibration from begin() is stale: redo it before listening.
            radio.calibrateImage(s_freq);
            if (s_rxOn) radio.startReceive();
            s_tcxo = volts;
        }
        xSemaphoreGive(s_mx);
    }

    if (st == RADIOLIB_ERR_NONE) Serial.printf("[mesh] TCXO -> %.1f V (re-calibrated)\n", (double)volts);
    else                         Serial.printf("[mesh] setTCXO(%.1f) failed: %d\n", (double)volts, (int)st);
    return st;
}

float tcxo() { return s_tcxo; }

bool queueTx(const uint8_t *frame, size_t len)
{
    if (!s_ready || !s_txQ || len == 0 || len > mesh::kMaxFrame) return false;
    Frame f;
    memcpy(f.data, frame, len);
    f.len = len;
    return xQueueSend(s_txQ, &f, 0) == pdTRUE;
}

size_t poll(Frame *out, size_t maxFrames)
{
    size_t n = 0;
    while (n < maxFrames && xQueueReceive(s_rxQ, &out[n], 0) == pdTRUE) n++;
    return n;
}

int16_t transmitNow(const uint8_t *frame, size_t len)
{
    if (!s_ready) return RADIOLIB_ERR_UNKNOWN;

    int16_t st = RADIOLIB_ERR_UNKNOWN;
    if (xSemaphoreTake(s_mx, pdMS_TO_TICKS(2000)) == pdTRUE) {
        st = radio.transmit((uint8_t *)frame, len);
        if (st == RADIOLIB_ERR_NONE) s_stats.txOk++; else s_stats.txFail++;
        if (s_rxOn) radio.startReceive();
        xSemaphoreGive(s_mx);
    }
    return st;
}

const Stats &stats() { return s_stats; }
void resetStats() { s_stats = Stats{}; }

int16_t noiseFloor(int32_t &rssiDbm)
{
    if (!s_ready) return RADIOLIB_ERR_UNKNOWN;
    if (xSemaphoreTake(s_mx, pdMS_TO_TICKS(3000)) != pdTRUE) return RADIOLIB_ERR_UNKNOWN;

    const int16_t st = radio.startReceive();
    int32_t best = -200, sum = 0;
    for (int i = 0; i < 12; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
        const int16_t r = (int16_t)radio.getRSSI(false);   // instantaneous, not the last packet's
        if (r > best) best = r;
        sum += r;
    }
    if (s_rxOn) radio.startReceive();
    xSemaphoreGive(s_mx);

    rssiDbm = sum / 12;
    return st;
}

}  // namespace meshradio

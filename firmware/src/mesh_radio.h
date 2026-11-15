/*
 * mesh_radio.h — the SX1262 side of the mesh: modem configuration, a receive
 * task, and the counters that let a host prove the radio is alive over serial.
 *
 * Device only (RadioLib + FreeRTOS). The wire format lives in lib/mesh/ and is
 * what the host tests exercise.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace meshradio {

struct Frame {
    uint8_t data[255] = {0};
    size_t  len = 0;
    int16_t rssi = 0;    // dBm
    float   snr = 0.0f;  // dB
};

// Configure the modem for Meshtastic LongFast at `freqMHz` and start the receive
// task. Returns a RadioLib error code (0 == RADIOLIB_ERR_NONE).
int16_t begin(float freqMHz, float tcxoVoltage = 1.6f);
bool    ready();

// Change frequency and restart RX. Returns a RadioLib error code.
// Safe after sleep(): the chip is woken first, because an SX1262 in sleep mode
// does not answer SPI commands and a bare setFrequency() fails with -705.
int16_t retune(float freqMHz);
float   frequency();

// Change the DIO3 TCXO control voltage and re-calibrate. Meshtastic's variant
// for this board says 1.8 V; RadioLib's default is 1.6 V. Live so both can be
// compared on the hardware instead of argued about.
int16_t setTcxo(float volts);
float   tcxo();

// Radio on/off. Off means the chip is asleep, which is the single biggest saving
// on a 470 mAh battery.
void startReceive();
void sleep();
bool receiving();

// Hand a frame to the radio task. False when the TX queue is full.
bool queueTx(const uint8_t *frame, size_t len);
// Non-blocking: drain received frames. Returns how many were written.
size_t poll(Frame *out, size_t maxFrames);

// Transmit right now, blocking (used by self-tests and the status commands).
int16_t transmitNow(const uint8_t *frame, size_t len);

struct Stats {
    uint32_t rxFrames = 0;    // LoRa frames delivered by the radio (CRC passed)
    uint32_t txOk = 0;
    uint32_t txFail = 0;
    uint32_t rxErrors = 0;    // CRC/sync failures the radio reported
    int16_t  lastRssi = 0;
    float    lastSnr = 0.0f;
};
const Stats &stats();
void resetStats();

// A measurement that needs no second node: measure the noise floor in RX. A
// configured, PLL-locked SX1262 reads roughly -105..-125 dBm; a broken TCXO or
// antenna path reads -128 or fails outright. This is how a headless agent can
// tell "the chip is alive" from "the chip is silent".
int16_t noiseFloor(int32_t &rssiDbm);

}  // namespace meshradio

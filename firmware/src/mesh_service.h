/*
 * mesh_service.h — the mesh's state and policy: the node table, the inbox, the
 * packet-id counter, the region, and the JSON the phone reads.
 *
 * main.cpp owns the pixels; everything else about the mesh lives here so the
 * serial commands, the HTTP routes and the watch screen all read the same state.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mesh_node.h"

namespace meshsvc {

// Regions we can transmit in. The frequency is not a constant of the region: it
// follows from the primary channel's name hash (see mesh_wire.h), and the two
// differ because BR_902 is only 22 slots wide.
constexpr int kRegionCount = 2;

// Loads settings, resolves the key, configures the radio and starts the receive
// task. Returns false if the radio did not come up.
bool begin(uint32_t selfNum, const char *longName, const char *shortName);

// Drain received frames and act on them. Call once per main-loop pass.
void loop();

// The wall clock, fed in by main.cpp from the RTC once a second. The mesh
// schedules the periodic NodeInfo off this rather than off millis(): on this
// board millis()-derived scheduling stalls while the CPU is idle (measured: 11 s
// counted for 220 s of wall clock), and the RTC is a separate chip that keeps
// perfect time regardless. 0 (or a bogus value) means "no clock yet".
void setWallClock(uint32_t utcSeconds);

bool  sendText(const char *text, uint32_t to);   // to == mesh::kBroadcast for all
void  announce();                                // broadcast our NodeInfo now

bool  enabled();
void  setEnabled(bool on);

int         regionIndex();
const char *regionName();
float       frequency();
bool        setRegion(int index);

// Retune within the current region, without changing the region. This is the
// escape hatch for the one number in this design that cannot be settled from a
// desk: which frequency the nodes actually in range are using.
bool setFrequency(float mhz);

// DIO3 TCXO control voltage (default 1.8 V, which is what Meshtastic's variant
// for this board uses). Persisted, so an A/B test survives a reboot.
bool  setTcxoVoltage(float volts);
float tcxoVoltage();

// True exactly once after a message arrives, so the UI can buzz and redraw.
bool takeMessageFlag();

uint32_t selfNum();
uint8_t  channelHash();
const mesh::NodeDb  &nodes();
const mesh::Mailbox &inbox();

struct Counters {
    uint32_t rxFrames = 0;   // frames the radio delivered
    uint32_t rxText = 0;
    uint32_t rxNodeInfo = 0;
    uint32_t rxOther = 0;
    uint32_t notForUs = 0;
    uint32_t badProto = 0;      // decrypted to no valid protobuf (foreign key/protocol)
    uint32_t shortFrames = 0;   // CRC-valid but shorter than a Meshtastic header
    uint32_t tx = 0;
    uint32_t txFail = 0;
    uint32_t crcErrors = 0;
    int16_t  lastRssi = 0;
    float    lastSnr = 0.0f;
    uint32_t lastRxS = 0;    // uptime seconds of the last decoded frame
};
const Counters &counters();

// Serial-facing dumps. Each returns the number of characters written.
size_t statusText(char *out, size_t n);
// The internals behind the status: uptime, the announce countdown, the radio
// flags and the loop task's spare stack. Exists because "the periodic announce
// never fired" was invisible from the outside.
size_t debugText(char *out, size_t n);
size_t nodesText(char *out, size_t n);
size_t inboxText(char *out, size_t n);
// Park the radio in RX and measure the noise floor; also re-tries a couple of
// TCXO voltages so a wrong one shows up as an implausible reading instead of
// silence. Writes a human report, returns false if the radio never came up.
bool radioSelfTest(char *out, size_t n);

// Phone-facing JSON.
size_t jsonStatus(char *out, size_t n);
size_t jsonNodes(char *out, size_t n);
size_t jsonInbox(char *out, size_t n, uint32_t sinceId);

// Percent-decoding for query strings: the watch assigns meaning to the argument
// values, so "%20" has to become a space before it reaches a message.
size_t urlDecode(char *dst, size_t dstN, const char *src);

}  // namespace meshsvc

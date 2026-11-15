/*
 * mesh_wire.h — the Meshtastic on-air protocol, as this firmware speaks it.
 *
 * Everything here was taken from meshtastic/firmware sources (not from memory):
 *   src/mesh/RadioInterface.h   PacketHeader layout, PACKET_FLAGS_* masks
 *   src/mesh/RadioInterface.cpp applyModemConfig() (frequency/slot derivation)
 *   src/mesh/RadioLibInterface.h  syncWord 0x2b, preambleLength 16
 *   src/mesh/Channels.cpp/.h    xorHash, generateHash, defaultpsk, getName()
 *   src/mesh/CryptoEngine.cpp   nonce layout, AES-CTR with setCounterSize(4)
 *   ../pb/mesh.proto, ../pb/portnums.proto   message and portnum definitions
 * The full write-up with file/line citations is mesh/docs/meshtastic-onair-spec.md.
 *
 * Pure logic: no Arduino, no radio. The point is that the bytes are testable on
 * the build host, because a wrong byte here is silent over the air.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mesh_defs.h"

namespace mesh {

// --------------------------------------------------------------- LoRa modem
// Meshtastic's LongFast preset. ldrOptimize is automatic in the firmware:
// (1 << sf) / bw >= 16 -> for SF11/BW250 that is 8.19, so LDRO stays off.
constexpr float    kBandwidthKHz    = 250.0f;
constexpr uint8_t  kSpreadingFactor = 11;
constexpr uint8_t  kCodingRate      = 5;      // RadioLib numbering: 5 == 4/5
constexpr uint8_t  kSyncWord        = 0x2b;
constexpr uint16_t kPreambleLength  = 16;
constexpr bool     kCrcEnabled      = true;
// Region power limits: BR_902 and US are both 30 dBm. The SX1262 PA tops out
// around 22 dBm, and this watch's antenna is a chip/wire part, so 20 is honest.
constexpr int8_t   kTxPowerDbm      = 20;

// A region we can operate in. The frequency is NOT a constant of the region:
// the firmware derives it from the primary channel's effective name (see
// regionFrequencyMHz below), which is why this is computed, not hardcoded.
struct Region {
    const char *name;
    float    freqStart, freqEnd;   // MHz band edges
    float    freq;                 // MHz, derived for the primary channel
};

// djb2, exactly as meshtastic/firmware's hash() (RadioInterface.cpp).
uint32_t djb2(const char *s);

// The frequency the firmware computes: freqStart + bw/2000 + slot*slotWidth,
// where slotWidth = bw/1000 and slot = djb2(effectiveName) % numSlots.
// Cross-check: US (902-928, 250 kHz) with "LongFast" gives 906.875 MHz, which is
// the published default for that region - so this derivation is the right one.
float regionFrequencyMHz(float freqStart, float freqEnd, float bwKHz, const char *effectiveName);
uint32_t regionSlots(float freqStart, float freqEnd, float bwKHz);

// Brazil: ANATEL 902-907.5 MHz. Only 22 slots wide, so LongFast's default slot
// (djb2 % 22) is NOT the same frequency as in the US: 903.875 vs 906.875 MHz.
constexpr const char *kRegionBR902Name = "BR_902";
constexpr const char *kRegionUSName    = "US";
float regionFrequencyBR902();
float regionFrequencyUS();

// ------------------------------------------------------------------ channel
extern const uint8_t kDefaultPsk[16];

// Xor of every byte - Meshtastic's xorHash().
uint8_t xorHash(const uint8_t *p, size_t n);

// The 1-byte psk alias -> real 16-byte key (Channels::getKey): alias 1 is the
// default, alias V bumps the last byte by V-1. Alias 0 means "no encryption".
void resolveKey(uint8_t pskIndex, uint8_t out[16]);

// The channel hash byte, xorHash(name) ^ xorHash(key). For the default primary
// channel the effective name is the preset name "LongFast" (getName() falls
// back to it when the stored name is empty), giving 0x08.
uint8_t channelHashFor(const char *effectiveName, const uint8_t *key, size_t keyLen);
constexpr uint8_t kPrimaryChannelHash = 0x08;

// ------------------------------------------------------------------- header
// Wire layout: 16 bytes, little-endian, matching PacketHeader exactly.

struct Header {
    uint32_t to = kBroadcast;
    uint32_t from = 0;
    uint32_t id = 0;
    uint8_t  flags = 0;
    uint8_t  channel = kPrimaryChannelHash;
    uint8_t  nextHop = 0;
    uint8_t  relayNode = 0;

    uint8_t hopLimit() const { return flags & 0x07; }
    uint8_t hopStart() const { return (uint8_t)((flags & 0xE0) >> 5); }
    bool    wantAck() const { return (flags & 0x08) != 0; }
    bool    viaMqtt() const { return (flags & 0x10) != 0; }

    void setHops(uint8_t limit, uint8_t start)
    {
        flags = (uint8_t)((flags & ~0xE7) | (limit & 0x07) | ((start & 0x07) << 5));
    }
    void setWantAck(bool on)
    {
        flags = on ? (uint8_t)(flags | 0x08) : (uint8_t)(flags & ~0x08);
    }

    // How far away the sender is, as Meshtastic computes it.
    uint8_t hopsAway() const
    {
        return hopStart() > hopLimit() ? (uint8_t)(hopStart() - hopLimit()) : 0;
    }

    void writeTo(uint8_t *out) const;                        // 16 bytes
    static bool parse(const uint8_t *in, size_t n, Header &h);
};

// ------------------------------------------------------------------- crypto
// nonce = LE64(packetId) || LE32(fromNode) || 00000000, counter = last 4 bytes.
// Meshtastic calls setCounterSize(4); incrementing all 16 bytes big-endian is
// identical here because bytes 12..15 start at zero and a frame is <= 15 blocks.
void buildNonce(uint64_t packetId, uint32_t fromNode, uint8_t nonce[16]);
void cryptPayload(const uint8_t key[16], uint64_t id, uint32_t from,
                  uint8_t *payload, size_t len);   // in place; CTR is symmetric

// -------------------------------------------------------------- the messages
enum PortNum : uint32_t {
    PORT_TEXT      = 1,
    PORT_POSITION  = 3,
    PORT_NODEINFO  = 4,
    PORT_ROUTING   = 5,
    PORT_ADMIN     = 6,
    PORT_TELEMETRY = 67,
};

// hardware model: meshtastic_HardwareModel_T_WATCH_S3 (mesh.proto)
constexpr uint32_t kHwModelWatchS3 = 51;

// DeviceConfig.Role. We advertise CLIENT_MUTE on purpose: this watch does not
// rebroadcast other nodes' packets, and claiming CLIENT (which relays) would
// invite traffic we cannot forward.
constexpr uint32_t kRoleClientMute = 1;

// Data { portnum = 1; payload = 2; }. Returns 0 if it does not fit.
size_t encodeData(uint8_t *out, size_t cap, uint32_t portnum,
                  const uint8_t *payload, size_t n);
bool   parseData(const uint8_t *in, size_t n, uint32_t &portnum,
                 const uint8_t *&payload, size_t &payloadLen);

struct UserInfo {
    char     id[16] = {0};          // "!a1b2c3d4"
    char     longName[40] = {0};
    char     shortName[5] = {0};
    uint32_t hwModel = 0;
    uint32_t role = 0;
};

size_t encodeUser(uint8_t *out, size_t cap, const char *id, const char *longName,
                  const char *shortName, uint32_t hwModel,
                  uint32_t role = kRoleClientMute);
bool   parseUser(const uint8_t *in, size_t n, UserInfo &u);

// Position: only the fields worth carrying on a 240px screen.
struct PositionInfo {
    bool     valid = false;
    int32_t  latI = 0;      // degrees * 1e7
    int32_t  lonI = 0;
    int32_t  altitudeM = 0;
    bool     hasAltitude = false;
};
bool parsePosition(const uint8_t *in, size_t n, PositionInfo &p);

// --------------------------------------------------------------- the packets
struct Packet {
    Header  header;
    uint8_t payload[kMaxPayload] = {0};
    size_t  payloadLen = 0;

    size_t frameLen() const { return kHeaderLen + payloadLen; }
    // Writes header + payload into `out` (already-encrypted payload).
    bool   writeFrame(uint8_t *out, size_t cap) const;
};

// Builders. `id` must be unique per sender; the caller owns that.
bool buildText(Packet &p, uint32_t selfNum, uint32_t id, const char *text,
               uint32_t to, const uint8_t key[16], uint8_t channelHash);
bool buildNodeInfo(Packet &p, uint32_t selfNum, uint32_t id, const char *longName,
                   const char *shortName, uint32_t hwModel,
                   const uint8_t key[16], uint8_t channelHash,
                   uint32_t role = kRoleClientMute);

// ----------------------------------------------------------------- receiving
enum class RxStatus {
    Ok = 0,
    TooShort,        // fewer bytes than a header
    NotForUs,        // addressed to another node
    BadChannel,      // channel hash is not ours (only when strict)
    Crypto,          // decrypted bytes are not a valid Data message
    Unknown,         // parsed, but a portnum we do not handle
};

struct Inbound {
    Header   header;
    uint32_t portnum = 0;
    uint8_t  decrypted[kMaxPayload] = {0};
    size_t   decryptedLen = 0;
    const uint8_t *payload = nullptr;      // into decrypted[]
    size_t   payloadLen = 0;
    UserInfo user;                          // filled for PORT_NODEINFO
    PositionInfo position;                  // filled for PORT_POSITION
};

// Decrypt + parse. The channel byte is only a hint in Meshtastic ("used as a
// hint for the decoder to limit which channels we consider"), so by default we
// accept any hash and let the protobuf parse be the real check; pass
// strictChannel to demand a match. `myNum` (0 = do not check) rejects packets
// addressed to somebody else, which also throws out most wrong-key garbage.
RxStatus parseFrame(const uint8_t *frame, size_t n, const uint8_t key[16],
                    uint8_t ourChannelHash, uint32_t myNum, Inbound &out,
                    bool strictChannel = false);

}  // namespace mesh

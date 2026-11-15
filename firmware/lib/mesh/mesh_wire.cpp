#include "mesh_wire.h"

#include <stdio.h>
#include <string.h>

#include "aes128.h"
#include "mesh_node.h"   // copyStr
#include "proto.h"

namespace mesh {

// The well-known Meshtastic default key (Channels.h: defaultpsk). A channel
// whose psk is the single byte 0x01 ("AQ==") resolves to exactly this.
const uint8_t kDefaultPsk[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

// ------------------------------------------------------------------ region
// djb2 by Dan Bernstein, byte for byte the firmware's hash() in
// RadioInterface.cpp (including its treatment of the byte as unsigned).
uint32_t djb2(const char *s)
{
    uint32_t h = 5381;
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        h = ((h << 5) + h) + (uint32_t)(*p);
    return h;
}

uint32_t regionSlots(float freqStart, float freqEnd, float bwKHz)
{
    // PROFILE_STD has spacing = 0 and padding = 0, so the slot width is the
    // bandwidth itself: slotWidth = spacing + padding*2 + bw/1000.
    const float slotWidth = bwKHz / 1000.0f;
    if (slotWidth <= 0.0f) return 1;
    // round((freqEnd - freqStart + spacing) / slotWidth)
    return (uint32_t)((freqEnd - freqStart) / slotWidth + 0.5f);
}

float regionFrequencyMHz(float freqStart, float freqEnd, float bwKHz, const char *effectiveName)
{
    const float    slotWidth = bwKHz / 1000.0f;
    const uint32_t slots     = regionSlots(freqStart, freqEnd, bwKHz);
    const uint32_t slot      = slots ? (uint32_t)(djb2(effectiveName) % slots) : 0;
    // freqStart is the band edge: add half a bandwidth to land in the middle of
    // the first slot, then step by the slot width.
    return freqStart + (bwKHz / 2000.0f) + (float)slot * slotWidth;
}

float regionFrequencyBR902()
{
    return regionFrequencyMHz(902.0f, 907.5f, kBandwidthKHz, "LongFast");
}

float regionFrequencyUS()
{
    return regionFrequencyMHz(902.0f, 928.0f, kBandwidthKHz, "LongFast");
}

// ------------------------------------------------------------------ channel
uint8_t xorHash(const uint8_t *p, size_t n)
{
    uint8_t code = 0;
    for (size_t i = 0; i < n; i++) code ^= p[i];
    return code;
}

void resolveKey(uint8_t pskIndex, uint8_t out[16])
{
    if (pskIndex == 0) {          // "user disabled encryption"
        memset(out, 0, 16);
        return;
    }
    memcpy(out, kDefaultPsk, 16);
    // Channels.cpp: *last = *last + pskIndex - 1; index 1 means no change.
    out[15] = (uint8_t)(kDefaultPsk[15] + pskIndex - 1);
}

uint8_t channelHashFor(const char *effectiveName, const uint8_t *key, size_t keyLen)
{
    return (uint8_t)(xorHash((const uint8_t *)effectiveName, strlen(effectiveName)) ^
                     xorHash(key, keyLen));
}

// ------------------------------------------------------------------- header
void Header::writeTo(uint8_t *out) const
{
    put32le(out + 0, to);
    put32le(out + 4, from);
    put32le(out + 8, id);
    out[12] = flags;
    out[13] = channel;
    out[14] = nextHop;
    out[15] = relayNode;
}

bool Header::parse(const uint8_t *in, size_t n, Header &h)
{
    if (n < kHeaderLen) return false;
    h.to        = get32le(in + 0);
    h.from      = get32le(in + 4);
    h.id        = get32le(in + 8);
    h.flags     = in[12];
    h.channel   = in[13];
    h.nextHop   = in[14];
    h.relayNode = in[15];
    return true;
}

// ------------------------------------------------------------------- crypto
void buildNonce(uint64_t packetId, uint32_t fromNode, uint8_t nonce[16])
{
    memset(nonce, 0, 16);
    for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)((packetId >> (8 * i)) & 0xff);
    for (int i = 0; i < 4; i++) nonce[8 + i] = (uint8_t)((fromNode >> (8 * i)) & 0xff);
}

void cryptPayload(const uint8_t key[16], uint64_t id, uint32_t from,
                  uint8_t *payload, size_t len)
{
    if (!len) return;
    uint8_t nonce[16];
    buildNonce(id, from, nonce);
    aes128Ctr(key, nonce, payload, payload, len);
}

// ----------------------------------------------------------------- messages
size_t encodeData(uint8_t *out, size_t cap, uint32_t portnum,
                  const uint8_t *payload, size_t n)
{
    Writer w(out, cap);
    w.putVarintField(1, portnum);
    if (n) w.putBytes(2, payload, n);
    return w.ok() ? w.size() : 0;
}

bool parseData(const uint8_t *in, size_t n, uint32_t &portnum,
               const uint8_t *&payload, size_t &payloadLen)
{
    uint64_t port = 0;
    if (!findVarint(in, n, 1, port)) return false;
    if (port > 511) return false;            // portnums.proto: MAX = 511
    portnum = (uint32_t)port;

    payload = nullptr;
    payloadLen = 0;
    findBytes(in, n, 2, payload, payloadLen);
    return true;
}

size_t encodeUser(uint8_t *out, size_t cap, const char *id, const char *longName,
                  const char *shortName, uint32_t hwModel, uint32_t role)
{
    Writer w(out, cap);
    w.putString(1, id ? id : "");
    w.putString(2, longName ? longName : "");
    w.putString(3, shortName ? shortName : "");
    w.putVarintField(5, hwModel);
    // Protobuf omits zero-valued fields, so only a non-CLIENT role is written.
    if (role) w.putVarintField(7, role);
    return w.ok() ? w.size() : 0;
}

bool parseUser(const uint8_t *in, size_t n, UserInfo &u)
{
    const uint8_t *p = nullptr;
    size_t l = 0;

    if (findBytes(in, n, 1, p, l)) copyStr(u.id, sizeof(u.id), (const char *)p, l);
    if (findBytes(in, n, 2, p, l)) copyStr(u.longName, sizeof(u.longName), (const char *)p, l);
    if (findBytes(in, n, 3, p, l)) copyStr(u.shortName, sizeof(u.shortName), (const char *)p, l);

    uint64_t v = 0;
    if (findVarint(in, n, 5, v)) u.hwModel = (uint32_t)v;
    if (findVarint(in, n, 7, v)) u.role = (uint32_t)v;
    return u.id[0] || u.longName[0] || u.shortName[0];
}

bool parsePosition(const uint8_t *in, size_t n, PositionInfo &out)
{
    out = PositionInfo{};

    size_t off = 0;
    Field f;
    while (nextField(in, n, off, f)) {
        if (f.num == 1 && f.wire == 5) { out.latI = (int32_t)f.v; out.valid = true; }
        else if (f.num == 2 && f.wire == 5) { out.lonI = (int32_t)f.v; out.valid = true; }
        else if (f.num == 3 && f.wire == 0) { out.altitudeM = (int32_t)(int64_t)f.v; out.hasAltitude = true; }
    }
    // A position of exactly 0,0 is the "not set" state Meshtastic never sends on
    // purpose, and it is what garbage-decrypts to most often.
    if (out.latI == 0 && out.lonI == 0) out.valid = false;
    return out.valid;
}

// ------------------------------------------------------------------ packets
bool Packet::writeFrame(uint8_t *out, size_t cap) const
{
    if (frameLen() > cap) return false;
    header.writeTo(out);
    memcpy(out + kHeaderLen, payload, payloadLen);
    return true;
}

bool buildText(Packet &p, uint32_t selfNum, uint32_t id, const char *text,
               uint32_t to, const uint8_t key[16], uint8_t channelHash)
{
    if (!text) return false;
    const size_t n = strlen(text);

    uint8_t data[kMaxPayload];
    const size_t dn = encodeData(data, sizeof(data), PORT_TEXT, (const uint8_t *)text, n);
    if (!dn) return false;                    // the Data wrapper did not fit

    p.header = Header{};
    p.header.to = to;
    p.header.from = selfNum;
    p.header.id = id;
    p.header.channel = channelHash;
    p.header.setHops(3, 3);                   // HOP_RELIABLE, and we are the origin
    memcpy(p.payload, data, dn);
    p.payloadLen = dn;
    cryptPayload(key, id, selfNum, p.payload, p.payloadLen);
    return true;
}

bool buildNodeInfo(Packet &p, uint32_t selfNum, uint32_t id, const char *longName,
                   const char *shortName, uint32_t hwModel,
                   const uint8_t key[16], uint8_t channelHash, uint32_t role)
{
    char idStr[16];
    snprintf(idStr, sizeof(idStr), "!%08lx", (unsigned long)selfNum);

    uint8_t user[128];
    const size_t un = encodeUser(user, sizeof(user), idStr, longName, shortName, hwModel, role);
    if (!un) return false;

    uint8_t data[kMaxPayload];
    const size_t dn = encodeData(data, sizeof(data), PORT_NODEINFO, user, un);
    if (!dn) return false;

    p.header = Header{};
    p.header.to = kBroadcast;                 // node info is always broadcast
    p.header.from = selfNum;
    p.header.id = id;
    p.header.channel = channelHash;
    p.header.setHops(3, 3);
    memcpy(p.payload, data, dn);
    p.payloadLen = dn;
    cryptPayload(key, id, selfNum, p.payload, p.payloadLen);
    return true;
}

// ---------------------------------------------------------------- receiving
RxStatus parseFrame(const uint8_t *frame, size_t n, const uint8_t key[16],
                    uint8_t ourChannelHash, uint32_t myNum, Inbound &out,
                    bool strictChannel)
{
    out = Inbound{};

    if (!frame || n < kHeaderLen) return RxStatus::TooShort;
    if (!Header::parse(frame, n, out.header)) return RxStatus::TooShort;

    if (myNum != 0 && out.header.to != kBroadcast && out.header.to != myNum)
        return RxStatus::NotForUs;
    if (strictChannel && out.header.channel != ourChannelHash)
        return RxStatus::BadChannel;

    size_t clen = n - kHeaderLen;
    if (clen > kMaxPayload) clen = kMaxPayload;
    memcpy(out.decrypted, frame + kHeaderLen, clen);
    out.decryptedLen = clen;

    // CTR has no tag, so a wrong key produces plausible-looking bytes. The check
    // that matters is whether the plaintext parses as the protobuf we expect.
    cryptPayload(key, out.header.id, out.header.from, out.decrypted, clen);

    uint32_t port = 0;
    const uint8_t *pl = nullptr;
    size_t plen = 0;
    if (!parseData(out.decrypted, clen, port, pl, plen)) return RxStatus::Crypto;

    out.portnum = port;
    out.payload = pl;
    out.payloadLen = plen;

    switch (port) {
        case PORT_NODEINFO:
            parseUser(pl, plen, out.user);
            return RxStatus::Ok;
        case PORT_POSITION:
            parsePosition(pl, plen, out.position);
            return RxStatus::Ok;
        case PORT_TEXT:
            return RxStatus::Ok;
        default:
            return RxStatus::Unknown;      // parsed fine, we just do not act on it
    }
}

}  // namespace mesh

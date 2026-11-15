#include "mesh_service.h"

#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mesh_radio.h"
#include "mesh_wire.h"

namespace meshsvc {

using namespace mesh;

namespace {

struct RegionDef {
    const char *name;
    float       freq;
};

RegionDef regionDefs[kRegionCount] = {
    {"BR_902", 0.0f},   // filled at begin(): the value is derived, not typed in
    {"US", 0.0f},
};

Preferences prefs;

NodeDb   gNodes;
Mailbox  gInbox;

uint8_t  gKey[16];
uint8_t  gChannel = kPrimaryChannelHash;
uint32_t gSelf = 0;
char     gLongName[kLongNameN] = "T-Watch S3";
char     gShortName[kShortNameN] = "BORA";   // Meshtastic short names are <= 4 chars
bool     gEnabled = false;
bool     gMessageFlag = false;
int      gRegion = 0;
uint32_t gPacketId = 0;
Counters gCounters;

// Announce bookkeeping. A plain countdown, ticked once a second and printed in
// the status text: the previous form differenced two uptimes inside an if() and
// silently never fired, and a number that never moves is much easier to see.
uint32_t gSinceAnnounceS = 0;
uint32_t gLastTickMs = 0;
uint32_t gLoopPasses = 0;

// Wall clock (from the RTC via main.cpp) and the wall-second of the last
// announce. When we have a clock, this is what the interval is measured against.
uint32_t gWallUtc = 0;
uint32_t gLastAnnounceWall = 0;

constexpr uint32_t kAnnounceEveryS = 300;    // NodeInfo every 5 minutes

// DIO3 TCXO control voltage. 1.8 V is what meshtastic/firmware's t-watch-s3
// variant sets; RadioLib's own default would be 1.6 V, and LilyGo's library
// never sets it at all. Persisted so the choice can be measured, not asserted.
constexpr float kDefaultTcxoVolts = 1.8f;
float gTcxoVolts = kDefaultTcxoVolts;

uint32_t uptimeS() { return millis() / 1000UL; }

void printHex(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) Serial.printf("%02x", p[i]);
}

// A frame we could not use, printed with its reason. The tag is deliberately
// "rx short" / "rx foreign" and not "rx frame": the independent decoder keys on
// the latter, and it should only ever be handed frames that claim to be ours.
void printRawFrame(const char *why, const meshradio::Frame &f, const char *reason)
{
    Serial.printf("[mesh] rx %s frame ", why);
    printHex(f.data, f.len > 64 ? 64 : f.len);
    Serial.printf("%s rssi=%d snr=%.2f  %s (%u bytes)\n", f.len > 64 ? "..." : "",
                  (int)f.rssi, (double)f.snr, reason, (unsigned)f.len);
}

// Every frame we decode, in the format tools/verify_frames.py reads.
void printFrame(const char *dir, const uint8_t *frame, size_t n, const meshradio::Frame *meta)
{
    Serial.printf("[mesh] %s frame ", dir);
    printHex(frame, n);
    if (meta) Serial.printf(" rssi=%d snr=%.2f", (int)meta->rssi, (double)meta->snr);
    Serial.println();
}

void append(char *out, size_t n, size_t &off, const char *fmt, ...)
{
    if (off >= n) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(out + off, n - off, fmt, ap);
    va_end(ap);
    if (w > 0) off += (size_t)w;
    if (off >= n) off = n - 1;
}

// A quote or a control character inside a received text would break the JSON the
// phone parses, so it never gets out of here.
void jsonEscape(char *s)
{
    for (char *q = s; *q; ++q) {
        if (*q == '"' || *q == '\\' || (unsigned char)*q < 0x20) *q = '\'';
    }
}

void onText(const Inbound &in, const meshradio::Frame &f)
{
    char text[kMsgTextN];
    copyStr(text, sizeof(text), (const char *)in.payload, in.payloadLen);
    jsonEscape(text);

    if (!gInbox.push(text, in.header.from, in.header.to, in.header.id,
                     (int8_t)(f.snr >= 0 ? f.snr + 0.5f : f.snr - 0.5f),
                     in.header.hopsAway(), false, uptimeS())) {
        Serial.println("[mesh] text did not fit the inbox slot, dropped");
        return;
    }

    gCounters.rxText++;
    gMessageFlag = true;

    char name[48];
    const Node *n = gNodes.find(in.header.from);
    if (n) NodeDb::displayName(*n, name, sizeof(name));
    else   NodeDb::hexId(in.header.from, name, sizeof(name));

    Serial.printf("[mesh] text from %s (%u hop%s, snr %.1f): %s\n", name,
                  in.header.hopsAway(), in.header.hopsAway() == 1 ? "" : "s",
                  (double)f.snr, text);
}

void onNodeInfo(const Inbound &in)
{
    Node *n = gNodes.upsert(in.header.from, in.user.longName[0] ? in.user.longName : nullptr,
                            in.user.shortName[0] ? in.user.shortName : nullptr, uptimeS());
    if (!n) return;
    n->hwModel = (uint8_t)in.user.hwModel;
    n->role = (uint8_t)in.user.role;

    gCounters.rxNodeInfo++;
    Serial.printf("[mesh] node info: %s \"%s\" (%s) hw=%u role=%u\n",
                  in.user.id[0] ? in.user.id : "?", in.user.longName, in.user.shortName,
                  (unsigned)in.user.hwModel, (unsigned)in.user.role);
}

void handleFrame(meshradio::Frame &f)
{
    Inbound in;
    const RxStatus st = parseFrame(f.data, f.len, gKey, gChannel, gSelf, in);

    gCounters.lastRssi = f.rssi;
    gCounters.lastSnr = f.snr;

    switch (st) {
        case RxStatus::Ok:
            break;
        case RxStatus::NotForUs:
            gCounters.notForUs++;
            return;
        case RxStatus::Crypto:
        case RxStatus::BadChannel:
            // Something is on our frequency with our sync word that is not our
            // channel. Worth printing: on a band this empty it is the only
            // evidence that the receiver hears anything at all.
            gCounters.badProto++;
            printRawFrame("foreign", f, "decrypts to no valid protobuf");
            return;
        case RxStatus::TooShort:
            gCounters.shortFrames++;
            printRawFrame("short", f, "shorter than a Meshtastic header");
            return;
        case RxStatus::Unknown:
            break;
    }

    gCounters.rxFrames++;
    gCounters.lastRxS = uptimeS();
    printFrame("rx", f.data, f.len, &f);

    if (in.header.from == gSelf) return;      // our own frame, echoed or relayed

    gNodes.heard(in.header.from, (int8_t)f.snr, in.header.hopsAway(), uptimeS());

    switch (in.portnum) {
        case PORT_TEXT:     onText(in, f); break;
        case PORT_NODEINFO: onNodeInfo(in); break;
        case PORT_POSITION:
            parsePosition(in.payload, in.payloadLen, in.position);
            if (in.position.valid)
                Serial.printf("[mesh] position: %.5f,%.5f alt %ld m\n",
                              in.position.latI / 1e7, in.position.lonI / 1e7,
                              (long)in.position.altitudeM);
            gCounters.rxOther++;
            break;
        default:
            gCounters.rxOther++;
            break;
    }
}

// Meshtastic ids are (uptime << 22) | counter. Ours is the same shape so a
// receiver's dedupe window behaves, and it stays monotonic within a session -
// which is what makes the phone's "since" filter work.
uint32_t nextPacketId()
{
    if (gPacketId == 0) gPacketId = (uptimeS() & 0x3FFUL) << 22;
    gPacketId++;
    return gPacketId;
}

}  // namespace

// ------------------------------------------------------------------ lifecycle
bool begin(uint32_t selfNum, const char *longName, const char *shortName)
{
    gSelf = selfNum;
    copyStr(gLongName, sizeof(gLongName), longName, kLongNameN);
    copyStr(gShortName, sizeof(gShortName), shortName, kShortNameN);

    // Derived, not hardcoded: these are the numbers the region math produces.
    regionDefs[0].freq = regionFrequencyBR902();
    regionDefs[1].freq = regionFrequencyUS();

    prefs.begin("meshnvs", false);
    gRegion = prefs.getInt("region", 0);
    if (gRegion < 0 || gRegion >= kRegionCount) gRegion = 0;
    const uint8_t pskIndex = (uint8_t)prefs.getInt("psk", 1);
    gEnabled = prefs.getBool("enabled", false);

    resolveKey(pskIndex, gKey);
    gChannel = channelHashFor("LongFast", gKey, 16);   // the primary channel hash

    gNodes.setSelf(gSelf, gLongName, gShortName);

    Serial.printf("[mesh] node !%08lx  \"%s\" (%s)  region %s  %.3f MHz  channel hash 0x%02x "
                  " psk alias %u\n",
                  (unsigned long)gSelf, gLongName, gShortName, regionName(),
                  (double)regionDefs[gRegion].freq, gChannel, (unsigned)pskIndex);

    gTcxoVolts = prefs.getFloat("tcxo", kDefaultTcxoVolts);
    if (gTcxoVolts < 1.4f || gTcxoVolts > 3.4f) gTcxoVolts = kDefaultTcxoVolts;

    // A frequency set by hand with "F <MHz>" wins over the region default, so the
    // setting survives a reboot the way the command implies. Recorded by
    // setFrequency(), which is the only place that may write it.
    float bootFreq = regionDefs[gRegion].freq;
    const float custom = prefs.getFloat("freq", 0.0f);
    if (custom > 860.0f && custom < 930.0f) {
        bootFreq = custom;
        Serial.printf("[mesh] using the frequency set by hand: %.3f MHz "
                      "(region default is %.3f)\n",
                      (double)bootFreq, (double)regionDefs[gRegion].freq);
    }

    const int16_t st = meshradio::begin(bootFreq, gTcxoVolts);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("[mesh] radio bring-up failed (%d); mesh stays off\n", (int)st);
        gEnabled = false;
        return false;
    }
    if (!gEnabled) {
        meshradio::sleep();
        Serial.println("[mesh] mesh is disabled in settings: radio asleep");
    }

    announce();
    return true;
}

void loop()
{
    // One-second tick, owned here so that the countdown is a fact rather than a
    // difference nobody looks at.
    const uint32_t nowMs = millis();
    if (nowMs - gLastTickMs >= 1000) {
        const uint32_t elapsedS = (nowMs - gLastTickMs) / 1000;
        gSinceAnnounceS += elapsedS;
        gLastTickMs += elapsedS * 1000;
    }

    // Static, not on the stack: the Arduino loop task has a modest stack and a
    // 1 KB frame buffer per pass is a silly way to spend it.
    static meshradio::Frame frames[4];
    const size_t n = meshradio::poll(frames, 4);
    for (size_t i = 0; i < n; i++) handleFrame(frames[i]);

    // The radio's own error counters are the truth about the physical layer; the
    // service counters above only count what parsed.
    const meshradio::Stats &rs = meshradio::stats();
    gCounters.crcErrors = rs.rxErrors;
    gCounters.txFail = rs.txFail;

    gLoopPasses++;

    // Prefer the RTC's wall clock; fall back to the counted seconds only before
    // the clock has ever been set.
    bool due = false;
    if (gWallUtc > 1700000000UL) {
        if (gLastAnnounceWall == 0) gLastAnnounceWall = gWallUtc;   // first sight of a clock
        due = (gWallUtc - gLastAnnounceWall) >= kAnnounceEveryS;
    } else {
        due = gSinceAnnounceS >= kAnnounceEveryS;
    }
    if (gEnabled && due) announce();
}

void setWallClock(uint32_t utcSeconds)
{
    gWallUtc = utcSeconds;
    if (gLastAnnounceWall == 0 && gWallUtc > 1700000000UL) gLastAnnounceWall = gWallUtc;
}

bool enabled() { return gEnabled; }

void setEnabled(bool on)
{
    if (on == gEnabled) return;
    gEnabled = on;
    prefs.putBool("enabled", on);
    if (on) {
        meshradio::retune(regionDefs[gRegion].freq);
        meshradio::startReceive();
        announce();
    } else {
        meshradio::sleep();
    }
    Serial.printf("[mesh] mesh %s\n", on ? "enabled" : "disabled (radio asleep)");
}

int         regionIndex() { return gRegion; }
const char *regionName()  { return regionDefs[gRegion].name; }
float       frequency()   { return meshradio::frequency(); }

bool setRegion(int index)
{
    if (index < 0 || index >= kRegionCount || index == gRegion) return false;
    gRegion = index;
    prefs.putInt("region", gRegion);
    prefs.remove("freq");      // the region's own default takes over again
    const int16_t st = meshradio::retune(regionDefs[gRegion].freq);
    Serial.printf("[mesh] region -> %s (%.3f MHz), retune %d\n", regionName(),
                  (double)regionDefs[gRegion].freq, (int)st);
    return st == RADIOLIB_ERR_NONE;
}

bool setTcxoVoltage(float volts)
{
    if (volts < 1.4f || volts > 3.4f) {
        Serial.printf("[mesh] %.1f V is not a TCXO voltage this chip accepts "
                      "(1.6/1.7/1.8/2.2/2.4/2.7/3.0/3.3)\n", (double)volts);
        return false;
    }
    if (meshradio::setTcxo(volts) != RADIOLIB_ERR_NONE) return false;
    gTcxoVolts = volts;
    prefs.putFloat("tcxo", volts);
    return true;
}

float tcxoVoltage() { return gTcxoVolts; }

bool takeMessageFlag()
{
    const bool f = gMessageFlag;
    gMessageFlag = false;
    return f;
}

bool setFrequency(float mhz)
{
    if (mhz < 860.0f || mhz > 930.0f) {
        Serial.printf("[mesh] %.3f MHz is outside 860-930; refusing\n", (double)mhz);
        return false;
    }
    const int16_t st = meshradio::retune(mhz);
    if (st != RADIOLIB_ERR_NONE) return false;
    prefs.putFloat("freq", mhz);      // remembered across a reboot
    return true;
}

uint32_t selfNum() { return gSelf; }
uint8_t  channelHash() { return gChannel; }
const NodeDb  &nodes() { return gNodes; }
const Mailbox &inbox() { return gInbox; }
const Counters &counters() { return gCounters; }

// ------------------------------------------------------------------- sending
bool sendText(const char *text, uint32_t to)
{
    if (!meshradio::ready()) return false;
    if (!text || !*text) return false;
    if (!gEnabled) {
        Serial.println("[mesh] refused: mesh is disabled");
        return false;
    }

    Packet p;
    if (!buildText(p, gSelf, nextPacketId(), text, to, gKey, gChannel)) {
        Serial.println("[mesh] refused: message does not fit a LoRa frame");
        return false;
    }

    uint8_t frame[kMaxFrame];
    if (!p.writeFrame(frame, sizeof(frame))) return false;

    printFrame("tx", frame, p.frameLen(), nullptr);
    if (!meshradio::queueTx(frame, p.frameLen())) {
        Serial.println("[mesh] refused: transmit queue is full");
        return false;
    }

    gInbox.push(text, gSelf, to, p.header.id, 0, 0, true, uptimeS());
    gCounters.tx++;
    gMessageFlag = true;
    return true;
}

void announce()
{
    if (!meshradio::ready() || !gEnabled) return;

    Packet p;
    if (!buildNodeInfo(p, gSelf, nextPacketId(), gLongName, gShortName, kHwModelWatchS3,
                       gKey, gChannel)) {
        return;
    }
    uint8_t frame[kMaxFrame];
    if (!p.writeFrame(frame, sizeof(frame))) return;

    printFrame("tx", frame, p.frameLen(), nullptr);
    meshradio::queueTx(frame, p.frameLen());
    gCounters.tx++;
    gSinceAnnounceS = 0;
    gLastAnnounceWall = gWallUtc;
    Serial.printf("[mesh] announced node info as \"%s\" (%s) - next in %us\n",
                  gLongName, gShortName, (unsigned)kAnnounceEveryS);
}

// ------------------------------------------------------------------ reporting
size_t statusText(char *out, size_t n)
{
    size_t o = 0;
    const Counters &c = gCounters;
    const meshradio::Stats &rs = meshradio::stats();

    append(out, n, o, "[mesh] node !%08lx \"%s\" (%s)\n", (unsigned long)gSelf, gLongName, gShortName);
    append(out, n, o, "[mesh] %s  region %s  %.3f MHz  channel 0x%02x  radio %s  "
                      "announce in %lus\n",
           gEnabled ? "enabled" : "disabled", regionName(), (double)frequency(), gChannel,
           meshradio::ready() ? (meshradio::receiving() ? "rx" : "idle") : "down",
           (unsigned long)(gSinceAnnounceS < kAnnounceEveryS
                               ? kAnnounceEveryS - gSinceAnnounceS : 0));
    if (gWallUtc > 1700000000UL) {
        append(out, n, o, "[mesh] rtc %lu  next announce in %lds (wall clock)\n",
               (unsigned long)gWallUtc,
               (long)((int32_t)(kAnnounceEveryS - (gWallUtc - gLastAnnounceWall))));
    }
    append(out, n, o, "[mesh] rx=%lu (text %lu, nodeinfo %lu, other %lu)  notForUs=%lu  "
                      "badProto=%lu  short=%lu  crcErr=%lu\n",
           (unsigned long)c.rxFrames, (unsigned long)c.rxText, (unsigned long)c.rxNodeInfo,
           (unsigned long)c.rxOther, (unsigned long)c.notForUs, (unsigned long)c.badProto,
           (unsigned long)c.shortFrames, (unsigned long)c.crcErrors);
    append(out, n, o, "[mesh] tcxo %.1f V  tx=%lu (queued ok)  txFail=%lu  radioFrames=%lu  "
                      "nodes=%lu\n",
           (double)gTcxoVolts,
           (unsigned long)c.tx, (unsigned long)c.txFail, (unsigned long)rs.rxFrames,
           (unsigned long)gNodes.count());
    append(out, n, o, "[mesh] last rx rssi=%d dBm snr=%.2f dB  %lus ago  inbox=%lu\n",
           (int)c.lastRssi, (double)c.lastSnr,
           (unsigned long)(c.lastRxS ? uptimeS() - c.lastRxS : 0),
           (unsigned long)gInbox.size());
    return o;
}

size_t nodesText(char *out, size_t n)
{
    size_t o = 0;
    append(out, n, o, "[mesh] %lu node(s) heard (self %s):\n",
           (unsigned long)gNodes.count(), gShortName);
    Node list[kMaxNodes];
    const size_t cnt = gNodes.snapshot(list, kMaxNodes);
    if (!cnt) append(out, n, o, "[mesh]   (nobody yet)\n");
    for (size_t i = 0; i < cnt; i++) {
        char id[16], name[48];
        NodeDb::hexId(list[i].num, id, sizeof(id));
        NodeDb::displayName(list[i], name, sizeof(name));
        append(out, n, o, "[mesh]   %-10s %-16s snr %4d dB  %u hop%s  hw=%u  last %lus ago\n",
               id, name, (int)list[i].snr, list[i].hops, list[i].hops == 1 ? "" : "s",
               (unsigned)list[i].hwModel,
               (unsigned long)(uptimeS() > list[i].lastHeardS ? uptimeS() - list[i].lastHeardS : 0));
    }
    return o;
}

size_t inboxText(char *out, size_t n)
{
    size_t o = 0;
    append(out, n, o, "[mesh] %lu message(s) held:\n", (unsigned long)gInbox.size());
    for (size_t i = 0; i < gInbox.size(); i++) {
        const Msg *m = gInbox.at(i);
        if (!m) continue;
        char who[48];
        if (m->outgoing) {
            snprintf(who, sizeof(who), "me");
        } else {
            const Node *nd = gNodes.find(m->from);
            if (nd) NodeDb::displayName(*nd, who, sizeof(who));
            else    NodeDb::hexId(m->from, who, sizeof(who));
        }
        append(out, n, o, "[mesh]   #%lu %-10s %s\n", (unsigned long)m->id,
               m->outgoing ? "->" : who, m->text);
    }
    return o;
}

bool radioSelfTest(char *out, size_t n)
{
    size_t o = 0;
    if (!meshradio::ready()) {
        append(out, n, o, "[mesh] radio is not up; nothing to test\n");
        return false;
    }

    int32_t rssi = 0;
    const int16_t st = meshradio::noiseFloor(rssi);

    append(out, n, o, "[mesh] self-test: noise floor %ld dBm (startReceive state %d)\n",
           (long)rssi, (int)st);
    append(out, n, o, "[mesh]   a working front end reads a plausible noise floor "
                      "(roughly -85..-125 dBm, depending on the band and how noisy the "
                      "board itself is); a flat -128.0 or an error means the RX chain or "
                      "the TCXO/calibration is not right\n");
    append(out, n, o, "[mesh]   sync 0x%02x  BW %.1f kHz  SF%u  CR4/%u  preamble %u  freq %.3f MHz\n",
           kSyncWord, (double)kBandwidthKHz, kSpreadingFactor, kCodingRate, kPreambleLength,
           (double)frequency());

    // A transmit that reports success is the other half of "the radio works".
    Packet p;
    uint8_t frame[kMaxFrame];
    buildText(p, gSelf, nextPacketId(), "selftest", kBroadcast, gKey, gChannel);
    p.writeFrame(frame, sizeof(frame));
    const int16_t tx = meshradio::transmitNow(frame, p.frameLen());
    append(out, n, o, "[mesh]   test transmit: %s (%d), TxDone without error means the PA "
                      "and the SPI link are alive\n",
           (tx == RADIOLIB_ERR_NONE) ? "ok" : "FAILED", (int)tx);
    return true;
}

size_t debugText(char *out, size_t n)
{
    size_t o = 0;
    append(out, n, o, "[mesh] debug: uptime=%lus  sinceAnnounce=%lu/%us  enabled=%d  "
                      "ready=%d  receiving=%d  freq=%.3f  channel=0x%02x  "
                      "loopStackFreeWords=%u  heap=%lu\n",
           (unsigned long)uptimeS(), (unsigned long)gSinceAnnounceS,
           (unsigned)kAnnounceEveryS, (int)gEnabled, (int)meshradio::ready(),
           (int)meshradio::receiving(), (double)frequency(), (unsigned)gChannel,
           (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned long)ESP.getFreeHeap());
    append(out, n, o, "[mesh] debug: tick last at %lums, millis()=%lums, tcxo=%.1f V\n",
           (unsigned long)gLastTickMs, (unsigned long)millis(), (double)gTcxoVolts);
    append(out, n, o, "[mesh] debug: loopPasses=%lu  wallUtc=%lu  lastAnnounceWall=%lu\n",
           (unsigned long)gLoopPasses, (unsigned long)gWallUtc,
           (unsigned long)gLastAnnounceWall);
    return o;
}

// ---------------------------------------------------------------------- JSON
size_t jsonStatus(char *out, size_t n)
{
    size_t o = 0;
    const Counters &c = gCounters;
    append(out, n, o,
           "{\"node\":\"!%08lx\",\"long\":\"%s\",\"short\":\"%s\",\"region\":\"%s\","
           "\"freq\":%.3f,\"channel\":%u,\"enabled\":%s,\"radio\":%s,"
           "\"rx\":%lu,\"rxText\":%lu,\"rxNodeInfo\":%lu,\"notForUs\":%lu,\"badProto\":%lu,"
           "\"tx\":%lu,\"txFail\":%lu,\"crcErr\":%lu,\"shortFrames\":%lu,\"tcxo\":%.1f,\"nodes\":%lu,\"msgs\":%lu,"
           "\"rssi\":%d,\"snr\":%.2f,\"lastRxAgoS\":%lu,\"uptime\":%lu}",
           (unsigned long)gSelf, gLongName, gShortName, regionName(),
           (double)frequency(), (unsigned)gChannel, gEnabled ? "true" : "false",
           meshradio::ready() ? "true" : "false",
           (unsigned long)c.rxFrames, (unsigned long)c.rxText, (unsigned long)c.rxNodeInfo,
           (unsigned long)c.notForUs, (unsigned long)c.badProto, (unsigned long)c.tx,
           (unsigned long)c.txFail, (unsigned long)c.crcErrors,
           (unsigned long)c.shortFrames, (double)gTcxoVolts,
           (unsigned long)gNodes.count(), (unsigned long)gInbox.size(),
           (int)c.lastRssi, (double)c.lastSnr,
           (unsigned long)(c.lastRxS ? uptimeS() - c.lastRxS : 0), (unsigned long)uptimeS());
    return o;
}

size_t jsonNodes(char *out, size_t n)
{
    size_t o = 0;
    append(out, n, o, "{\"self\":\"!%08lx\",\"nodes\":[", (unsigned long)gSelf);

    Node list[kMaxNodes];
    const size_t cnt = gNodes.snapshot(list, kMaxNodes);
    for (size_t i = 0; i < cnt; i++) {
        char id[16];
        NodeDb::hexId(list[i].num, id, sizeof(id));
        char longName[kLongNameN], shortName[kShortNameN];
        copyStr(longName, sizeof(longName), list[i].longName, sizeof(list[i].longName));
        copyStr(shortName, sizeof(shortName), list[i].shortName, sizeof(list[i].shortName));
        jsonEscape(longName);
        jsonEscape(shortName);
        append(out, n, o,
               "%s{\"id\":\"%s\",\"long\":\"%s\",\"short\":\"%s\",\"snr\":%d,\"hops\":%u,"
               "\"hw\":%u,\"heardAgoS\":%lu}",
               i ? "," : "", id, longName, shortName, (int)list[i].snr,
               (unsigned)list[i].hops, (unsigned)list[i].hwModel,
               (unsigned long)(uptimeS() > list[i].lastHeardS ? uptimeS() - list[i].lastHeardS : 0));
    }
    append(out, n, o, "]}");
    return o;
}

size_t jsonInbox(char *out, size_t n, uint32_t sinceId)
{
    size_t o = 0;
    append(out, n, o, "{\"msgs\":[");
    bool first = true;
    for (size_t i = 0; i < gInbox.size(); i++) {
        const Msg *m = gInbox.at(i);
        if (!m) continue;
        if (sinceId && m->id <= sinceId) continue;

        char fromName[48];
        if (m->outgoing) {
            snprintf(fromName, sizeof(fromName), "me");
        } else {
            const Node *nd = gNodes.find(m->from);
            if (nd) NodeDb::displayName(*nd, fromName, sizeof(fromName));
            else    NodeDb::hexId(m->from, fromName, sizeof(fromName));
        }
        char text[kMsgTextN];
        copyStr(text, sizeof(text), m->text, sizeof(m->text));
        jsonEscape(text);

        append(out, n, o,
               "%s{\"id\":%lu,\"seq\":%lu,\"from\":\"!%08lx\",\"fromName\":\"%s\","
               "\"out\":%s,\"text\":\"%s\",\"snr\":%d,\"hops\":%u,\"at\":%lu}",
               first ? "" : ",", (unsigned long)m->id, (unsigned long)i,
               (unsigned long)m->from, fromName, m->outgoing ? "true" : "false",
               text, (int)m->snr, (unsigned)m->hops, (unsigned long)m->atS);
        first = false;
    }
    append(out, n, o, "],\"last\":%lu,\"count\":%lu}",
           (unsigned long)gInbox.lastId(), (unsigned long)gInbox.size());
    return o;
}

size_t urlDecode(char *dst, size_t dstN, const char *src)
{
    if (!dst || dstN == 0) return 0;
    size_t w = 0;
    if (src) {
        for (size_t i = 0; src[i] && w + 1 < dstN; i++) {
            if (src[i] == '%' && src[i + 1] && src[i + 2]) {
                auto nib = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int hi = nib(src[i + 1]), lo = nib(src[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    dst[w++] = (char)((hi << 4) | lo);
                    i += 2;
                    continue;
                }
            }
            dst[w++] = src[i] == '+' ? ' ' : src[i];
        }
    }
    dst[w] = '\0';
    return w;
}

}  // namespace meshsvc

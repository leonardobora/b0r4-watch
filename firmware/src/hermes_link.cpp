/*
 * hermes_link.cpp — see hermes_link.h.
 *
 * Design choices worth stating, because each one is a wearable constraint:
 *  - WiFi is ON DEMAND, not always-on. A watch that holds an association open is
 *    burning its 470 mAh to sit idle; we connect when asked and drop the link
 *    after `kIdleDisconnectMs` of quiet.
 *  - One request at a time, on a fixed PSRAM buffer (max ~8 s of 16 kHz mono).
 *    No queues: an utterance that cannot be sent now is not worth sending late.
 *  - The reply is fetched as text/plain (bridge `?plain=1`) so there is no JSON
 *    parser in the firmware, and the whole reply buffer is screen-sized.
 */

#include "hermes_link.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "LilyGoLib.h"

namespace hermeslink {

using State = State;

namespace {

constexpr uint32_t kSampleRate   = 16000;
constexpr int      kChannels     = 1;
constexpr int      kBits         = 16;
constexpr size_t   kMaxSamples   = kSampleRate * 8;              // 8 s ceiling
constexpr size_t   kChunkSamples = kSampleRate / 10;             // 100 ms per loop pass
constexpr uint32_t kWifiJoinMs   = 15000;
constexpr uint32_t kRequestMs    = 90000;                        // agent turns are slow
constexpr uint32_t kConnectMs    = 8000;                         // TCP connect: short, retried once
constexpr uint32_t kIdleDiscMs   = 15000;                        // 15s burst window -> WiFi_OFF to save battery
constexpr size_t   kReplyBytes   = 1400;                         // ~ what the screen can hold

State     s_state = State::Off;
char      s_ssid[33] = {0};
char      s_psk[65] = {0};
char      s_url[96] = {0};
char      s_host[64] = {0};
uint16_t  s_port = 8799;
char      s_path[48] = "/ask?plain=1";
char      s_reply[kReplyBytes] = {0};
char      s_transcript[192] = {0};
int16_t  *s_pcm = nullptr;
size_t    s_samples = 0;
uint32_t  s_recStartMs = 0;
uint32_t  s_lastUseMs = 0;
uint32_t  s_lastRtt = 0;
int       s_httpStatus = 0;
uint32_t  s_lastActivityMs = 0;
int16_t  *s_stage = nullptr;                 // one chunk, all mic channels
int       s_micChannels = 1;
int       s_micPick = 0;
uint32_t  s_lastPeak = 0;                    // level of the last take, for the record
uint32_t  s_lastRms = 0;
bool      s_wifiTried = false;
bool      s_viaAp = false;
// The outbound queue: at most one utterance, because a wearable that hoards
// questions is worse than one that says "ask again".
bool      s_pendingAudio = false;
size_t    s_pendingSamples = 0;
char      s_pendingText[192] = {0};

void parseUrl(const char *url);   // loadSettings() reads a stored url before this exists

// ---------------------------------------------------------------- settings ----

void loadSettings()
{
    Preferences prefs;
    prefs.begin("hermes", true);
    String ssid = prefs.getString("ssid", "");
    String psk  = prefs.getString("psk", "");
    String url  = prefs.getString("url", "");
    prefs.end();
    ssid.toCharArray(s_ssid, sizeof(s_ssid));
    psk.toCharArray(s_psk, sizeof(s_psk));
    if (url.length()) url.toCharArray(s_url, sizeof(s_url));
    else              strncpy(s_url, "http://192.168.18.10:8799/ask?plain=1", sizeof(s_url) - 1);
    parseUrl(s_url);
}

void parseUrl(const char *url)
{
    // http://host:port/path  (no TLS: this is a LAN hop, and the ESP32 has no
    // business holding a cert store for it)
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *slash = strchr(p, '/');
    char hostport[80] = {0};
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= sizeof(hostport)) n = sizeof(hostport) - 1;
        memcpy(hostport, p, n);
        strncpy(s_path, slash, sizeof(s_path) - 1);
    } else {
        strncpy(hostport, p, sizeof(hostport) - 1);
    }
    char *colon = strchr(hostport, ':');
    if (colon) {
        *colon = 0;
        s_port = (uint16_t)atoi(colon + 1);
    }
    strncpy(s_host, hostport, sizeof(s_host) - 1);
}

void saveSettings()
{
    Preferences prefs;
    prefs.begin("hermes", false);
    prefs.putString("ssid", s_ssid);
    prefs.putString("psk", s_psk);
    prefs.putString("url", s_url);
    prefs.end();
}

// -------------------------------------------------------------------- wifi ----

bool ensureWifi()
{
    s_viaAp = false;
    if (!s_ssid[0]) {
        // No station credentials, but the phone page's access point may be up — and
        // that AP is a perfectly good path to the bridge, because the PC joins it.
        // Treating "no SSID configured" as "no network" was wrong and cost a
        // debugging round: the watch refused to send while it *was* on a network.
        if ((WiFi.getMode() & WIFI_MODE_AP) && (uint32_t)WiFi.softAPIP() != 0) {
            s_viaAp = true;
            s_state = State::Idle;
            return true;
        }
        s_state = State::Off;
        return false;
    }
    if (WiFi.status() == WL_CONNECTED) return true;

    s_state = State::WifiJoining;
    // Only keep AP enabled if the phone web page explicitly turned it on; otherwise STA-only burst.
    WiFi.mode((WiFi.getMode() & WIFI_MODE_AP) ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(s_ssid, s_psk);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kWifiJoinMs) {
        delay(120);
        vTaskDelay(1);
    }
    if (WiFi.status() != WL_CONNECTED) {
        s_state = State::Error;
        strncpy(s_reply, "wifi: cannot join", sizeof(s_reply) - 1);
        return false;
    }
    s_state = State::Idle;
    return true;
}

void disconnectIfIdle()
{
    if (s_viaAp) return;          // the AP is not ours to drop; the phone page owns it
    if (!s_ssid[0]) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (s_state == State::Recording || s_state == State::Sending || s_state == State::WifiJoining) return;
    if (millis() - s_lastUseMs < kIdleDiscMs) return;
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
}

// ---------------------------------------------------------------- http POST ----

// Percent-encodes into the destination so a spoken/typed phrase can ride a query
// string without inventing an escaping bug on the device.
void urlEncode(const char *src, char *dst, size_t n)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = src; *p && o + 4 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else if (c == ' ') {
            dst[o++] = '%'; dst[o++] = '2'; dst[o++] = '0';
        } else {
            dst[o++] = '%'; dst[o++] = hex[c >> 4]; dst[o++] = hex[c & 0xF];
        }
    }
    dst[o] = 0;
}

// Sends the request, waits for the agent, and keeps the body. `body` may be null
// (the text path puts everything in the query string).
bool postAndReceive(const char *path, const uint8_t *body, size_t bodyLen)
{
    if (!ensureWifi()) return false;
    if (!s_host[0]) {
        s_state = State::Error;
        strncpy(s_reply, "bridge url not set", sizeof(s_reply) - 1);
        return false;
    }

    s_state = State::Sending;
    s_lastUseMs = millis();

    WiFiClient client;
    client.setTimeout(kRequestMs / 1000);
    // A short connect with one retry. Waiting the full agent timeout on a connect
    // that will never succeed leaves the face saying "thinking" for minutes when
    // the truthful answer is "there is no bridge" - and the common cause is simply
    // that the PC has not finished associating with the watch's AP yet.
    uint32_t connT0 = millis();
    client.setNoDelay(true);
    bool connected = client.connect(s_host, s_port, kConnectMs);
    Serial.printf("[hermes] connect %s:%u -> %s in %lums\n", s_host, s_port,
                  connected ? "ok" : "failed", (unsigned long)(millis() - connT0));
    if (!connected) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        connected = client.connect(s_host, s_port, kConnectMs);
    }
    if (!connected) {
        s_state = State::Error;
        snprintf(s_reply, sizeof(s_reply), "no bridge at %s:%u - is the PC on the AP?",
                 s_host, s_port);
        return false;
    }

    // One write instead of a chain of printfs: fewer chances for a short write to
    // hide, and Nagle disabled so the request cannot sit in a buffer.
    char req[256];
    int reqLen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/octet-stream\r\n"
        "X-Sample-Rate: %lu\r\nX-Channels: %d\r\nContent-Length: %u\r\n"
        "Connection: close\r\n\r\n",
        path, s_host, s_port, (unsigned long)kSampleRate, kChannels, (unsigned)bodyLen);
    if (reqLen > 0) {
        size_t wrote = client.write((const uint8_t *)req, (size_t)reqLen);
        Serial.printf("[hermes] wrote %u/%d request bytes to %s:%u (%s)\n",
                      (unsigned)wrote, reqLen, s_host, s_port, client.connected() ? "still open" : "closed");
        if (body && bodyLen) {
            size_t bw = client.write(body, bodyLen);
            Serial.printf("[hermes] wrote %u/%u audio bytes\n", (unsigned)bw, (unsigned)bodyLen);
        }
    } else {
        Serial.println("[hermes] request line did not fit its buffer");
    }
    client.flush();

    uint32_t t0 = millis();
    bool headersDone = false;
    char hdr[8] = {0};
    size_t replyLen = 0;
    s_reply[0] = 0;
    s_httpStatus = 0;
    char statusLine[48] = {0};
    size_t statusLen = 0;

    while (client.connected() || client.available()) {
        if (millis() - t0 > kRequestMs) break;
        while (client.available()) {
            char c = (char)client.read();
            if (!headersDone) {
                // Track the status line and the end of the headers.
                if (statusLen < sizeof(statusLine) - 1 && replyLen == 0) statusLine[statusLen++] = c;
                memmove(hdr, hdr + 1, 7);
                hdr[7] = c;
                if (memcmp(hdr + 4, "\r\n\r\n", 4) == 0) {
                    headersDone = true;
                    s_httpStatus = atoi(statusLine + 9);   // "HTTP/1.1 200 OK"
                    Serial.printf("[hermes] response: %.48s\n", statusLine);
                    s_reply[0] = 0;
                }
                continue;
            }
            if (replyLen < sizeof(s_reply) - 1) s_reply[replyLen++] = c;
        }
        vTaskDelay(1);
    }
    s_reply[replyLen] = 0;
    s_lastRtt = millis() - t0;
    Serial.printf("[hermes] read %u body bytes in %lums%s\n", (unsigned)replyLen,
                  (unsigned long)s_lastRtt, headersDone ? "" : " (no header terminator seen)");
    client.stop();

    if (!headersDone) {
        s_state = State::Error;
        strncpy(s_reply, "bridge: no answer", sizeof(s_reply) - 1);
        return false;
    }
    // A chunked body would arrive with hex lengths inline; the bridge always sends
    // Content-Length, so keep a cheap guard rather than a full decoder.
    if (strncmp(s_reply, "HTTP/", 5) == 0) {
        s_state = State::Error;
        strncpy(s_reply, "bridge: bad framing", sizeof(s_reply) - 1);
        return false;
    }
    while (replyLen && (s_reply[replyLen - 1] == '\n' || s_reply[replyLen - 1] == '\r')) {
        s_reply[--replyLen] = 0;
    }
    {
        Preferences prefs;
        prefs.begin("hermes", false);
        prefs.putString("lastreply", s_reply);
        prefs.putString("lastask", s_transcript);
        prefs.end();
    }
    if (s_httpStatus != 200 && !s_reply[0]) {
        s_state = State::Error;
        snprintf(s_reply, sizeof(s_reply), "bridge http %d", s_httpStatus);
        return false;
    }
    s_state = State::Reply;
    return true;
}

}  // namespace

// ------------------------------------------------------------------- public ----

bool begin()
{
    loadSettings();
    s_pcm = (int16_t *)ps_malloc(kMaxSamples * sizeof(int16_t));
    if (!s_pcm) s_pcm = (int16_t *)malloc(kMaxSamples * sizeof(int16_t));
    if (!s_pcm) return false;
    s_stage = (int16_t *)ps_malloc(kChunkSamples * 4 * sizeof(int16_t));
    if (!s_stage) s_stage = (int16_t *)malloc(kChunkSamples * 4 * sizeof(int16_t));
    {
        Preferences prefs;
        prefs.begin("hermes", true);
        String lastUp = prefs.getString("lastreply", "");
        String lastAsk = prefs.getString("lastask", "");
        prefs.end();
        lastUp.toCharArray(s_reply, sizeof(s_reply));
        lastAsk.toCharArray(s_transcript, sizeof(s_transcript));
    }
    if (s_reply[0]) s_state = State::Reply;
    else            s_state = s_ssid[0] ? State::Idle : State::Off;
    return true;
}

void loop()
{
    if (s_state == State::Recording) {
        AudioInputIf *mic = instance.getAudioInput();
        if (!mic) {
            s_state = State::Error;
            strncpy(s_reply, "no microphone", sizeof(s_reply) - 1);
            return;
        }
        if (s_samples >= kMaxSamples) {           // hit the ceiling: send what we have
            stopRecordingAndSend();
            return;
        }
        size_t want = kChunkSamples;
        if (s_samples + want > kMaxSamples) want = kMaxSamples - s_samples;
        if (s_micChannels <= 1 || !s_stage) {
            int got = mic->read((uint8_t *)(s_pcm + s_samples), want * sizeof(int16_t));
            if (got > 0) s_samples += (size_t)got / sizeof(int16_t);
            return;
        }
        // Interleaved multi-channel chunk: keep the channel with the most life in it.
        size_t wantAll = want * (size_t)s_micChannels;
        int got = mic->read((uint8_t *)s_stage, wantAll * sizeof(int16_t));
        if (got <= 0) return;
        size_t frames = (size_t)got / sizeof(int16_t) / (size_t)s_micChannels;
        if (frames == 0) return;
        if (s_samples == 0) {                     // first chunk decides the channel
            uint64_t best = 0;
            for (int c = 0; c < s_micChannels; c++) {
                uint64_t sum = 0;
                for (size_t f = 0; f < frames; f++) {
                    int16_t v = s_stage[f * s_micChannels + c];
                    sum += (uint64_t)(v < 0 ? -v : v);
                }
                if (sum > best) { best = sum; s_micPick = c; }
            }
            Serial.printf("[hermes] mic: %d channel(s), using channel %d\n", s_micChannels, s_micPick);
        }
        for (size_t f = 0; f < frames && s_samples < kMaxSamples; f++)
            s_pcm[s_samples++] = s_stage[f * s_micChannels + s_micPick];
        return;
    }
    if (millis() - s_lastActivityMs > 1000) {
        s_lastActivityMs = millis();
        disconnectIfIdle();
    }
}

bool startRecording()
{
    if (s_state == State::Sending) return false;
    AudioInputIf *mic = instance.getAudioInput();
    if (!mic) {
        s_state = State::Error;
        strncpy(s_reply, "no microphone", sizeof(s_reply) - 1);
        return false;
    }
    // This board's I2S stream can carry more than one slot, and reading the wrong
    // one yields a flat DC line that looks like "the microphone works" while
    // containing nothing - which is exactly what a first attempt produced. So open
    // every channel the board declares and pick the live one per take.
    int boardCh = instance.getCodecInputChannels();
    if (boardCh < 1 || boardCh > 4) boardCh = 1;
    if (!mic->open(kBits, boardCh, kSampleRate)) {
        instance.initMicrophone();                 // may not have been needed at boot
        if (!mic->open(kBits, boardCh, kSampleRate)) {
            s_state = State::Error;
            strncpy(s_reply, "mic open failed", sizeof(s_reply) - 1);
            return false;
        }
    }
    s_micChannels = boardCh;
    s_micPick = 0;
    s_samples = 0;
    s_recStartMs = millis();
    s_state = State::Recording;
    s_reply[0] = 0;
    return true;
}

bool stopRecordingAndSend()
{
    if (s_state != State::Recording) return false;
    AudioInputIf *mic = instance.getAudioInput();
    if (mic) mic->close();

    if (s_samples < kSampleRate / 4) {             // under 250 ms: nothing to say
        s_state = State::Idle;
        strncpy(s_reply, "(too short)", sizeof(s_reply) - 1);
        return false;
    }
    uint64_t peak = 0, sumsq = 0;
    for (size_t i = 0; i < s_samples; i++) {
        int32_t v = s_pcm[i];
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > peak) peak = a;
        sumsq += (uint64_t)((int64_t)v * v);
    }
    s_lastPeak = (uint32_t)peak;
    s_lastRms = s_samples ? (uint32_t)sqrt((double)sumsq / (double)s_samples) : 0;
    Serial.printf("[hermes] take: %u samples, peak=%u rms=%u (%s)\n",
                  (unsigned)s_samples, (unsigned)s_lastPeak, (unsigned)s_lastRms,
                  s_lastPeak < 300 ? "silent" : (s_lastPeak - s_lastRms < 50 ? "flat/DC" : "has signal"));
    snprintf(s_transcript, sizeof(s_transcript), "%us of audio",
             (unsigned)(s_samples / kSampleRate));
    // Park it for the PC to collect (see hermes_link.h).
    s_pendingAudio = true;
    s_pendingSamples = s_samples;
    s_pendingText[0] = 0;
    s_state = State::Idle;
    return true;
}

void queueText(const char *text)
{
    if (!text || !*text) return;
    strncpy(s_pendingText, text, sizeof(s_pendingText) - 1);
    s_pendingText[sizeof(s_pendingText) - 1] = 0;
    strncpy(s_transcript, text, sizeof(s_transcript) - 1);
    s_pendingAudio = false;
    s_pendingSamples = 0;
    s_state = State::Idle;
}

bool hasPending() { return s_pendingAudio || s_pendingText[0]; }
const char *pendingKind() { return s_pendingAudio ? "AUDIO" : "TEXT"; }

const uint8_t *pendingBytes(size_t *len)
{
    if (len) *len = s_pendingAudio ? s_pendingSamples * sizeof(int16_t) : 0;
    return (const uint8_t *)s_pcm;
}

const char *pendingText() { return s_pendingText; }

void clearPending()
{
    s_pendingAudio = false;
    s_pendingSamples = 0;
    s_pendingText[0] = 0;
}

void setReplyFromPeer(const char *text)
{
    if (!text) return;
    strncpy(s_reply, text, sizeof(s_reply) - 1);
    s_reply[sizeof(s_reply) - 1] = 0;
    s_state = State::Reply;
    s_lastUseMs = millis();
    Preferences prefs;
    prefs.begin("hermes", false);
    prefs.putString("lastreply", s_reply);
    prefs.putString("lastask", s_transcript);
    prefs.end();
}

bool sendText(const char *text)
{
    if (!text || !*text) return false;
    queueText(text);
    return true;
}

State state() { return s_state; }

const char *stateText()
{
    switch (s_state) {
        case State::Off:         return s_ssid[0] ? "idle" : "no link";
        case State::Idle:        return hasPending() ? "queued for PC" : "ready";
        case State::WifiJoining: return "joining wifi";
        case State::Recording:   return "listening";
        case State::Sending:     return "thinking";
        case State::Reply:       return "replied";
        case State::Error:       return "error";
    }
    return "?";
}

const char *lastReply() { return s_reply; }
const char *lastTranscript() { return s_transcript; }
uint32_t recordedSeconds() { return s_samples / kSampleRate; }
uint32_t lastRoundTripMs() { return s_lastRtt; }
int lastHttpStatus() { return s_httpStatus; }

bool configureWifi(const char *ssid, const char *psk)
{
    if (!ssid || !*ssid) return false;
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = 0;
    strncpy(s_psk, psk ? psk : "", sizeof(s_psk) - 1);
    s_psk[sizeof(s_psk) - 1] = 0;
    saveSettings();
    WiFi.disconnect(false, false);
    s_wifiTried = false;
    s_state = State::Idle;
    return true;
}

bool setBridgeUrl(const char *url)
{
    if (!url || !*url) return false;
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = 0;
    parseUrl(s_url);
    saveSettings();
    return true;
}

const char *wifiSsid() { return s_ssid; }
const char *bridgeUrl() { return s_url; }
bool wifiConfigured() { return s_ssid[0] != 0; }
bool wifiConnected() { return (WiFi.status() == WL_CONNECTED) || s_viaAp; }
int wifiRssi() { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }

size_t statusText(char *out, size_t n)
{
    return snprintf(out, n,
        "[hermes] state=%s  wifi=%s%s%s  rssi=%d  bridge=%s:%u%s\n"
        "[hermes] last: asked=%.60s  %ums  http=%d\n"
        "[hermes] reply: %.140s",
        stateText(),
        s_viaAp ? "(own AP)" : (s_ssid[0] ? s_ssid : "-"),
        wifiConnected() ? " (up " : " (down",
        wifiConnected() ? ")" : ")",
        wifiRssi(), s_host, s_port, s_path,
        s_transcript[0] ? s_transcript : "-", s_lastRtt, s_httpStatus,
        s_reply[0] ? s_reply : "-");
}

}  // namespace hermeslink

/*
 * hermes_link.h — the wrist half of "ask Hermes".
 *
 * The watch holds a microphone and a 240px screen; the agent holds the brains and
 * lives on the PC. This module is the seam: it joins WiFi, captures a few seconds
 * of audio (or takes a typed/canned prompt), POSTs it to the bridge that
 * transcribes and asks the agent, and keeps the reply for the UI to draw.
 *
 * Nothing here is clever on purpose: one request at a time, a fixed buffer, and a
 * state the screen can always show.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hermeslink {

enum class State {
    Off,          // no WiFi credentials configured
    Idle,         // ready, nothing in flight
    WifiJoining,
    Recording,
    Sending,
    Reply,        // a reply is on screen
    Error,
};

// Loads settings (SSID/PSK/bridge URL), joins WiFi if configured. Safe to call
// even with nothing configured: the face then says so instead of pretending.
bool begin();

// Call every main-loop pass. Non-blocking except while a request is in flight,
// which is bounded by `timeoutMs`.
void loop();

// --- actions, all initiated by the face or a serial command ---
bool startRecording();          // begin capturing from the mic
bool stopRecordingAndSend();    // stop, POST the audio, wait for the reply
bool sendText(const char *text);  // the no-microphone path (canned prompt / serial)

// --- the queue, and why it exists ---
// This device does NOT dial out. It records, parks the audio (or the text) in a
// small queue, and the PC comes and collects it. Two reasons, both learned the
// hard way: a wearable should not hold a socket open on a 470 mAh battery, and on
// a locked-down PC the inbound port a dial-out would need is dropped by the
// firewall (ufw DEFAULT_INPUT_POLICY=DROP) with no way to open it from here.
// The PC learns the address from the access point; the watch just answers.
void queueText(const char *text);
bool hasPending();
const char *pendingKind();                 // "AUDIO" or "TEXT"
const uint8_t *pendingBytes(size_t *len);  // valid while hasPending()
const char *pendingText();
void clearPending();
void setReplyFromPeer(const char *text);   // the PC delivering the agent's answer

// --- state for the UI ---
State state();
const char *stateText();            // one short line, screen-sized
const char *lastReply();            // the agent's last answer ("" if none)
const char *lastTranscript();       // what we asked (for the record)
uint32_t recordedSeconds();
uint32_t lastRoundTripMs();
int      lastHttpStatus();

// --- configuration (serial, or the phone page later) ---
bool configureWifi(const char *ssid, const char *psk);
bool setBridgeUrl(const char *url);
const char *wifiSsid();
const char *bridgeUrl();
bool wifiConfigured();
bool wifiConnected();
int  wifiRssi();

// A serial-facing dump: state, wifi, url, last exchange, timings.
size_t statusText(char *out, size_t n);

}  // namespace hermeslink

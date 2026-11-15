/*
 * ble_companion.h — Low-power NimBLE companion link (S21 Gadgetbridge + Omarchy).
 *
 * Exposes Nordic UART Service (6E400001-B5A3-F393-E0A9-E50E24DCCA9E) advertising
 * as "Bangle.js oma" with multi-central support (PC + S21 simultaneously).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace blecomp {

struct ThemePayload {
    char     name[24];
    uint32_t bg, surface, text, muted, accent;
};

struct PeerEntry {
    char     addr[20];
    char     label[20];
    bool     connected;
    bool     bonded;
    uint16_t connHandle;
};

void begin(bool enabledByDefault = true);
void loop();
void setEnabled(bool on);
void reAdvertise();
bool enabled();
bool connected();
bool appleConnected();
int  peerCount();
int  listPeers(PeerEntry *out, int maxCount);
bool removePeer(const char *addr);

// Poll flags from the main loop (thread-safe; cleared on take).
bool takeBuzzFlag();
bool takeUiDirty();
bool takeClockSync(time_t *utcOut, int *tzOffsetMinOut, bool *hasUtc, bool *hasTz);
bool takeThemeSync(ThemePayload *out);

// Companion state for Watch Face, S21 Inbox & Dedicated Media Player.
uint32_t    unreadCount();
void        clearUnread();
void        latestLine(char *out, size_t n);
void        inboxSummary(char *out, size_t n);
bool        latestNotif(char *src, size_t sn, char *title, size_t tn,
                        char *body, size_t bn, long *id, bool *canReply);
bool        sendNotifReply(long id, const char *msg);
bool        sendNotifDismiss(long id);
const char *musicTrack();
const char *musicArtist();
const char *musicAlbum();
bool        musicPlaying();
bool        musicShuffle();
int         musicRepeat();  // 0=off, 1=all, 2=one
void        mediaSnapshot(char *track, size_t tn, char *artist, size_t an,
                          char *album, size_t aln, bool *playing);
void        setMediaLocal(bool playing, const char *artist, const char *track, const char *album);
int         queueCount();
const char *queueItem(int idx);
void        setQueueLocal(const char *pipeList);
bool        toggleShuffle();
bool        toggleRepeat();
bool        sendQueuePlay(int idx);

// Turn-by-turn navigation & GPS speed telemetry (from Gadgetbridge "t":"nav" / "t":"gps").
bool        navActive();
void        navSnapshot(char *action, size_t acn, char *dist, size_t dn,
                        char *instr, size_t in, char *eta, size_t en, bool *active);
void        setNavLocal(bool active, const char *action, const char *dist,
                        const char *instr, const char *eta);
float       gpsSpeedKmh();
bool        gpsValid();
void        setGpsSpeedLocal(float kmh);
bool        callActive();
const char *callName();

// Actions Watch -> Phone/PC over BLE NUS TX:
// cmd: "play", "pause", "next", "previous", "volumeup", "volumedown"
bool sendMusicCmd(const char *cmd);
bool sendFindPhone(bool on);
bool sendOmaAction(const char *act);

size_t statusText(char *out, size_t n);

}  // namespace blecomp

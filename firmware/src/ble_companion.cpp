#include "ble_companion.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <string.h>

namespace blecomp {
namespace {

constexpr const char *kDeviceName = "Bangle.js oma";
constexpr const char *kNusSvcUuid = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *kNusRxUuid  = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr const char *kNusTxUuid  = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// Generic Access Service (GAP) for reading connected peer's real device name
constexpr const char *kGapSvcUuid  = "1800";
constexpr const char *kGapNameUuid = "2A00";

// Apple Media Service (AMS) & Apple Notification Center Service (ANCS) for iPhone
constexpr const char *kAmsSvcUuid          = "89D3502B-0F36-433A-8EF4-C502AD55F8DC";
constexpr const char *kAmsRemoteUuid       = "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2";
constexpr const char *kAmsEntityUpdateUuid = "2F7CABCE-808D-411F-9A0C-BB92BA96C102";

constexpr const char *kAncsSvcUuid         = "7905F431-B5CE-4E99-A40F-4B1E122D00D0";
constexpr const char *kAncsNotifUuid       = "9FBF120D-6301-42D9-8C58-25E699A21DBD";
constexpr const char *kAncsCtrlUuid        = "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9";
constexpr const char *kAncsDataUuid        = "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB";

struct NotifItem {
    long id;
    bool canReply;
    char src[20];
    char title[36];
    char body[80];
};

portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

bool                   gInited      = false;
bool                   gEnabled     = true;
bool                   gConnected   = false;
int                    gPeerCount   = 0;
bool                   gBuzzPending = false;
bool                   gUiDirty     = false;

bool                   gHasUtc      = false;
time_t                 gSyncUtc     = 0;
bool                   gHasTz       = false;
int                    gSyncTzMin   = 0;

bool                   gHasTheme    = false;
ThemePayload           gTheme       = {};

NotifItem              gInbox[4]    = {};
int                    gInboxCount  = 0;
uint32_t               gUnread      = 0;
uint32_t               gRxLines     = 0;

char                   gArtist[56]  = {0};
char                   gTrack[80]   = {0};
char                   gAlbum[56]   = {0};
bool                   gPlaying     = false;
bool                   gShuffle     = false;
int                    gRepeat      = 0;  // 0=off, 1=all, 2=one
uint32_t               gTrackDurS   = 0;
uint32_t               gTrackPosS   = 0;
uint32_t               gTrackPosAtMs = 0;
char                   gQueue[6][48] = {{0}};
int                    gQueueCount  = 0;

bool                   gWeatherOk   = false;
int                    gWeatherC    = 0;
char                   gWeatherCond[12] = {0};

bool                   gNavActive   = false;
char                   gNavAction[24] = {0};
char                   gNavDist[24]   = {0};
char                   gNavInstr[96]  = {0};
char                   gNavEta[20]    = {0};
float                  gGpsSpeedKmh = 0.0f;
bool                   gGpsValid    = false;
bool                   gCallActive  = false;
char                   gCallName[48] = {0};

char                   gRxLine[512] = {0};
size_t                 gRxLen       = 0;

NimBLEServer               *gServer          = nullptr;
NimBLECharacteristic       *gTxChar          = nullptr;

// Apple iOS (AMS + ANCS) client state over the same peripheral link
bool                        gAppleConnected  = false;
uint16_t                    gAppleHandle     = BLE_HS_CONN_HANDLE_NONE;
uint16_t                    gProbeHandle     = BLE_HS_CONN_HANDLE_NONE;
bool                        gProbePending    = false;
uint32_t                    gProbeAtMs       = 0;
NimBLERemoteCharacteristic *gAmsRemoteChar   = nullptr;
NimBLERemoteCharacteristic *gAncsCtrlChar    = nullptr;
bool                        gAncsFetchPend   = false;
uint8_t                     gAncsFetchUid[4] = {0};
uint8_t                     gAncsBuf[160]    = {0};
size_t                      gAncsBufLen      = 0;

void startAdv();
void sanitizePtBrAscii(char *s);

void onAmsEntityUpdate(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
    if (!data || len < 3) return;
    uint8_t entity = data[0], attr = data[1];
    char val[80] = {0};
    size_t vlen = len - 3;
    if (vlen >= sizeof(val)) vlen = sizeof(val) - 1;
    memcpy(val, data + 3, vlen);
    val[vlen] = 0;

    if (entity == 0 && attr == 1) {
        // Player PlaybackInfo: "<state>,<rate>,<elapsed>" where state '1' = Playing
        uint32_t elap = 0;
        const char *c1 = strchr(val, ',');
        const char *c2 = c1 ? strchr(c1 + 1, ',') : nullptr;
        if (c2) elap = (uint32_t)atof(c2 + 1);
        portENTER_CRITICAL(&gMux);
        gPlaying = (val[0] == '1');
        if (c2) { gTrackPosS = elap; gTrackPosAtMs = millis(); }
        gUiDirty = true;
        portEXIT_CRITICAL(&gMux);
    } else if (entity == 0 && attr == 2) {
        // Player ShuffleMode: "0"=Off, "1"/"2"=On
        portENTER_CRITICAL(&gMux);
        gShuffle = (val[0] != '0' && val[0] != 0);
        gUiDirty = true;
        portEXIT_CRITICAL(&gMux);
    } else if (entity == 0 && attr == 3) {
        // Player RepeatMode: "0"=Off, "1"=One, "2"=All
        portENTER_CRITICAL(&gMux);
        gRepeat = (val[0] == '1') ? 2 : (val[0] == '2' ? 1 : 0);
        gUiDirty = true;
        portEXIT_CRITICAL(&gMux);
    } else if (entity == 2) {
        // Track: 0=Artist, 1=Album, 2=Title, 3=Duration
        sanitizePtBrAscii(val);
        portENTER_CRITICAL(&gMux);
        if (attr == 0) {
            strncpy(gArtist, val, sizeof(gArtist) - 1);
            gArtist[sizeof(gArtist) - 1] = 0;
        } else if (attr == 1) {
            strncpy(gAlbum, val, sizeof(gAlbum) - 1);
            gAlbum[sizeof(gAlbum) - 1] = 0;
        } else if (attr == 2) {
            if (strcmp(gTrack, val) != 0) { gTrackPosS = 0; gTrackPosAtMs = millis(); }
            strncpy(gTrack, val, sizeof(gTrack) - 1);
            gTrack[sizeof(gTrack) - 1] = 0;
        } else if (attr == 3) {
            gTrackDurS = (uint32_t)atof(val);
        }
        gUiDirty = true;
        portEXIT_CRITICAL(&gMux);
    }
}

void onAncsNotif(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
    if (!data || len < 8) return;
    uint8_t evId = data[0];
    uint8_t catId = data[2];
    if (evId == 0 || evId == 1) {
        // Added (0) or Modified (1) — Google Maps updates the same turn notification with evId==1!
        if (evId == 0 && catId == 1) {
            portENTER_CRITICAL(&gMux);
            gCallActive = true;
            strncpy(gCallName, "iPhone Call", sizeof(gCallName) - 1);
            gCallName[sizeof(gCallName) - 1] = 0;
            gBuzzPending = true;
            gUiDirty = true;
            portEXIT_CRITICAL(&gMux);
        }
        memcpy(gAncsFetchUid, data + 4, 4);
        gAncsFetchPend = true;
    } else if (evId == 2 && catId == 1) {
        portENTER_CRITICAL(&gMux);
        gCallActive = false;
        gCallName[0] = 0;
        gUiDirty = true;
        portEXIT_CRITICAL(&gMux);
    }
}

// Transliterate UTF-8 PT-BR / Latin-1 accents to clean ASCII for LVGL Montserrat fonts.
void sanitizePtBrAscii(char *s)
{
    if (!s) return;
    char *r = s, *w = s;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        if (c >= 0x20 && c <= 0x7E) {
            *w++ = (char)c;
            r++;
        } else if (c == 0xC3 && r[1]) {
            unsigned char c2 = (unsigned char)r[1];
            char rep = 0;
            if      (c2 >= 0x80 && c2 <= 0x85) rep = 'A';
            else if (c2 == 0x87)               rep = 'C';
            else if (c2 >= 0x88 && c2 <= 0x8B) rep = 'E';
            else if (c2 >= 0x8C && c2 <= 0x8F) rep = 'I';
            else if (c2 == 0x91)               rep = 'N';
            else if (c2 >= 0x92 && c2 <= 0x96) rep = 'O';
            else if (c2 >= 0x99 && c2 <= 0x9C) rep = 'U';
            else if (c2 >= 0xA0 && c2 <= 0xA5) rep = 'a';
            else if (c2 == 0xA7)               rep = 'c';
            else if (c2 >= 0xA8 && c2 <= 0xAB) rep = 'e';
            else if (c2 >= 0xAC && c2 <= 0xAF) rep = 'i';
            else if (c2 == 0xB1)               rep = 'n';
            else if (c2 >= 0xB2 && c2 <= 0xB6) rep = 'o';
            else if (c2 >= 0xB9 && c2 <= 0xBC) rep = 'u';
            if (rep) *w++ = rep;
            r += 2;
        } else if (c == 0xC2 && r[1]) {
            unsigned char c2 = (unsigned char)r[1];
            if (c2 == 0xA0)      *w++ = ' ';
            else if (c2 == 0xBA) *w++ = 'o';
            else if (c2 == 0xAA) *w++ = 'a';
            else if (c2 == 0xB0) *w++ = 'o';
            else if (c2 == 0xAB || c2 == 0xBB) *w++ = '"';
            r += 2;
        } else if ((c == 0xCC || c == 0xCD) && r[1]) {
            // Combining diacritical mark (NFD): drop mark, keep base ASCII letter
            r += 2;
        } else if (c == 0xE2 && r[1] && r[2]) {
            if ((unsigned char)r[1] == 0x80) {
                unsigned char c3 = (unsigned char)r[2];
                if (c3 == 0x93 || c3 == 0x94 || c3 == 0xA2) *w++ = '-';
                else if (c3 == 0x98 || c3 == 0x99) *w++ = '\'';
                else if (c3 == 0x9C || c3 == 0x9D) *w++ = '"';
                else if (c3 == 0xA6) { *w++ = '.'; *w++ = '.'; *w++ = '.'; }
            }
            r += 3;
        } else if (c >= 0xF0 && r[1] && r[2] && r[3]) {
            r += 4;
        } else if (c >= 0xE0 && r[1] && r[2]) {
            r += 3;
        } else if (c >= 0xC0 && r[1]) {
            r += 2;
        } else {
            r++;
        }
    }
    *w = 0;
}

void shortenNavInstr(char *s, size_t maxLen)
{
    if (!s) return;
    static const char *kDrop[] = {
        "Vire a direita na ", "Vire a esquerda na ", "Vire a direita ", "Vire a esquerda ",
        "Curva acentuada a direita ", "Curva acentuada a esquerda ",
        "Mantenha-se a direita ", "Mantenha-se a esquerda ",
        "Siga em frente na ", "Siga em frente ", "Continue na ", "Continue em ",
        "Turn right onto ", "Turn left onto ", "Turn right ", "Turn left ",
        "Keep right ", "Keep left ", "Continue onto ", "Continue on ",
    };
    for (const char *pfx : kDrop) {
        size_t plen = strlen(pfx);
        if (strncasecmp(s, pfx, plen) == 0 && s[plen]) {
            memmove(s, s + plen, strlen(s + plen) + 1);
            break;
        }
    }
    if (strlen(s) > maxLen) {
        s[maxLen - 3] = '.';
        s[maxLen - 2] = '.';
        s[maxLen - 1] = '.';
        s[maxLen] = 0;
    }
}

int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

char mapUnicodeCpToAscii(uint16_t cp)
{
    if (cp >= 0x20 && cp <= 0x7E) return (char)cp;
    if (cp == 0x00A0) return ' ';
    if (cp == 0x00AA) return 'a';
    if (cp == 0x00BA || cp == 0x00B0) return 'o';
    if (cp >= 0x00C0 && cp <= 0x00C5) return 'A';
    if (cp == 0x00C7) return 'C';
    if (cp >= 0x00C8 && cp <= 0x00CB) return 'E';
    if (cp >= 0x00CC && cp <= 0x00CF) return 'I';
    if (cp == 0x00D1) return 'N';
    if (cp >= 0x00D2 && cp <= 0x00D6) return 'O';
    if (cp >= 0x00D9 && cp <= 0x00DC) return 'U';
    if (cp >= 0x00E0 && cp <= 0x00E5) return 'a';
    if (cp == 0x00E7) return 'c';
    if (cp >= 0x00E8 && cp <= 0x00EB) return 'e';
    if (cp >= 0x00EC && cp <= 0x00EF) return 'i';
    if (cp == 0x00F1) return 'n';
    if (cp >= 0x00F2 && cp <= 0x00F6) return 'o';
    if (cp >= 0x00F9 && cp <= 0x00FC) return 'u';
    if (cp == 0x2013 || cp == 0x2014 || cp == 0x2022) return '-';
    if (cp == 0x2018 || cp == 0x2019) return '\'';
    if (cp == 0x201C || cp == 0x201D) return '"';
    return 0;
}

// Minimal zero-alloc JSON string extractor for Gadgetbridge flat payloads.
bool extractJsonStr(const char *json, const char *key, char *out, size_t cap)
{
    if (!cap) return false;
    out[0] = 0;
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n' || *p == 'r' || *p == 't') { out[o++] = ' '; p++; continue; }
            if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                int h0 = hexVal(p[1]), h1 = hexVal(p[2]), h2 = hexVal(p[3]), h3 = hexVal(p[4]);
                if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                    uint16_t cp = (uint16_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    if (cp == 0x2026 && o + 3 < cap) {
                        out[o++] = '.'; out[o++] = '.'; out[o++] = '.';
                    } else {
                        char rep = mapUnicodeCpToAscii(cp);
                        if (rep) out[o++] = rep;
                    }
                    p += 5;
                    continue;
                }
            }
            out[o++] = *p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    return o > 0;
}

void pushNotifLocked(const char *src, const char *title, const char *body, long id = 0, bool canReply = false)
{
    for (int i = 3; i > 0; i--) gInbox[i] = gInbox[i - 1];
    gInbox[0].id       = id;
    gInbox[0].canReply = canReply;
    strncpy(gInbox[0].src,   src ? src : "Phone", sizeof(gInbox[0].src) - 1);
    strncpy(gInbox[0].title, title ? title : "",  sizeof(gInbox[0].title) - 1);
    strncpy(gInbox[0].body,  body ? body : "",    sizeof(gInbox[0].body) - 1);
    gInbox[0].src[sizeof(gInbox[0].src) - 1]     = 0;
    gInbox[0].title[sizeof(gInbox[0].title) - 1] = 0;
    gInbox[0].body[sizeof(gInbox[0].body) - 1]   = 0;
    if (gInboxCount < 4) gInboxCount++;
    gUnread++;
    gBuzzPending = true;
    gUiDirty = true;
}

bool parseThemeSpec(const char *spec, ThemePayload &tp)
{
    char buf[128] = {0};
    strncpy(buf, spec, sizeof(buf) - 1);
    char *parts[6] = {0};
    int n = 0;
    char *tok = strtok(buf, "|");
    while (tok && n < 6) {
        parts[n++] = tok;
        tok = strtok(nullptr, "|");
    }
    if (n != 6) return false;
    memset(&tp, 0, sizeof(tp));
    strncpy(tp.name, parts[0], sizeof(tp.name) - 1);
    tp.bg      = (uint32_t)strtoul(parts[1], nullptr, 16);
    tp.surface = (uint32_t)strtoul(parts[2], nullptr, 16);
    tp.text    = (uint32_t)strtoul(parts[3], nullptr, 16);
    tp.muted   = (uint32_t)strtoul(parts[4], nullptr, 16);
    tp.accent  = (uint32_t)strtoul(parts[5], nullptr, 16);
    return true;
}

void handleLine(const char *raw)
{
    while (*raw && ((unsigned char)*raw <= 0x20)) raw++;
    if (!*raw) return;

    // 1. Espruino / Gadgetbridge clock sync: setTime(1794603000);E.setTimeZone(-3)
    const char *st = strstr(raw, "setTime(");
    if (st) {
        long long sec = strtoll(st + 8, nullptr, 10);
        if (sec > 1700000000LL) {
            portENTER_CRITICAL(&gMux);
            gSyncUtc = (time_t)sec;
            gHasUtc  = true;
            gUiDirty = true;
            portEXIT_CRITICAL(&gMux);
        }
    }
    const char *tz = strstr(raw, "setTimeZone(");
    if (tz) {
        float hrs = strtof(tz + 12, nullptr);
        if (hrs >= -14.0f && hrs <= 14.0f) {
            portENTER_CRITICAL(&gMux);
            gSyncTzMin = (int)(hrs * 60.0f);
            gHasTz     = true;
            gUiDirty   = true;
            portEXIT_CRITICAL(&gMux);
        }
    }

    // 2. Omarchy live theme push: TH Everforest|181D20|2D353B|D3C6AA|475258|7FBBB3
    if (strncmp(raw, "TH ", 3) == 0) {
        ThemePayload tp;
        if (parseThemeSpec(raw + 3, tp)) {
            portENTER_CRITICAL(&gMux);
            gTheme    = tp;
            gHasTheme = true;
            gUiDirty  = true;
            portEXIT_CRITICAL(&gMux);
        }
        return;
    }

    if (strncmp(raw, "NAME ", 5) == 0 && raw[5]) {
        char nm[20] = {0};
        strncpy(nm, raw + 5, sizeof(nm) - 1);
        sanitizePtBrAscii(nm);
        if (nm[0] && gServer) {
            std::vector<uint16_t> handles = gServer->getPeerDevices();
            for (uint16_t h : handles) {
                if (gAppleConnected && h == gAppleHandle) continue;
                NimBLEConnInfo info = gServer->getPeerInfoByHandle(h);
                std::string addr = info.getIdAddress().toString();
                Preferences p;
                p.begin("ble_names", false);
                p.putString(addr.c_str(), nm);
                p.end();
                portENTER_CRITICAL(&gMux);
                gUiDirty = true;
                portEXIT_CRITICAL(&gMux);
                break;
            }
        }
        return;
    }

    // 3. Gadgetbridge JSON envelope: GB({...})
    const char *gb = strstr(raw, "GB({");
    const char *json = gb ? (gb + 3) : (raw[0] == '{' ? raw : nullptr);
    if (json) {
        char type[20] = {0};
        extractJsonStr(json, "t", type, sizeof(type));
        if (strcmp(type, "notify") == 0) {
            char src[20], title[36], body[80];
            extractJsonStr(json, "src",   src,   sizeof(src));
            extractJsonStr(json, "title", title, sizeof(title));
            extractJsonStr(json, "body",  body,  sizeof(body));
            long nid = 0;
            const char *idp = strstr(json, "\"id\":");
            if (idp) nid = atol(idp + 5);
            bool canReply = (strstr(json, "\"reply\":true") != nullptr);
            sanitizePtBrAscii(src);
            sanitizePtBrAscii(title);
            sanitizePtBrAscii(body);
            portENTER_CRITICAL(&gMux);
            pushNotifLocked(src[0] ? src : "S21", title, body, nid, canReply);
            portEXIT_CRITICAL(&gMux);
        } else if (strcmp(type, "notify-") == 0) {
            const char *idp = strstr(json, "\"id\":");
            long nid = idp ? atol(idp + 5) : 0;
            portENTER_CRITICAL(&gMux);
            if (gInboxCount > 0 && (nid == 0 || gInbox[0].id == nid)) {
                if (gUnread > 0) gUnread--;
                gUiDirty = true;
            }
            portEXIT_CRITICAL(&gMux);
        } else if (strcmp(type, "call") == 0) {
            char cmd[20] = {0}, name[48] = {0};
            extractJsonStr(json, "cmd",  cmd,  sizeof(cmd));
            extractJsonStr(json, "name", name, sizeof(name));
            sanitizePtBrAscii(name);
            if (strcmp(cmd, "incoming") == 0 || strcmp(cmd, "start") == 0) {
                portENTER_CRITICAL(&gMux);
                pushNotifLocked("Call", name[0] ? name : "Incoming", cmd);
                gCallActive = true;
                strncpy(gCallName, name[0] ? name : "Incoming", sizeof(gCallName) - 1);
                gCallName[sizeof(gCallName) - 1] = 0;
                gBuzzPending = true;
                gUiDirty = true;
                portEXIT_CRITICAL(&gMux);
            } else if (strcmp(cmd, "end") == 0 || strcmp(cmd, "idle") == 0 || strcmp(cmd, "hangup") == 0) {
                portENTER_CRITICAL(&gMux);
                gCallActive = false;
                gCallName[0] = 0;
                gUiDirty = true;
                portEXIT_CRITICAL(&gMux);
            }
        } else if (strcmp(type, "weather") == 0) {
            int temp = 0;
            const char *tp = strstr(json, "\"temp\":");
            if (tp) {
                temp = atoi(tp + 7);
                if (temp > 150) temp -= 273;  // Gadgetbridge sends Kelvin (e.g. 295)
            }
            char txt[32] = {0};
            extractJsonStr(json, "txt", txt, sizeof(txt));
            String w(txt);
            w.toLowerCase();
            const char *shortC = "CLR";
            if (w.indexOf("thunder") >= 0 || w.indexOf("storm") >= 0) shortC = "STRM";
            else if (w.indexOf("rain") >= 0 || w.indexOf("drizzle") >= 0 || w.indexOf("shower") >= 0) shortC = "RAIN";
            else if (w.indexOf("fog") >= 0 || w.indexOf("mist") >= 0) shortC = "FOG";
            else if (w.indexOf("snow") >= 0) shortC = "SNOW";
            else if (w.indexOf("cloud") >= 0 || w.indexOf("overcast") >= 0) shortC = "CLD";
            else if (w.indexOf("sun") >= 0 || w.indexOf("clear") >= 0) shortC = "SUN";
            portENTER_CRITICAL(&gMux);
            gWeatherOk = (tp != nullptr);
            gWeatherC  = temp;
            strncpy(gWeatherCond, shortC, sizeof(gWeatherCond) - 1);
            gWeatherCond[sizeof(gWeatherCond) - 1] = 0;
            gUiDirty = true;
            portEXIT_CRITICAL(&gMux);
        } else if (strcmp(type, "weather") == 0) {
            int temp = 0;
            const char *tp = strstr(json, "\"temp\":");
            if (tp) {
                temp = atoi(tp + 7);
                if (temp > 150) temp -= 273;  // Gadgetbridge sends Kelvin (e.g. 295)
            }
            char txt[32] = {0};
            extractJsonStr(json, "txt", txt, sizeof(txt));
            String w(txt);
            w.toLowerCase();
            const char *shortC = "CLR";
            if (w.indexOf("thunder") >= 0 || w.indexOf("storm") >= 0) shortC = "STRM";
            else if (w.indexOf("rain") >= 0 || w.indexOf("drizzle") >= 0 || w.indexOf("shower") >= 0) shortC = "RAIN";
            else if (w.indexOf("fog") >= 0 || w.indexOf("mist") >= 0) shortC = "FOG";
            else if (w.indexOf("snow") >= 0) shortC = "SNOW";
            else if (w.indexOf("cloud") >= 0 || w.indexOf("overcast") >= 0) shortC = "CLD";
            else if (w.indexOf("sun") >= 0 || w.indexOf("clear") >= 0) shortC = "SUN";
            portENTER_CRITICAL(&gMux);
            gWeatherOk = (tp != nullptr);
            gWeatherC  = temp;
            strncpy(gWeatherCond, shortC, sizeof(gWeatherCond) - 1);
            gWeatherCond[sizeof(gWeatherCond) - 1] = 0;
            gUiDirty = true;
            portEXIT_CRITICAL(&gMux);
        } else if (strcmp(type, "musicinfo") == 0) {
            char artist[56], track[80], album[56];
            extractJsonStr(json, "artist", artist, sizeof(artist));
            extractJsonStr(json, "track",  track,  sizeof(track));
            extractJsonStr(json, "album",  album,  sizeof(album));
            const char *dp = strstr(json, "\"dur\":");
            uint32_t dur = dp ? (uint32_t)atol(dp + 6) : 0;
            sanitizePtBrAscii(artist);
            sanitizePtBrAscii(track);
            sanitizePtBrAscii(album);
            portENTER_CRITICAL(&gMux);
            if (strcmp(gTrack, track) != 0) { gTrackPosS = 0; gTrackPosAtMs = millis(); }
            if (dur > 0) gTrackDurS = dur;
            strncpy(gArtist, artist, sizeof(gArtist) - 1);
            strncpy(gTrack,  track,  sizeof(gTrack) - 1);
            strncpy(gAlbum,  album,  sizeof(album) - 1);
            gArtist[sizeof(gArtist) - 1] = 0;
            gTrack[sizeof(gTrack) - 1]   = 0;
            gAlbum[sizeof(gAlbum) - 1]   = 0;
            gUiDirty = true;
            portEXIT_CRITICAL(&gMux);
        } else if (strcmp(type, "musicstate") == 0) {
            char state[16] = {0}, repStr[16] = {0};
            extractJsonStr(json, "state", state, sizeof(state));
            bool hasShuf = (strstr(json, "\"shuffle\":true") != nullptr || strstr(json, "\"shuffle\":false") != nullptr);
            bool shufVal = (strstr(json, "\"shuffle\":true") != nullptr);
            bool hasRepStr = extractJsonStr(json, "repeat", repStr, sizeof(repStr));
            const char *repNum = hasRepStr ? nullptr : strstr(json, "\"repeat\":");
            const char *posP = strstr(json, "\"position\":");
            portENTER_CRITICAL(&gMux);
            if (state[0]) gPlaying = (strcmp(state, "play") == 0);
            if (posP) { gTrackPosS = (uint32_t)atol(posP + 11); gTrackPosAtMs = millis(); }
            if (hasShuf)  gShuffle = shufVal;
            if (hasRepStr) {
                if (strcmp(repStr, "one") == 0 || strcmp(repStr, "track") == 0) gRepeat = 2;
                else if (strcmp(repStr, "all") == 0 || strcmp(repStr, "playlist") == 0) gRepeat = 1;
                else gRepeat = 0;
            } else if (repNum) {
                int rv = atoi(repNum + 9);
                gRepeat = (rv == 2) ? 2 : (rv == 1 ? 1 : 0);
            }
            gUiDirty = true;
            portEXIT_CRITICAL(&gMux);
        } else if (strcmp(type, "queue") == 0) {
            const char *arr = strstr(json, "\"items\":[");
            if (arr) {
                arr += 9;
                portENTER_CRITICAL(&gMux);
                gQueueCount = 0;
                while (*arr && *arr != ']' && gQueueCount < 6) {
                    const char *q1 = strchr(arr, '"');
                    if (!q1) break;
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (!q2) break;
                    size_t len = (size_t)(q2 - q1);
                    if (len >= sizeof(gQueue[0])) len = sizeof(gQueue[0]) - 1;
                    memcpy(gQueue[gQueueCount], q1, len);
                    gQueue[gQueueCount][len] = 0;
                    sanitizePtBrAscii(gQueue[gQueueCount]);
                    gQueueCount++;
                    arr = q2 + 1;
                }
                gUiDirty = true;
                portEXIT_CRITICAL(&gMux);
            }
        } else if (strcmp(type, "nav") == 0) {
            char action[24] = {0}, dist[24] = {0}, instr[96] = {0}, eta[20] = {0};
            extractJsonStr(json, "action", action, sizeof(action));
            if (!extractJsonStr(json, "distance", dist, sizeof(dist))) {
                const char *dp = strstr(json, "\"distance\":");
                if (dp) {
                    long m = atol(dp + 11);
                    if (m >= 1000) snprintf(dist, sizeof(dist), "%.1f km", m / 1000.0);
                    else if (m > 0) snprintf(dist, sizeof(dist), "%ld m", m);
                }
            }
            extractJsonStr(json, "instr", instr, sizeof(instr));
            extractJsonStr(json, "eta",   eta,   sizeof(eta));
            sanitizePtBrAscii(action);
            sanitizePtBrAscii(dist);
            sanitizePtBrAscii(instr);
            sanitizePtBrAscii(eta);
            shortenNavInstr(instr, 54);
            bool active = (instr[0] != 0 || dist[0] != 0 || action[0] != 0);
            portENTER_CRITICAL(&gMux);
            bool changed = (strcmp(gNavInstr, instr) != 0 || strcmp(gNavAction, action) != 0);
            gNavActive = active;
            strncpy(gNavAction, action, sizeof(gNavAction) - 1); gNavAction[sizeof(gNavAction) - 1] = 0;
            strncpy(gNavDist,   dist,   sizeof(gNavDist) - 1);   gNavDist[sizeof(gNavDist) - 1]     = 0;
            strncpy(gNavInstr,  instr,  sizeof(gNavInstr) - 1);  gNavInstr[sizeof(gNavInstr) - 1]   = 0;
            strncpy(gNavEta,    eta,    sizeof(gNavEta) - 1);    gNavEta[sizeof(gNavEta) - 1]       = 0;
            if (active && changed) gBuzzPending = true;
            gUiDirty = true;
            portEXIT_CRITICAL(&gMux);
        } else if (strcmp(type, "gps") == 0) {
            const char *sp = strstr(json, "\"speed\":");
            if (sp) {
                float v = (float)atof(sp + 8);
                if (v < 0) v = 0;
                portENTER_CRITICAL(&gMux);
                gGpsSpeedKmh = v;
                gGpsValid    = true;
                gUiDirty     = true;
                portEXIT_CRITICAL(&gMux);
            }
        }
    }

    portENTER_CRITICAL(&gMux);
    gRxLines++;
    portEXIT_CRITICAL(&gMux);
}

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
    {
        // Latency 4 lets the radio skip empty polling intervals (~300ms) while keeping instant wake on RX
        pServer->updateConnParams(connInfo.getConnHandle(), 30, 60, 4, 500);
        int cnt = (int)pServer->getConnectedCount();
        portENTER_CRITICAL(&gMux);
        gPeerCount = cnt;
        gConnected = (cnt > 0);
        gUiDirty   = true;
        portEXIT_CRITICAL(&gMux);
        Serial.printf("[ble] peer connected (peers=%d)\n", cnt);
        NimBLEDevice::startSecurity(connInfo.getConnHandle());
        gProbeHandle  = connInfo.getConnHandle();
        gProbePending = true;
        gProbeAtMs    = millis() + 900;
        // Keep advertising for a 2nd central so BOTH PC and phone stay linked simultaneously!
        if (gEnabled && cnt < 2) {
            startAdv();
        }
    }

    void onAuthenticationComplete(NimBLEConnInfo &connInfo) override
    {
        if (connInfo.isEncrypted()) {
            gProbeHandle  = connInfo.getConnHandle();
            gProbePending = true;
            gProbeAtMs    = millis() + 200;
        }
    }

    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override
    {
        if (connInfo.getConnHandle() == gAppleHandle) {
            gAppleConnected = false;
            gAppleHandle    = BLE_HS_CONN_HANDLE_NONE;
            gAmsRemoteChar  = nullptr;
            gAncsCtrlChar   = nullptr;
        }
        int cnt = pServer ? (int)pServer->getConnectedCount() : 0;
        portENTER_CRITICAL(&gMux);
        gPeerCount = cnt;
        gConnected = (cnt > 0);
        gUiDirty   = true;
        portEXIT_CRITICAL(&gMux);
        Serial.printf("[ble] peer disconnected (reason=%d, peers=%d)\n", reason, cnt);
        if (gEnabled) startAdv();
    }
};

class RxCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override
    {
        std::string v = pChar->getValue();
        for (char c : v) {
            if (c == '\n' || c == '\r') {
                if (gRxLen > 0) {
                    gRxLine[gRxLen] = 0;
                    if (strncmp(gRxLine, "GB(", 3) == 0) {
                        std::string addr = connInfo.getIdAddress().toString();
                        Preferences p;
                        p.begin("ble_names", false);
                        if (!p.isKey(addr.c_str())) {
                            p.putString(addr.c_str(), "Galaxy S21");
                            portENTER_CRITICAL(&gMux);
                            gUiDirty = true;
                            portEXIT_CRITICAL(&gMux);
                        }
                        p.end();
                    }
                    handleLine(gRxLine);
                    gRxLen = 0;
                }
            } else if (gRxLen + 1 < sizeof(gRxLine)) {
                gRxLine[gRxLen++] = c;
            }
        }
    }
};

ServerCb gServerCb;
RxCb     gRxCb;

void startAdv()
{
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->stop();
    NimBLEAdvertisementData advData;
    advData.setFlags(0x06);
    advData.setName(kDeviceName);
    advData.setAppearance(0x00C1);  // Generic Sports Watch (primary ADV packet for iOS/Android)
    NimBLEAdvertisementData scanData;
    scanData.setCompleteServices(NimBLEUUID(kNusSvcUuid));
    adv->setAdvertisementData(advData);
    adv->setScanResponseData(scanData);
    adv->setMinInterval(240);
    adv->setMaxInterval(400);
    adv->start();
}

bool sendRawLine(const char *jsonLine)
{
    if (!gInited || !gEnabled || !gConnected || !gTxChar || !jsonLine) return false;
    size_t len = strlen(jsonLine);
    // Send in <=120B chunks; Gadgetbridge BangleJS parser requires CRLF (\r\n) line ending!
    for (size_t off = 0; off < len; off += 120) {
        size_t chunk = (len - off > 120) ? 120 : (len - off);
        gTxChar->setValue((const uint8_t *)(jsonLine + off), chunk);
        if (!gTxChar->notify()) return false;
    }
    return true;
}

}  // namespace

void begin(bool enabledByDefault)
{
    Preferences p;
    p.begin("blecomp", true);
    gEnabled = p.getBool("on", enabledByDefault);
    p.end();

    if (!gEnabled) {
        Serial.println("[ble] disabled in settings (0 mA)");
        return;
    }

    if (!gInited) {
        NimBLEDevice::init(kDeviceName);
        NimBLEDevice::setPower(-3);
        NimBLEDevice::setMTU(185);
        NimBLEDevice::setSecurityAuth(true, false, true);  // bonding + Secure Connections
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
        gServer = NimBLEDevice::createServer();
        gServer->setCallbacks(&gServerCb);

        NimBLEService *svc = gServer->createService(kNusSvcUuid);
        gTxChar = svc->createCharacteristic(kNusTxUuid, NIMBLE_PROPERTY::NOTIFY);
        NimBLECharacteristic *rxChar = svc->createCharacteristic(
            kNusRxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
        rxChar->setCallbacks(&gRxCb);
        svc->start();
        gInited = true;
    }

    startAdv();
    Serial.printf("[ble] advertising as '%s' (NUS dual-peer, -3 dBm)\n", kDeviceName);
}

void setEnabled(bool on)
{
    if (on == gEnabled && (gInited || !on)) return;
    gEnabled = on;
    Preferences p;
    p.begin("blecomp", false);
    p.putBool("on", on);
    p.end();

    if (on) {
        if (!gInited) {
            begin(true);
            return;
        }
        startAdv();
        Serial.println("[ble] enabled -> advertising");
    } else if (gInited) {
        NimBLEDevice::getAdvertising()->stop();
        if (gServer && gConnected) {
            for (uint16_t h : gServer->getPeerDevices()) gServer->disconnect(h);
        }
        portENTER_CRITICAL(&gMux);
        gPeerCount = 0;
        gConnected = false;
        gUiDirty   = true;
        portEXIT_CRITICAL(&gMux);
        Serial.println("[ble] disabled -> radio idle");
    }
}

bool enabled()        { return gEnabled; }
bool connected()      { return gEnabled && gConnected; }
bool appleConnected() { return gEnabled && gAppleConnected; }
int  peerCount()      { return gEnabled ? gPeerCount : 0; }

void onAncsData(NimBLERemoteCharacteristic *, uint8_t *data, size_t len, bool)
{
    if (!data || !len) return;
    if (gAncsBufLen + len > sizeof(gAncsBuf)) gAncsBufLen = 0;
    memcpy(gAncsBuf + gAncsBufLen, data, len);
    gAncsBufLen += len;
    if (gAncsBufLen < 8 || gAncsBuf[0] != 0x00) return;

    // Parse TLV attributes: [attrId:1][len:2LE][utf8:len]
    char fullApp[48] = {0}, app[24] = "iPhone", title[48] = {0}, body[96] = {0};
    size_t pos = 5;
    bool complete = false;
    while (pos + 3 <= gAncsBufLen) {
        uint8_t aid = gAncsBuf[pos];
        uint16_t alen = (uint16_t)(gAncsBuf[pos + 1] | (gAncsBuf[pos + 2] << 8));
        pos += 3;
        if (pos + alen > gAncsBufLen) return;  // wait for remaining BLE chunk
        if (aid == 0 && alen > 0) {
            // Keep full bundle id for Maps/Nav detection, plus shortened display name
            const char *s = (const char *)(gAncsBuf + pos);
            size_t takeFull = alen < sizeof(fullApp) - 1 ? alen : sizeof(fullApp) - 1;
            memcpy(fullApp, s, takeFull); fullApp[takeFull] = 0;
            size_t take = alen < sizeof(app) - 1 ? alen : sizeof(app) - 1;
            memcpy(app, s, take); app[take] = 0;
            const char *dot = strrchr(app, '.');
            if (dot && dot[1]) memmove(app, dot + 1, strlen(dot + 1) + 1);
        } else if (aid == 1) {
            size_t take = alen < sizeof(title) - 1 ? alen : sizeof(title) - 1;
            memcpy(title, gAncsBuf + pos, take); title[take] = 0;
        } else if (aid == 3) {
            size_t take = alen < sizeof(body) - 1 ? alen : sizeof(body) - 1;
            memcpy(body, gAncsBuf + pos, take); body[take] = 0;
            complete = true;
        }
        pos += alen;
    }
    if (complete || pos == gAncsBufLen) {
        gAncsBufLen = 0;
        sanitizePtBrAscii(app);
        sanitizePtBrAscii(title);
        sanitizePtBrAscii(body);

        // Route iPhone navigation notifications (Google Maps, Apple Maps, Waze, OsmAnd) into SC_NAV
        auto hasI = [](const char *hay, const char *needle) -> bool {
            if (!hay || !needle) return false;
            String h(hay); h.toLowerCase();
            String n(needle); n.toLowerCase();
            return h.indexOf(n) >= 0;
        };
        bool isNavApp = hasI(fullApp, "maps") || hasI(fullApp, "waze") || hasI(fullApp, "osmand");
        if (isNavApp && (title[0] || body[0])) {
            char combo[150] = {0};
            snprintf(combo, sizeof(combo), "%s %s", title, body);
            const char *act = "straight";
            if (hasI(combo, "u-turn") || hasI(combo, "uturn") || hasI(combo, "retorno")) act = "uturn";
            else if (hasI(combo, "roundabout") || hasI(combo, "rotatoria")) act = "roundabout";
            else if (hasI(combo, "slight right") || hasI(combo, "mantenha-se a direita") || hasI(combo, "keep right")) act = "slight_right";
            else if (hasI(combo, "slight left") || hasI(combo, "mantenha-se a esquerda") || hasI(combo, "keep left")) act = "slight_left";
            else if (hasI(combo, "right") || hasI(combo, "direita")) act = "right";
            else if (hasI(combo, "left") || hasI(combo, "esquerda")) act = "left";
            else if (hasI(combo, "arrive") || hasI(combo, "destination") || hasI(combo, "chegou") || hasI(combo, "destino")) act = "finish";

            // Extract leading distance token like "150 m", "1.2 km", "500 ft" if present in title/body
            char dist[24] = {0};
            const char *src = title[0] ? title : body;
            for (const char *p = src; *p; p++) {
                if (*p >= '0' && *p <= '9') {
                    char tmp[24] = {0};
                    size_t ti = 0;
                    const char *q = p;
                    while (*q && ti + 1 < sizeof(tmp) && ((*q >= '0' && *q <= '9') || *q == '.' || *q == ',' || *q == ' ')) {
                        tmp[ti++] = *q++;
                    }
                    if (*q == 'm' || *q == 'k' || *q == 'f') {
                        while (*q && ti + 1 < sizeof(tmp) && *q != ' ' && *q != '\n') tmp[ti++] = *q++;
                        tmp[ti] = 0;
                        strncpy(dist, tmp, sizeof(dist) - 1);
                        break;
                    }
                }
            }
            char instr[96] = {0};
            strncpy(instr, body[0] ? body : title, sizeof(instr) - 1);
            shortenNavInstr(instr, 54);
            portENTER_CRITICAL(&gMux);
            bool changed = (strcmp(gNavInstr, instr) != 0 || strcmp(gNavAction, act) != 0);
            gNavActive = true;
            strncpy(gNavAction, act, sizeof(gNavAction) - 1); gNavAction[sizeof(gNavAction) - 1] = 0;
            strncpy(gNavDist, dist, sizeof(gNavDist) - 1); gNavDist[sizeof(gNavDist) - 1] = 0;
            strncpy(gNavInstr, instr, sizeof(gNavInstr) - 1); gNavInstr[sizeof(gNavInstr) - 1] = 0;
            gNavEta[0] = 0;
            if (changed) gBuzzPending = true;
            gUiDirty = true;
            portEXIT_CRITICAL(&gMux);
        }

        portENTER_CRITICAL(&gMux);
        if (gCallActive && title[0]) {
            strncpy(gCallName, title, sizeof(gCallName) - 1);
            gCallName[sizeof(gCallName) - 1] = 0;
        }
        pushNotifLocked(app[0] ? app : "iPhone", title, body);
        portEXIT_CRITICAL(&gMux);
    }
}

void loop()
{
    if (!gEnabled || !gInited || !gServer) return;

    if (gProbePending && millis() >= gProbeAtMs) {
        gProbePending = false;
        NimBLEClient *cli = gServer->getClient(gProbeHandle);
        if (cli && cli->isConnected()) {
            // Try reading peer's real GAP Device Name (0x1800 / 0x2A00) and cache in NVS
            NimBLEConnInfo pInfo = gServer->getPeerInfoByHandle(gProbeHandle);
            std::string pAddr = pInfo.getIdAddress().toString();
            NimBLERemoteService *gap = cli->getService(kGapSvcUuid);
            if (gap) {
                NimBLERemoteCharacteristic *gn = gap->getCharacteristic(kGapNameUuid);
                if (gn && gn->canRead()) {
                    std::string val = gn->readValue();
                    if (!val.empty()) {
                        char nm[20] = {0};
                        strncpy(nm, val.c_str(), sizeof(nm) - 1);
                        sanitizePtBrAscii(nm);
                        if (strcmp(nm, "SM-G991B") == 0) strcpy(nm, "Galaxy S21");
                        if (nm[0]) {
                            Preferences p;
                            p.begin("ble_names", false);
                            p.putString(pAddr.c_str(), nm);
                            p.end();
                            portENTER_CRITICAL(&gMux);
                            gUiDirty = true;
                            portEXIT_CRITICAL(&gMux);
                        }
                    }
                }
            }

            NimBLERemoteService *ams  = cli->getService(kAmsSvcUuid);
            NimBLERemoteService *ancs = cli->getService(kAncsSvcUuid);
            if (ams || ancs) {
                gAppleConnected = true;
                gAppleHandle    = gProbeHandle;
                if (ams) {
                    gAmsRemoteChar = ams->getCharacteristic(kAmsRemoteUuid);
                    NimBLERemoteCharacteristic *eu = ams->getCharacteristic(kAmsEntityUpdateUuid);
                    if (eu && eu->canNotify()) {
                        eu->subscribe(true, onAmsEntityUpdate);
                        const uint8_t subPlayer[] = {0x00, 0x00, 0x01, 0x02, 0x03};
                        const uint8_t subTrack[]  = {0x02, 0x00, 0x01, 0x02, 0x03};
                        eu->writeValue(subPlayer, sizeof(subPlayer), true);
                        eu->writeValue(subTrack,  sizeof(subTrack),  true);
                    }
                }
                if (ancs) {
                    gAncsCtrlChar = ancs->getCharacteristic(kAncsCtrlUuid);
                    NimBLERemoteCharacteristic *ds = ancs->getCharacteristic(kAncsDataUuid);
                    NimBLERemoteCharacteristic *ns = ancs->getCharacteristic(kAncsNotifUuid);
                    if (ds && ds->canNotify()) ds->subscribe(true, onAncsData);
                    if (ns && ns->canNotify()) ns->subscribe(true, onAncsNotif);
                }
                portENTER_CRITICAL(&gMux);
                gUiDirty = true;
                portEXIT_CRITICAL(&gMux);
                Serial.printf("[ble] iPhone Apple services bound (AMS=%d ANCS=%d)\n",
                              ams != nullptr, ancs != nullptr);
            }
        }
    }

    if (gAncsFetchPend && gAncsCtrlChar) {
        gAncsFetchPend = false;
        gAncsBufLen = 0;
        const uint8_t req[] = {
            0x00,
            gAncsFetchUid[0], gAncsFetchUid[1], gAncsFetchUid[2], gAncsFetchUid[3],
            0x00,                   // AppID
            0x01, 32, 0x00,         // Title (32B max)
            0x03, 64, 0x00          // Message (64B max)
        };
        gAncsCtrlChar->writeValue(req, sizeof(req), true);
    }
}

void reAdvertise()
{
    if (!gEnabled) {
        setEnabled(true);
        return;
    }
    if (gInited) {
        startAdv();
        Serial.println("[ble] re-advertising as 'Bangle.js oma'");
    }
}

int listPeers(PeerEntry *out, int maxCount)
{
    if (!out || maxCount <= 0 || !gInited) return 0;
    int n = 0;
    Preferences p;
    p.begin("ble_names", true);

    // 1. Active connected peers first
    if (gServer) {
        std::vector<uint16_t> handles = gServer->getPeerDevices();
        for (uint16_t h : handles) {
            if (n >= maxCount) break;
            NimBLEConnInfo info = gServer->getPeerInfoByHandle(h);
            NimBLEAddress idAddr = info.getIdAddress();
            std::string s = idAddr.toString();
            PeerEntry &e = out[n++];
            memset(&e, 0, sizeof(e));
            strncpy(e.addr, s.c_str(), sizeof(e.addr) - 1);
            String cached = p.getString(e.addr, "");
            if (cached.length() > 0) {
                snprintf(e.label, sizeof(e.label), "%s", cached.c_str());
            } else if (gAppleConnected && h == gAppleHandle) {
                snprintf(e.label, sizeof(e.label), "iPhone");
            } else {
                snprintf(e.label, sizeof(e.label), "%s", e.addr);
            }
            e.connected  = true;
            e.bonded     = NimBLEDevice::isBonded(idAddr);
            e.connHandle = h;
        }
    }

    // 2. Saved bonded peers not currently connected
    int bonds = NimBLEDevice::getNumBonds();
    for (int i = 0; i < bonds && n < maxCount; i++) {
        NimBLEAddress bAddr = NimBLEDevice::getBondedAddress(i);
        std::string s = bAddr.toString();
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcasecmp(out[j].addr, s.c_str()) == 0) { dup = true; break; }
        }
        if (dup) continue;
        PeerEntry &e = out[n++];
        memset(&e, 0, sizeof(e));
        strncpy(e.addr, s.c_str(), sizeof(e.addr) - 1);
        String cached = p.getString(e.addr, "");
        snprintf(e.label, sizeof(e.label), "%s", cached.length() > 0 ? cached.c_str() : e.addr);
        e.connected  = false;
        e.bonded     = true;
        e.connHandle = BLE_HS_CONN_HANDLE_NONE;
    }
    p.end();
    return n;
}

bool removePeer(const char *addr)
{
    if (!addr || !addr[0] || !gInited) return false;
    bool did = false;

    if (gServer) {
        std::vector<uint16_t> handles = gServer->getPeerDevices();
        for (uint16_t h : handles) {
            NimBLEConnInfo info = gServer->getPeerInfoByHandle(h);
            if (strcasecmp(info.getIdAddress().toString().c_str(), addr) == 0 ||
                strcasecmp(info.getAddress().toString().c_str(), addr) == 0) {
                gServer->disconnect(h);
                did = true;
            }
        }
    }

    int bonds = NimBLEDevice::getNumBonds();
    for (int i = bonds - 1; i >= 0; i--) {
        NimBLEAddress bAddr = NimBLEDevice::getBondedAddress(i);
        if (strcasecmp(bAddr.toString().c_str(), addr) == 0) {
            NimBLEDevice::deleteBond(bAddr);
            did = true;
        }
    }

    if (did) {
        portENTER_CRITICAL(&gMux);
        gUiDirty = true;
        portEXIT_CRITICAL(&gMux);
        if (gEnabled) startAdv();
    }
    return did;
}

bool takeBuzzFlag()
{
    bool v;
    portENTER_CRITICAL(&gMux);
    v = gBuzzPending;
    gBuzzPending = false;
    portEXIT_CRITICAL(&gMux);
    return v;
}

bool takeUiDirty()
{
    bool v;
    portENTER_CRITICAL(&gMux);
    v = gUiDirty;
    gUiDirty = false;
    portEXIT_CRITICAL(&gMux);
    return v;
}

bool takeClockSync(time_t *utcOut, int *tzOffsetMinOut, bool *hasUtc, bool *hasTz)
{
    bool any = false;
    portENTER_CRITICAL(&gMux);
    *hasUtc = gHasUtc;
    *hasTz  = gHasTz;
    if (gHasUtc) { *utcOut = gSyncUtc; gHasUtc = false; any = true; }
    if (gHasTz)  { *tzOffsetMinOut = gSyncTzMin; gHasTz = false; any = true; }
    portEXIT_CRITICAL(&gMux);
    return any;
}

bool takeThemeSync(ThemePayload *out)
{
    bool ok = false;
    portENTER_CRITICAL(&gMux);
    if (gHasTheme) {
        *out = gTheme;
        gHasTheme = false;
        ok = true;
    }
    portEXIT_CRITICAL(&gMux);
    return ok;
}

uint32_t unreadCount() { return gUnread; }

void clearUnread()
{
    portENTER_CRITICAL(&gMux);
    gUnread = 0;
    gInboxCount = 0;
    gUiDirty = true;
    portEXIT_CRITICAL(&gMux);
}

void latestLine(char *out, size_t n)
{
    if (!n) return;
    portENTER_CRITICAL(&gMux);
    if (gCallActive) {
        snprintf(out, n, "CALL: %s", gCallName[0] ? gCallName : "Incoming");
    } else if (gNavActive && (gNavDist[0] || gNavInstr[0])) {
        snprintf(out, n, "NAV: %s %s", gNavDist, gNavInstr);
    } else if (gUnread > 0 && gInboxCount > 0) {
        snprintf(out, n, "%s: %s %s",
                 gInbox[0].src,
                 gInbox[0].title,
                 gInbox[0].body);
    } else if (gPlaying && gTrack[0]) {
        snprintf(out, n, ">> %s - %s", gArtist[0] ? gArtist : "Media", gTrack);
    } else {
        out[0] = 0;
    }
    portEXIT_CRITICAL(&gMux);
}

void inboxSummary(char *out, size_t n)
{
    if (!n) return;
    portENTER_CRITICAL(&gMux);
    size_t off = 0;
    if (gTrack[0]) {
        off += (size_t)snprintf(out + off, n - off, "[%s] %s - %s\n",
                                gPlaying ? "PLAY" : "PAUSE",
                                gArtist[0] ? gArtist : "Media", gTrack);
    }
    if (gInboxCount == 0 && off == 0) {
        snprintf(out, n, "No notifications yet.\nConnected peers: %d", gPeerCount);
    } else {
        for (int i = 0; i < gInboxCount && off + 8 < n; i++) {
            off += (size_t)snprintf(out + off, n - off, "- %s: %s %s\n",
                                    gInbox[i].src, gInbox[i].title, gInbox[i].body);
        }
    }
    portEXIT_CRITICAL(&gMux);
}

bool weatherActive() { return gWeatherOk; }

void weatherSummary(char *out, size_t n)
{
    if (!out || !n) return;
    portENTER_CRITICAL(&gMux);
    if (!gWeatherOk) {
        out[0] = 0;
    } else {
        snprintf(out, n, "%dC %s", gWeatherC, gWeatherCond[0] ? gWeatherCond : "CLR");
    }
    portEXIT_CRITICAL(&gMux);
}

const char *musicTrack()   { return gTrack; }
const char *musicArtist()  { return gArtist; }
const char *musicAlbum()   { return gAlbum; }
bool        musicPlaying() { return gPlaying; }
bool        musicShuffle() { return gShuffle; }
int         musicRepeat()  { return gRepeat; }
int         queueCount()   { return gQueueCount; }
const char *queueItem(int idx)
{
    if (idx < 0 || idx >= gQueueCount) return "";
    return gQueue[idx];
}
void setQueueLocal(const char *pipeList)
{
    if (!pipeList) return;
    portENTER_CRITICAL(&gMux);
    gQueueCount = 0;
    const char *p = pipeList;
    while (*p && gQueueCount < 6) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len >= sizeof(gQueue[0])) len = sizeof(gQueue[0]) - 1;
        memcpy(gQueue[gQueueCount], p, len);
        gQueue[gQueueCount][len] = 0;
        sanitizePtBrAscii(gQueue[gQueueCount]);
        if (gQueue[gQueueCount][0]) gQueueCount++;
        if (!bar) break;
        p = bar + 1;
    }
    gUiDirty = true;
    portEXIT_CRITICAL(&gMux);
}

void mediaSnapshot(char *track, size_t tn, char *artist, size_t an,
                   char *album, size_t aln, bool *playing)
{
    portENTER_CRITICAL(&gMux);
    if (tn)  { strncpy(track,  gTrack,  tn - 1);  track[tn - 1]  = 0; }
    if (an)  { strncpy(artist, gArtist, an - 1);  artist[an - 1] = 0; }
    if (aln) { strncpy(album,  gAlbum,  aln - 1); album[aln - 1] = 0; }
    if (playing) *playing = gPlaying;
    portEXIT_CRITICAL(&gMux);
}

void mediaProgress(uint32_t *posSec, uint32_t *durSec)
{
    portENTER_CRITICAL(&gMux);
    uint32_t pos = gTrackPosS;
    if (gPlaying && gTrackPosAtMs > 0) {
        pos += (millis() - gTrackPosAtMs) / 1000UL;
    }
    if (gTrackDurS > 0 && pos > gTrackDurS) pos = gTrackDurS;
    if (posSec) *posSec = pos;
    if (durSec) *durSec = gTrackDurS;
    portEXIT_CRITICAL(&gMux);
}

void setMediaLocal(bool playing, const char *artist, const char *track, const char *album)
{
    portENTER_CRITICAL(&gMux);
    gPlaying = playing;
    if (artist) { strncpy(gArtist, artist, sizeof(gArtist) - 1); gArtist[sizeof(gArtist) - 1] = 0; }
    if (track)  { strncpy(gTrack,  track,  sizeof(gTrack) - 1);  gTrack[sizeof(gTrack) - 1]   = 0; }
    if (album)  { strncpy(gAlbum,  album,  sizeof(gAlbum) - 1);  gAlbum[sizeof(gAlbum) - 1]   = 0; }
    gUiDirty = true;
    portEXIT_CRITICAL(&gMux);
}

bool navActive() { return gNavActive; }

void navSnapshot(char *action, size_t acn, char *dist, size_t dn,
                 char *instr, size_t in, char *eta, size_t en, bool *active)
{
    portENTER_CRITICAL(&gMux);
    if (acn) { strncpy(action, gNavAction, acn - 1); action[acn - 1] = 0; }
    if (dn)  { strncpy(dist,   gNavDist,   dn - 1);  dist[dn - 1]    = 0; }
    if (in)  { strncpy(instr,  gNavInstr,  in - 1);  instr[in - 1]   = 0; }
    if (en)  { strncpy(eta,    gNavEta,    en - 1);  eta[en - 1]     = 0; }
    if (active) *active = gNavActive;
    portEXIT_CRITICAL(&gMux);
}

void setNavLocal(bool active, const char *action, const char *dist,
                 const char *instr, const char *eta)
{
    portENTER_CRITICAL(&gMux);
    gNavActive = active;
    if (action) { strncpy(gNavAction, action, sizeof(gNavAction) - 1); gNavAction[sizeof(gNavAction) - 1] = 0; }
    else gNavAction[0] = 0;
    if (dist)   { strncpy(gNavDist,   dist,   sizeof(gNavDist) - 1);   gNavDist[sizeof(gNavDist) - 1]     = 0; }
    else gNavDist[0] = 0;
    if (instr)  { strncpy(gNavInstr,  instr,  sizeof(gNavInstr) - 1);  gNavInstr[sizeof(gNavInstr) - 1]   = 0; }
    else gNavInstr[0] = 0;
    if (eta)    { strncpy(gNavEta,    eta,    sizeof(gNavEta) - 1);    gNavEta[sizeof(gNavEta) - 1]       = 0; }
    else gNavEta[0] = 0;
    sanitizePtBrAscii(gNavAction);
    sanitizePtBrAscii(gNavDist);
    sanitizePtBrAscii(gNavInstr);
    sanitizePtBrAscii(gNavEta);
    shortenNavInstr(gNavInstr, 54);
    gUiDirty = true;
    portEXIT_CRITICAL(&gMux);
}

float gpsSpeedKmh() { return gGpsSpeedKmh; }
bool  gpsValid()    { return gGpsValid; }
bool  callActive()  { return gCallActive; }
const char *callName() { return gCallName; }

void setGpsSpeedLocal(float kmh)
{
    portENTER_CRITICAL(&gMux);
    gGpsSpeedKmh = kmh < 0 ? 0 : kmh;
    gGpsValid    = true;
    gUiDirty     = true;
    portEXIT_CRITICAL(&gMux);
}

bool latestNotif(char *src, size_t sn, char *title, size_t tn,
                 char *body, size_t bn, long *id, bool *canReply)
{
    portENTER_CRITICAL(&gMux);
    bool ok = (gInboxCount > 0);
    if (ok) {
        if (sn) { strncpy(src, gInbox[0].src, sn - 1); src[sn - 1] = 0; }
        if (tn) { strncpy(title, gInbox[0].title, tn - 1); title[tn - 1] = 0; }
        if (bn) { strncpy(body, gInbox[0].body, bn - 1); body[bn - 1] = 0; }
        if (id) *id = gInbox[0].id;
        if (canReply) *canReply = gInbox[0].canReply;
    } else {
        if (sn) src[0] = 0;
        if (tn) title[0] = 0;
        if (bn) body[0] = 0;
        if (id) *id = 0;
        if (canReply) *canReply = false;
    }
    portEXIT_CRITICAL(&gMux);
    return ok;
}

bool sendNotifReply(long id, const char *msg)
{
    if (!msg || !msg[0]) return false;
    char clean[64] = {0};
    size_t w = 0;
    for (const char *p = msg; *p && w + 1 < sizeof(clean); p++) {
        if (*p == '"' || *p == '\\' || *p == '\r' || *p == '\n') continue;
        clean[w++] = *p;
    }
    clean[w] = 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"t\":\"notify\",\"n\":\"REPLY\",\"id\":%ld,\"msg\":\"%s\"}\r\n", id, clean);
    bool ok = sendRawLine(buf);
    if (ok) {
        portENTER_CRITICAL(&gMux);
        if (gUnread > 0) gUnread--;
        gUiDirty = true;
        portEXIT_CRITICAL(&gMux);
    }
    return ok;
}

bool sendNotifDismiss(long id)
{
    char buf[80];
    snprintf(buf, sizeof(buf), "{\"t\":\"notify\",\"n\":\"DISMISS\",\"id\":%ld}\r\n", id);
    bool ok = sendRawLine(buf);
    portENTER_CRITICAL(&gMux);
    if (gUnread > 0) gUnread--;
    gUiDirty = true;
    portEXIT_CRITICAL(&gMux);
    return ok;
}

bool sendMusicCmd(const char *cmd)
{
    if (cmd) {
        portENTER_CRITICAL(&gMux);
        if (strcmp(cmd, "play") == 0)       gPlaying = true;
        else if (strcmp(cmd, "pause") == 0) gPlaying = false;
        gUiDirty = true;
        portEXIT_CRITICAL(&gMux);
    }
    bool sentApple = false;
    if (gAppleConnected && gAmsRemoteChar && cmd) {
        uint8_t op = 0xFF;
        if      (strcmp(cmd, "play") == 0)       op = 0x00;
        else if (strcmp(cmd, "pause") == 0)      op = 0x01;
        else if (strcmp(cmd, "next") == 0)       op = 0x03;
        else if (strcmp(cmd, "previous") == 0)   op = 0x04;
        else if (strcmp(cmd, "volumeup") == 0)   op = 0x05;
        else if (strcmp(cmd, "volumedown") == 0) op = 0x06;
        if (op != 0xFF) sentApple = gAmsRemoteChar->writeValue(&op, 1, true);
    }
    char buf[64];
    // Espruino Bluetooth.println() framing: exact JSON + \r\n (Gadgetbridge strips idx-1 before \n!)
    snprintf(buf, sizeof(buf), "{\"t\":\"music\",\"n\":\"%s\"}\r\n", cmd ? cmd : "play");
    bool sentNus = sendRawLine(buf);
    return sentApple || sentNus;
}

bool sendFindPhone(bool on)
{
    return sendRawLine(on ? "{\"t\":\"findPhone\",\"n\":true}\r\n"
                          : "{\"t\":\"findPhone\",\"n\":false}\r\n");
}

bool sendOmaAction(const char *act)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"t\":\"oma\",\"act\":\"%s\"}\r\n", act);
    return sendRawLine(buf);
}

bool toggleShuffle()
{
    portENTER_CRITICAL(&gMux);
    gShuffle = !gShuffle;
    gUiDirty = true;
    portEXIT_CRITICAL(&gMux);
    bool sentApple = false;
    if (gAppleConnected && gAmsRemoteChar) {
        uint8_t op = 0x08;  // ToggleShuffleMode
        sentApple = gAmsRemoteChar->writeValue(&op, 1, true);
    }
    return sendOmaAction("shuffle") || sentApple;
}

bool toggleRepeat()
{
    portENTER_CRITICAL(&gMux);
    gRepeat = (gRepeat + 1) % 3;
    gUiDirty = true;
    portEXIT_CRITICAL(&gMux);
    bool sentApple = false;
    if (gAppleConnected && gAmsRemoteChar) {
        uint8_t op = 0x07;  // ToggleRepeatMode
        sentApple = gAmsRemoteChar->writeValue(&op, 1, true);
    }
    return sendOmaAction("repeat") || sentApple;
}

bool sendQueuePlay(int idx)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"t\":\"oma\",\"act\":\"qplay\",\"idx\":%d}\r\n", idx);
    return sendRawLine(buf);
}

size_t statusText(char *out, size_t n)
{
    return snprintf(out, n,
        "[ble] enabled=%d  connected=%d  peers=%d  name='%s'  rx_lines=%lu  unread=%lu  track='%s'",
        (int)gEnabled, (int)gConnected, gPeerCount, kDeviceName,
        (unsigned long)gRxLines, (unsigned long)gUnread,
        gTrack[0] ? gTrack : "-");
}

}  // namespace blecomp

/*
 * T-Watch S3 — Omarchy Minimalist Companion Firmware
 *
 * Clean, squared-corner (0px radius) wearable UI built for Omarchy + Galaxy S21 BLE.
 * Legacy conference QR, phone SoftAP web server, and Matrix rain screensaver removed.
 *
 * Screens:
 *   0: Watch Face     (BLE/Mesh status, squared battery bar, hero clock, S21 ticker)
 *   1: Media Player   (Dedicated full-screen BLE media transport + volume)
 *   2: OmaLauncher    (2x3 squared utility tile grid)
 *   3: S21 / BLE      (BLE radio management, re-pair/adv, ping, notification inbox)
 *   4: Color Schemes  (8 curated palettes + live Omarchy desktop sync)
 *   5: Steps          (BMA423/456 step counter)
 *   6: LoRa Mesh      (SX1262 Meshtastic node, off by default to save battery)
 *   7: Settings       (scheme picker shortcut, BLE/Mesh power switches, sliders)
 */

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <time.h>

#include "ble_companion.h"
#include "mesh_service.h"

#include <LV_Helper.h>
#include <LilyGoLib.h>

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

// ----------------------------------------------------------------- palettes
struct Palette {
    const char *name;
    uint32_t bg, surface, text, muted, accent;
};

static char gOmaThemeName[24] = "Omarchy";

static Palette PALETTES[] = {
    {"Everforest",  0x181D20, 0x2D353B, 0xD3C6AA, 0x475258, 0x7FBBB3},
    {gOmaThemeName, 0x181D20, 0x2D353B, 0xD3C6AA, 0x475258, 0x7FBBB3},
    {"Tokyo Night", 0x1A1B26, 0x24283B, 0xC0CAF5, 0x565F89, 0x7AA2F7},
    {"Catppuccin",  0x1E1E2E, 0x313244, 0xCDD6F4, 0x6C7086, 0xCBA6F7},
    {"Gruvbox",     0x1D2021, 0x32302F, 0xEBDBB2, 0x928374, 0xD79921},
    {"Kanagawa",    0x1F1F28, 0x2A2A37, 0xDCD7BA, 0x727169, 0xDCA561},
    {"Rose Pine",   0x191724, 0x26233A, 0xE0DEF4, 0x6E6A86, 0xEBBCBA},
    {"Matte Mono",  0x0D0D0D, 0x1C1C1C, 0xE6E6E6, 0x666666, 0xFFFFFF},
};
static constexpr int PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);

static int  cfgPalette     = 0;
static int  cfgBright      = 160;
static int  cfgTimeoutS    = 15;
static int  cfgTzOffsetMin = -180;  // UTC-03 (Brazil)
static Preferences prefs;

static inline lv_color_t colBg()      { return lv_color_hex(PALETTES[cfgPalette].bg); }
static inline lv_color_t colSurface() { return lv_color_hex(PALETTES[cfgPalette].surface); }
static inline lv_color_t colText()    { return lv_color_hex(PALETTES[cfgPalette].text); }
static inline lv_color_t colMuted()   { return lv_color_hex(PALETTES[cfgPalette].muted); }
static inline lv_color_t colAccent()  { return lv_color_hex(PALETTES[cfgPalette].accent); }

// ----------------------------------------------------------------- screens
enum Screen {
    SC_WATCH = 0,
    SC_MEDIA,
    SC_NAV,
    SC_NOTIF,
    SC_LAUNCHER,
    SC_BLE,
    SC_THEMES,
    SC_STEPS,
    SC_MESH,
    SC_SETTINGS,
    SC_COUNT
};

static const bool kSwipeOk[SC_COUNT] = {
    true,   // SC_WATCH
    true,   // SC_MEDIA
    true,   // SC_NAV
    true,   // SC_NOTIF
    true,   // SC_LAUNCHER
    true,   // SC_BLE
    true,   // SC_THEMES
    true,   // SC_STEPS
    true,   // SC_MESH
    false,  // SC_SETTINGS (sliders)
};

static lv_obj_t *screens[SC_COUNT] = {};
static int       curScreen         = SC_WATCH;

// Widgets
static lv_obj_t *lblLinkFace = nullptr, *lblDate = nullptr, *lblTime = nullptr, *lblSeconds = nullptr;
static lv_obj_t *lblBattery = nullptr, *barBattery = nullptr, *lblStepsFace = nullptr;
static lv_obj_t *lblMediaState = nullptr, *lblMediaTrack = nullptr, *lblMediaArtist = nullptr;
static lv_obj_t *lblMediaAlbum = nullptr, *boxMediaCard = nullptr, *btnMediaPlay = nullptr, *lblMediaPlay = nullptr;
static lv_obj_t *lblNavEta = nullptr, *lblNavDist = nullptr, *lblNavAction = nullptr, *lblNavInstr = nullptr;
static lv_obj_t *lblNavHdr = nullptr, *lblNavSpeed = nullptr, *lblNavMusic = nullptr, *lblRideBtn = nullptr;
static lv_obj_t *wakeShield = nullptr;
static lv_obj_t *boxNavArrow = nullptr, *lineNavStem = nullptr, *lineNavHead = nullptr;
static lv_point_precise_t gNavStemPts[4] = {{38, 66}, {38, 16}, {38, 16}, {38, 16}};
static lv_point_precise_t gNavHeadPts[3] = {{22, 32}, {38, 16}, {54, 32}};
static lv_obj_t *lblBleState = nullptr, *lblBleInbox = nullptr, *lblBleToggle = nullptr;
static lv_obj_t *lblNotifTag = nullptr, *lblNotifSrc = nullptr, *lblNotifBody = nullptr;
static lv_obj_t *rowBlePeer[3] = {nullptr, nullptr, nullptr};
static lv_obj_t *lblBlePeer[3] = {nullptr, nullptr, nullptr};
static char      gBlePeerAddr[3][20] = {{0}, {0}, {0}};
static lv_obj_t *lblStepsBig = nullptr;
static lv_obj_t *lblMeshId = nullptr, *lblMeshState = nullptr, *lblMeshStats = nullptr;
static lv_obj_t *lblMeshNodes = nullptr, *lblMeshLast = nullptr;
static lv_obj_t *lblBrightVal = nullptr, *lblTimeoutVal = nullptr, *lblAbout = nullptr;

// Runtime state
static bool        dimmed          = false;
static bool        deepOff         = false;
static bool        ateWakeTouch    = false;
static bool        gRideMode       = false;
static uint32_t    gRideStartMs    = 0;
static volatile bool gBtnShort     = false;
static volatile bool gBtnLong      = false;
static lv_indev_t *gIndev          = nullptr;
static bool        touchWasDown    = false;
static int16_t     touchX0 = 0, touchY0 = 0;
static uint32_t    gTiltSeen       = 0;
static uint32_t    gLastStepCount  = 0;
static uint32_t    lastClockMs     = 0;
static uint32_t    lastBatteryMs   = 0;
static uint32_t    meshUiAt        = 0;
static String      serialBuf;

#define FONT_XS &lv_font_montserrat_12
#define FONT_S  &lv_font_montserrat_14
#define FONT_M  &lv_font_montserrat_18
#define FONT_L  &lv_font_montserrat_36
#define FONT_XL &lv_font_montserrat_48
#define GUTTER  8

static void buildUI();
static void wakeScreen();
static void showScreen(int idx);
static void updateWatchLinkUI();
static void updateMediaUI();
static void updateNavUI();
static void updateBleUI();
static void updateMeshUI();

// ----------------------------------------------------------------- UI helpers
static lv_obj_t *mkPage(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, 240, 240);
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, colBg(), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, GUTTER, 0);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    return p;
}

static lv_obj_t *mkLabel(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                         uint32_t colorHex, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colorHex), 0);
    lv_obj_align(l, LV_ALIGN_CENTER, x, y);
    return l;
}

static lv_obj_t *mkTag(lv_obj_t *parent, const char *txt, int x, int y)
{
    lv_obj_t *t = lv_label_create(parent);
    lv_label_set_text(t, txt);
    lv_obj_set_style_text_font(t, FONT_XS, 0);
    lv_obj_set_style_text_color(t, colAccent(), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, x, y);
    return t;
}

static void styleSquareSlider(lv_obj_t *sl)
{
    lv_obj_set_style_radius(sl, 0, 0);
    lv_obj_set_style_radius(sl, 0, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sl, colSurface(), 0);
    lv_obj_set_style_bg_color(sl, colAccent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, colText(), LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 1, 0);
    lv_obj_set_style_border_color(sl, colMuted(), 0);
}

static void mkButton(lv_obj_t *parent, const char *txt, int x, int y, int w, int h,
                     lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_bg_color(b, colSurface(), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, colMuted(), 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, colText(), 0);
    lv_obj_center(l);
}

// ----------------------------------------------------------------- RTC math
static time_t utcFromTm(const struct tm &t)
{
    int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
    int a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
    long jdn = d + (153 * mm + 2) / 5 + 365L * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
    long days = jdn - 2440588L;
    return (time_t)days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

static void tmFromUtc(time_t utc, struct tm &t)
{
    long days = (long)(utc / 86400);
    long secs = (long)(utc % 86400);
    if (secs < 0) { secs += 86400; days -= 1; }
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1;
    long m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    memset(&t, 0, sizeof(t));
    t.tm_year = (int)y - 1900; t.tm_mon = (int)m - 1; t.tm_mday = (int)d;
    t.tm_hour = (int)(secs / 3600); t.tm_min = (int)((secs % 3600) / 60);
    t.tm_sec  = (int)(secs % 60);
    t.tm_wday = (int)((days % 7 + 11) % 7);
}

static bool rtcValid()
{
    struct tm t;
    instance.rtc.getDateTime(&t);
    return (t.tm_year + 1900) >= 2020;
}

static void rtcSetUtc(time_t utc)
{
    struct tm t;
    tmFromUtc(utc, t);
    instance.rtc.setDateTime((uint16_t)(t.tm_year + 1900), (uint8_t)(t.tm_mon + 1),
                             (uint8_t)t.tm_mday, (uint8_t)t.tm_hour,
                             (uint8_t)t.tm_min, (uint8_t)t.tm_sec);
}

static void rtcFromBuildTime()
{
    static const char *mon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char m[4] = {0};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(__DATE__, "%3s %d %d", m, &d, &y) != 3) return;
    if (sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss) != 3) return;
    int mo = 0;
    for (int i = 0; i < 12; i++) if (!strcmp(m, mon[i])) mo = i;
    struct tm t = {};
    t.tm_year = y - 1900; t.tm_mon = mo; t.tm_mday = d;
    t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
    rtcSetUtc(utcFromTm(t) - (time_t)cfgTzOffsetMin * 60);
}

static time_t rtcNowUtc()
{
    struct tm t;
    instance.rtc.getDateTime(&t);
    if ((t.tm_year + 1900) < 2020) return 0;
    return utcFromTm(t);
}

// ----------------------------------------------------------------- theme sync
static void applyThemePayload(const blecomp::ThemePayload &tp)
{
    strncpy(gOmaThemeName, tp.name[0] ? tp.name : "Omarchy", sizeof(gOmaThemeName) - 1);
    gOmaThemeName[sizeof(gOmaThemeName) - 1] = 0;
    PALETTES[1].bg      = tp.bg;
    PALETTES[1].surface = tp.surface;
    PALETTES[1].text    = tp.text;
    PALETTES[1].muted   = tp.muted;
    PALETTES[1].accent  = tp.accent;
    cfgPalette = 1;

    prefs.putInt("palette", cfgPalette);
    prefs.putString("oname", gOmaThemeName);
    prefs.putUInt("obg",   tp.bg);
    prefs.putUInt("osurf", tp.surface);
    prefs.putUInt("otxt",  tp.text);
    prefs.putUInt("omut",  tp.muted);
    prefs.putUInt("oacc",  tp.accent);
    buildUI();
}

static void onPalette(lv_event_t *e)
{
    cfgPalette = (int)(intptr_t)lv_event_get_user_data(e);
    prefs.putInt("palette", cfgPalette);
    buildUI();
}

static void onBright(lv_event_t *e)
{
    cfgBright = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    if (lblBrightVal) lv_label_set_text_fmt(lblBrightVal, "%d", cfgBright);
    instance.setBrightness(cfgBright);
    prefs.putInt("bright", cfgBright);
}

static void onTimeout(lv_event_t *e)
{
    cfgTimeoutS = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    if (lblTimeoutVal) lv_label_set_text_fmt(lblTimeoutVal, "%ds", cfgTimeoutS);
    prefs.putInt("timeout", cfgTimeoutS);
}

// ----------------------------------------------------------------- builders
static void buildWatch(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);

    // Top Casio bezel strip: link status left, battery % + bar right
    lblLinkFace = lv_label_create(p);
    lv_obj_set_style_text_font(lblLinkFace, FONT_XS, 0);
    lv_obj_set_style_text_color(lblLinkFace, colAccent(), 0);
    lv_obj_align(lblLinkFace, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_label_set_text(lblLinkFace, "ble");

    lblBattery = lv_label_create(p);
    lv_obj_set_style_text_font(lblBattery, FONT_XS, 0);
    lv_obj_set_style_text_color(lblBattery, colMuted(), 0);
    lv_obj_align(lblBattery, LV_ALIGN_TOP_RIGHT, -46, 0);
    lv_label_set_text(lblBattery, "--%");

    barBattery = lv_bar_create(p);
    lv_obj_set_size(barBattery, 38, 8);
    lv_obj_align(barBattery, LV_ALIGN_TOP_RIGHT, -2, 3);
    lv_bar_set_range(barBattery, 0, 100);
    lv_obj_set_style_bg_color(barBattery, colSurface(), 0);
    lv_obj_set_style_bg_color(barBattery, colAccent(), LV_PART_INDICATOR);
    lv_obj_set_style_border_width(barBattery, 1, 0);
    lv_obj_set_style_border_color(barBattery, colMuted(), 0);
    lv_obj_set_style_radius(barBattery, 0, 0);
    lv_obj_set_style_radius(barBattery, 0, LV_PART_INDICATOR);

    // Casio Digital LCD Frame (220x132 squared bezel)
    lv_obj_t *lcd = lv_obj_create(p);
    lv_obj_set_size(lcd, 220, 132);
    lv_obj_align(lcd, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_bg_color(lcd, colSurface(), 0);
    lv_obj_set_style_border_width(lcd, 2, 0);
    lv_obj_set_style_border_color(lcd, colMuted(), 0);
    lv_obj_set_style_radius(lcd, 0, 0);
    lv_obj_set_style_pad_all(lcd, 6, 0);
    lv_obj_set_scrollbar_mode(lcd, LV_SCROLLBAR_MODE_OFF);

    lblDate = lv_label_create(lcd);
    lv_obj_set_style_text_font(lblDate, FONT_XS, 0);
    lv_obj_set_style_text_color(lblDate, colAccent(), 0);
    lv_obj_align(lblDate, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_label_set_text(lblDate, "--- -- ---");

    lv_obj_t *modeTag = lv_label_create(lcd);
    lv_obj_set_style_text_font(modeTag, FONT_XS, 0);
    lv_obj_set_style_text_color(modeTag, colMuted(), 0);
    lv_obj_align(modeTag, LV_ALIGN_TOP_RIGHT, -2, 0);
    lv_label_set_text(modeTag, "24H");

    // Inner LCD hairline divider below date
    lv_obj_t *topRule = lv_obj_create(lcd);
    lv_obj_set_size(topRule, 204, 1);
    lv_obj_align(topRule, LV_ALIGN_TOP_MID, 0, 18);
    lv_obj_set_style_bg_color(topRule, colMuted(), 0);
    lv_obj_set_style_border_width(topRule, 0, 0);
    lv_obj_set_style_radius(topRule, 0, 0);

    lblTime = lv_label_create(lcd);
    lv_obj_set_style_text_font(lblTime, FONT_XL, 0);
    lv_obj_set_style_text_color(lblTime, colText(), 0);
    lv_obj_align(lblTime, LV_ALIGN_LEFT_MID, 6, 0);
    lv_label_set_text(lblTime, "--:--");

    lblSeconds = lv_label_create(lcd);
    lv_obj_set_style_text_font(lblSeconds, FONT_M, 0);
    lv_obj_set_style_text_color(lblSeconds, colAccent(), 0);
    lv_obj_align(lblSeconds, LV_ALIGN_RIGHT_MID, -6, 8);
    lv_label_set_text(lblSeconds, ":--");

    lv_obj_t *botRule = lv_obj_create(lcd);
    lv_obj_set_size(botRule, 204, 2);
    lv_obj_align(botRule, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_color(botRule, colAccent(), 0);
    lv_obj_set_style_bg_opa(botRule, LV_OPA_70, 0);
    lv_obj_set_style_border_width(botRule, 0, 0);
    lv_obj_set_style_radius(botRule, 0, 0);

    lblStepsFace = lv_label_create(lcd);
    lv_obj_set_width(lblStepsFace, 204);
    lv_label_set_long_mode(lblStepsFace, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(lblStepsFace, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(lblStepsFace, FONT_XS, 0);
    lv_obj_set_style_text_color(lblStepsFace, colText(), 0);
    lv_obj_align(lblStepsFace, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_label_set_text(lblStepsFace, "STP 0");

    mkButton(p, ":: apps", 0, 95, 96, 24, [](lv_event_t *) {
        showScreen(SC_LAUNCHER);
    }, nullptr);
}

static void onLauncherJump(lv_event_t *e)
{
    showScreen((int)(intptr_t)lv_event_get_user_data(e));
}

static void buildLauncher(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 6, 0);

    lv_obj_t *hdrRow = lv_obj_create(p);
    lv_obj_set_size(hdrRow, 224, 16);
    lv_obj_set_style_bg_opa(hdrRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdrRow, 0, 0);
    lv_obj_set_style_pad_all(hdrRow, 0, 0);
    lv_obj_set_flex_flow(hdrRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdrRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hdr = lv_label_create(hdrRow);
    lv_label_set_text(hdr, "APPS");
    lv_obj_set_style_text_font(hdr, FONT_XS, 0);
    lv_obj_set_style_text_color(hdr, colAccent(), 0);

    lv_obj_t *sub = lv_label_create(hdrRow);
    lv_label_set_text_fmt(sub, "ble:%d", blecomp::peerCount());
    lv_obj_set_style_text_font(sub, FONT_XS, 0);
    lv_obj_set_style_text_color(sub, colMuted(), 0);

    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_set_size(grid, 224, 168);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_style_pad_row(grid, 5, 0);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

    struct Tile { const char *title; const char *tag; int screen; bool highlight; };
    const Tile kTiles[] = {
        {"MEDIA",    blecomp::musicPlaying() ? "playing" : "player", SC_MEDIA,    blecomp::musicPlaying()},
        {"NAV",      blecomp::navActive()    ? "routing" : "gps",    SC_NAV,      blecomp::navActive()},
        {"MSGS",     blecomp::unreadCount()  ? "unread"  : "reply",  SC_NOTIF,    blecomp::unreadCount() > 0},
        {"BLE",      blecomp::connected()    ? "linked"  : (blecomp::enabled() ? "adv" : "off"), SC_BLE, false},
        {"THEMES",   PALETTES[cfgPalette].name,                      SC_THEMES,   false},
        {"STEPS",    "pedometer",                                    SC_STEPS,    false},
        {"MESH",     meshsvc::enabled() ? "radio on" : "off",        SC_MESH,     meshsvc::enabled()},
        {"SETTINGS", "power/ui",                                     SC_SETTINGS, false},
    };

    for (const auto &t : kTiles) {
        lv_obj_t *b = lv_button_create(grid);
        lv_obj_set_size(b, 108, 37);
        lv_obj_set_style_bg_color(b, colSurface(), 0);
        lv_obj_set_style_border_width(b, t.highlight ? 2 : 1, 0);
        lv_obj_set_style_border_color(b, t.highlight ? colAccent() : colMuted(), 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 4, 0);
        lv_obj_add_event_cb(b, onLauncherJump, LV_EVENT_CLICKED, (void *)(intptr_t)t.screen);

        lv_obj_t *tl = lv_label_create(b);
        lv_label_set_text(tl, t.title);
        lv_obj_set_style_text_font(tl, FONT_XS, 0);
        lv_obj_set_style_text_color(tl, t.highlight ? colAccent() : colText(), 0);
        lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 2, 0);

        lv_obj_t *tg = lv_label_create(b);
        lv_label_set_text(tg, t.tag);
        lv_obj_set_width(tg, 94);
        lv_label_set_long_mode(tg, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(tg, FONT_XS, 0);
        lv_obj_set_style_text_color(tg, colMuted(), 0);
        lv_obj_align(tg, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    }
}

static void buildThemes(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 4, 0);

    lv_obj_t *hdr = lv_label_create(p);
    lv_label_set_text(hdr, "COLOR SCHEMES");
    lv_obj_set_style_text_font(hdr, FONT_XS, 0);
    lv_obj_set_style_text_color(hdr, colAccent(), 0);

    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_set_size(grid, 224, 172);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < PALETTE_COUNT; i++) {
        bool active = (i == cfgPalette);
        lv_obj_t *b = lv_button_create(grid);
        lv_obj_set_size(b, 108, 36);
        lv_obj_set_style_bg_color(b, lv_color_hex(PALETTES[i].surface), 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_border_width(b, active ? 2 : 1, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(active ? PALETTES[i].accent : PALETTES[i].muted), 0);
        lv_obj_set_style_pad_all(b, 4, 0);
        lv_obj_add_event_cb(b, onPalette, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *sw = lv_obj_create(b);
        lv_obj_set_size(sw, 10, 10);
        lv_obj_set_style_bg_color(sw, lv_color_hex(PALETTES[i].accent), 0);
        lv_obj_set_style_border_width(sw, 0, 0);
        lv_obj_set_style_radius(sw, 0, 0);
        lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(sw, LV_ALIGN_LEFT_MID, 2, 0);

        lv_obj_t *lbl = lv_label_create(b);
        lv_label_set_text(lbl, PALETTES[i].name);
        lv_obj_set_width(lbl, 82);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(lbl, FONT_XS, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(PALETTES[i].text), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 16, 0);
    }
}

static void buildMedia(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);

    // Header row: MEDIA tag left, live [PLAYING]/[PAUSED] state right
    lv_obj_t *hdr = lv_label_create(p);
    lv_label_set_text(hdr, "MEDIA PLAYER");
    lv_obj_set_style_text_font(hdr, FONT_XS, 0);
    lv_obj_set_style_text_color(hdr, colAccent(), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 2, 0);

    lblMediaState = lv_label_create(p);
    lv_label_set_text(lblMediaState, "[ IDLE ]");
    lv_obj_set_style_text_font(lblMediaState, FONT_XS, 0);
    lv_obj_set_style_text_color(lblMediaState, colMuted(), 0);
    lv_obj_align(lblMediaState, LV_ALIGN_TOP_RIGHT, -2, 0);

    // Squared Now-Playing display card
    boxMediaCard = lv_obj_create(p);
    lv_obj_set_size(boxMediaCard, 220, 94);
    lv_obj_align(boxMediaCard, LV_ALIGN_CENTER, 0, -34);
    lv_obj_set_style_bg_color(boxMediaCard, colSurface(), 0);
    lv_obj_set_style_border_width(boxMediaCard, 1, 0);
    lv_obj_set_style_border_color(boxMediaCard, colMuted(), 0);
    lv_obj_set_style_radius(boxMediaCard, 0, 0);
    lv_obj_set_style_pad_all(boxMediaCard, 8, 0);
    lv_obj_set_scrollbar_mode(boxMediaCard, LV_SCROLLBAR_MODE_OFF);

    lblMediaTrack = lv_label_create(boxMediaCard);
    lv_obj_set_width(lblMediaTrack, 200);
    lv_label_set_long_mode(lblMediaTrack, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(lblMediaTrack, FONT_M, 0);
    lv_obj_set_style_text_color(lblMediaTrack, colText(), 0);
    lv_obj_align(lblMediaTrack, LV_ALIGN_TOP_LEFT, 0, 2);
    lv_label_set_text(lblMediaTrack, "No track playing");

    lblMediaArtist = lv_label_create(boxMediaCard);
    lv_obj_set_width(lblMediaArtist, 200);
    lv_label_set_long_mode(lblMediaArtist, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lblMediaArtist, FONT_S, 0);
    lv_obj_set_style_text_color(lblMediaArtist, colAccent(), 0);
    lv_obj_align(lblMediaArtist, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_label_set_text(lblMediaArtist, "Phone / PC BLE");

    lblMediaAlbum = lv_label_create(boxMediaCard);
    lv_obj_set_width(lblMediaAlbum, 200);
    lv_label_set_long_mode(lblMediaAlbum, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lblMediaAlbum, FONT_XS, 0);
    lv_obj_set_style_text_color(lblMediaAlbum, colMuted(), 0);
    lv_obj_align(lblMediaAlbum, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_label_set_text(lblMediaAlbum, "Start music on phone or PC");

    lv_obj_t *rule = lv_obj_create(boxMediaCard);
    lv_obj_set_size(rule, 200, 2);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(rule, colAccent(), 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);

    // Transport buttons: PREV | PLAY/PAUSE | NEXT
    mkButton(p, LV_SYMBOL_PREV, -76, 44, 64, 38, [](lv_event_t *) {
        blecomp::sendMusicCmd("previous");
    }, nullptr);

    btnMediaPlay = lv_button_create(p);
    lv_obj_set_size(btnMediaPlay, 78, 38);
    lv_obj_align(btnMediaPlay, LV_ALIGN_CENTER, 0, 44);
    lv_obj_set_style_bg_color(btnMediaPlay, colSurface(), 0);
    lv_obj_set_style_border_width(btnMediaPlay, 2, 0);
    lv_obj_set_style_border_color(btnMediaPlay, colAccent(), 0);
    lv_obj_set_style_radius(btnMediaPlay, 0, 0);
    lv_obj_set_style_shadow_width(btnMediaPlay, 0, 0);
    lv_obj_add_event_cb(btnMediaPlay, [](lv_event_t *) {
        blecomp::sendMusicCmd(blecomp::musicPlaying() ? "pause" : "play");
        updateMediaUI();
    }, LV_EVENT_CLICKED, nullptr);
    lblMediaPlay = lv_label_create(btnMediaPlay);
    lv_label_set_text(lblMediaPlay, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(lblMediaPlay, FONT_M, 0);
    lv_obj_set_style_text_color(lblMediaPlay, colAccent(), 0);
    lv_obj_center(lblMediaPlay);

    mkButton(p, LV_SYMBOL_NEXT, 76, 44, 64, 38, [](lv_event_t *) {
        blecomp::sendMusicCmd("next");
    }, nullptr);

    // Volume row centered cleanly between bottom < and > nav buttons
    mkButton(p, "vol -", -38, 94, 66, 26, [](lv_event_t *) {
        blecomp::sendMusicCmd("volumedown");
    }, nullptr);
    mkButton(p, "vol +", 38, 94, 66, 26, [](lv_event_t *) {
        blecomp::sendMusicCmd("volumeup");
    }, nullptr);
}

static void buildNav(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);

    lblNavHdr = lv_label_create(p);
    lv_label_set_text(lblNavHdr, "NAV");
    lv_obj_set_style_text_font(lblNavHdr, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNavHdr, colAccent(), 0);
    lv_obj_align(lblNavHdr, LV_ALIGN_TOP_LEFT, 2, 0);

    lblNavSpeed = lv_label_create(p);
    lv_label_set_text(lblNavSpeed, "0 km/h");
    lv_obj_set_style_text_font(lblNavSpeed, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNavSpeed, colText(), 0);
    lv_obj_align(lblNavSpeed, LV_ALIGN_TOP_MID, -6, 0);

    lblNavEta = lv_label_create(p);
    lv_label_set_text(lblNavEta, "[ IDLE ]");
    lv_obj_set_style_text_font(lblNavEta, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNavEta, colMuted(), 0);
    lv_obj_align(lblNavEta, LV_ALIGN_TOP_RIGHT, -2, 0);

    // Squared 78x78 vector arrow box (top-left below header)
    boxNavArrow = lv_obj_create(p);
    lv_obj_set_size(boxNavArrow, 78, 78);
    lv_obj_align(boxNavArrow, LV_ALIGN_TOP_LEFT, 2, 20);
    lv_obj_set_style_bg_color(boxNavArrow, colSurface(), 0);
    lv_obj_set_style_border_width(boxNavArrow, 1, 0);
    lv_obj_set_style_border_color(boxNavArrow, colMuted(), 0);
    lv_obj_set_style_radius(boxNavArrow, 0, 0);
    lv_obj_set_style_pad_all(boxNavArrow, 0, 0);
    lv_obj_set_scrollbar_mode(boxNavArrow, LV_SCROLLBAR_MODE_OFF);

    lineNavStem = lv_line_create(boxNavArrow);
    lv_line_set_points(lineNavStem, gNavStemPts, 4);
    lv_obj_set_style_line_width(lineNavStem, 6, 0);
    lv_obj_set_style_line_color(lineNavStem, colAccent(), 0);
    lv_obj_set_style_line_rounded(lineNavStem, false, 0);

    lineNavHead = lv_line_create(boxNavArrow);
    lv_line_set_points(lineNavHead, gNavHeadPts, 3);
    lv_obj_set_style_line_width(lineNavHead, 6, 0);
    lv_obj_set_style_line_color(lineNavHead, colAccent(), 0);
    lv_obj_set_style_line_rounded(lineNavHead, false, 0);

    // Distance + Action tag on top-right beside the arrow box
    lblNavDist = lv_label_create(p);
    lv_obj_set_width(lblNavDist, 134);
    lv_label_set_long_mode(lblNavDist, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lblNavDist, FONT_M, 0);
    lv_obj_set_style_text_color(lblNavDist, colText(), 0);
    lv_obj_align(lblNavDist, LV_ALIGN_TOP_LEFT, 88, 30);
    lv_label_set_text(lblNavDist, "--");

    lblNavAction = lv_label_create(p);
    lv_obj_set_width(lblNavAction, 134);
    lv_label_set_long_mode(lblNavAction, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lblNavAction, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNavAction, colAccent(), 0);
    lv_obj_align(lblNavAction, LV_ALIGN_TOP_LEFT, 88, 58);
    lv_label_set_text(lblNavAction, "READY");

    // Compact 2-line street / instruction card
    lv_obj_t *ibox = lv_obj_create(p);
    lv_obj_set_size(ibox, 220, 46);
    lv_obj_align(ibox, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_bg_color(ibox, colSurface(), 0);
    lv_obj_set_style_border_width(ibox, 1, 0);
    lv_obj_set_style_border_color(ibox, colMuted(), 0);
    lv_obj_set_style_radius(ibox, 0, 0);
    lv_obj_set_style_pad_all(ibox, 4, 0);
    lv_obj_set_scrollbar_mode(ibox, LV_SCROLLBAR_MODE_OFF);

    lblNavInstr = lv_label_create(ibox);
    lv_obj_set_width(lblNavInstr, 208);
    lv_label_set_long_mode(lblNavInstr, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lblNavInstr, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNavInstr, colText(), 0);
    lv_label_set_text(lblNavInstr, "Start OsmAnd or Maps on phone");

    // Combined Bike HUD music strip (tap to open Media Player)
    lv_obj_t *mstrip = lv_button_create(p);
    lv_obj_set_size(mstrip, 220, 22);
    lv_obj_align(mstrip, LV_ALIGN_TOP_MID, 0, 154);
    lv_obj_set_style_bg_color(mstrip, colSurface(), 0);
    lv_obj_set_style_border_width(mstrip, 1, 0);
    lv_obj_set_style_border_color(mstrip, colMuted(), 0);
    lv_obj_set_style_radius(mstrip, 0, 0);
    lv_obj_set_style_shadow_width(mstrip, 0, 0);
    lv_obj_set_style_pad_hor(mstrip, 6, 0);
    lv_obj_add_event_cb(mstrip, [](lv_event_t *) { showScreen(SC_MEDIA); }, LV_EVENT_CLICKED, nullptr);

    lblNavMusic = lv_label_create(mstrip);
    lv_obj_set_width(lblNavMusic, 206);
    lv_label_set_long_mode(lblNavMusic, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(lblNavMusic, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNavMusic, colMuted(), 0);
    lv_obj_align(lblNavMusic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(lblNavMusic, "music: idle");

    // Bottom bar between < and > arrows: RIDE toggle | Clear
    lv_obj_t *bRide = lv_button_create(p);
    lv_obj_set_size(bRide, 74, 24);
    lv_obj_align(bRide, LV_ALIGN_CENTER, -38, 94);
    lv_obj_set_style_bg_color(bRide, colSurface(), 0);
    lv_obj_set_style_border_width(bRide, 1, 0);
    lv_obj_set_style_border_color(bRide, colAccent(), 0);
    lv_obj_set_style_radius(bRide, 0, 0);
    lv_obj_set_style_shadow_width(bRide, 0, 0);
    lv_obj_add_event_cb(bRide, [](lv_event_t *) {
        gRideMode = !gRideMode;
        if (gRideMode) gRideStartMs = millis();
        instance.vibrator();
        updateNavUI();
    }, LV_EVENT_CLICKED, nullptr);
    lblRideBtn = lv_label_create(bRide);
    lv_label_set_text(lblRideBtn, gRideMode ? "ride:ON" : "ride:OFF");
    lv_obj_set_style_text_font(lblRideBtn, FONT_XS, 0);
    lv_obj_set_style_text_color(lblRideBtn, colText(), 0);
    lv_obj_center(lblRideBtn);

    mkButton(p, "clear", 38, 94, 68, 24, [](lv_event_t *) {
        if (gRideMode) return;  // guard against accidental handlebar taps while riding
        blecomp::setNavLocal(false, "", "", "", "");
        updateNavUI();
    }, nullptr);
}

static void buildNotif(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 5, 0);

    lv_obj_t *hdrRow = lv_obj_create(p);
    lv_obj_set_size(hdrRow, 224, 22);
    lv_obj_set_style_bg_opa(hdrRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdrRow, 0, 0);
    lv_obj_set_style_pad_all(hdrRow, 0, 0);
    lv_obj_set_flex_flow(hdrRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdrRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *hdr = lv_label_create(hdrRow);
    lv_label_set_text(hdr, "MSGS");
    lv_obj_set_style_text_font(hdr, FONT_XS, 0);
    lv_obj_set_style_text_color(hdr, colAccent(), 0);

    lblNotifTag = lv_label_create(hdrRow);
    lv_label_set_text(lblNotifTag, "[ IDLE ]");
    lv_obj_set_style_text_font(lblNotifTag, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNotifTag, colMuted(), 0);

    mkButton(hdrRow, "dismiss", 0, 0, 62, 20, [](lv_event_t *) {
        char s[20], t[36], b[80];
        long id = 0; bool rep = false;
        blecomp::latestNotif(s, sizeof(s), t, sizeof(t), b, sizeof(b), &id, &rep);
        blecomp::sendNotifDismiss(id);
        blecomp::clearUnread();
        if (lblNotifTag) lv_label_set_text(lblNotifTag, "[ CLEARED ]");
    }, nullptr);

    lv_obj_t *card = lv_obj_create(p);
    lv_obj_set_size(card, 224, 68);
    lv_obj_set_style_bg_color(card, colSurface(), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, colAccent(), 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, 5, 0);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);

    lblNotifSrc = lv_label_create(card);
    lv_obj_set_width(lblNotifSrc, 210);
    lv_label_set_long_mode(lblNotifSrc, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(lblNotifSrc, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNotifSrc, colAccent(), 0);
    lv_label_set_text(lblNotifSrc, "No messages");

    lblNotifBody = lv_label_create(card);
    lv_obj_set_width(lblNotifBody, 210);
    lv_label_set_long_mode(lblNotifBody, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lblNotifBody, FONT_XS, 0);
    lv_obj_set_style_text_color(lblNotifBody, colText(), 0);
    lv_label_set_text(lblNotifBody, "WhatsApp/S21 messages appear here.");

    // 6 PT-BR Quick Replies (2 cols x 3 rows)
    lv_obj_t *grid = lv_obj_create(p);
    lv_obj_set_size(grid, 224, 102);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 5, 0);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);

    static const char *kQuickReplies[6] = {
        "OK", "To indo", "Ja chego", "Sim", "Nao", "Te ligo"
    };
    for (int i = 0; i < 6; i++) {
        mkButton(grid, kQuickReplies[i], 0, 0, 108, 30, [](lv_event_t *e) {
            const char *msg = (const char *)lv_event_get_user_data(e);
            char s[20], t[36], b[80];
            long id = 0; bool rep = false;
            blecomp::latestNotif(s, sizeof(s), t, sizeof(t), b, sizeof(b), &id, &rep);
            bool sent = blecomp::sendNotifReply(id, msg);
            if (lblNotifTag) {
                lv_label_set_text(lblNotifTag, sent ? "[ SENT! ]" : "[ NO BLE ]");
                lv_obj_set_style_text_color(lblNotifTag, sent ? colAccent() : colMuted(), 0);
            }
            instance.vibrator();
        }, (void *)kQuickReplies[i]);
    }
}

static void buildBle(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);

    lv_obj_t *hdr = lv_label_create(p);
    lv_label_set_text(hdr, "BLE DEVICES");
    lv_obj_set_style_text_font(hdr, FONT_XS, 0);
    lv_obj_set_style_text_color(hdr, colAccent(), 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 2, 0);

    lblBleState = lv_label_create(p);
    lv_label_set_text(lblBleState, "adv | 0 peers");
    lv_obj_set_style_text_font(lblBleState, FONT_XS, 0);
    lv_obj_set_style_text_color(lblBleState, colMuted(), 0);
    lv_obj_align(lblBleState, LV_ALIGN_TOP_RIGHT, -2, 0);

    lv_obj_t *box = lv_obj_create(p);
    lv_obj_set_size(box, 220, 98);
    lv_obj_align(box, LV_ALIGN_CENTER, 0, -33);
    lv_obj_set_style_bg_color(box, colSurface(), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, colMuted(), 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 3, 0);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

    lblBleInbox = lv_label_create(box);
    lv_obj_set_width(lblBleInbox, 208);
    lv_label_set_long_mode(lblBleInbox, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lblBleInbox, FONT_XS, 0);
    lv_obj_set_style_text_color(lblBleInbox, colMuted(), 0);
    lv_label_set_text(lblBleInbox, "No devices.\nTap [pair] -> 'Bangle.js oma'");

    for (int i = 0; i < 3; i++) {
        rowBlePeer[i] = lv_obj_create(box);
        lv_obj_set_size(rowBlePeer[i], 210, 26);
        lv_obj_set_style_bg_color(rowBlePeer[i], colBg(), 0);
        lv_obj_set_style_border_width(rowBlePeer[i], 1, 0);
        lv_obj_set_style_border_color(rowBlePeer[i], colMuted(), 0);
        lv_obj_set_style_radius(rowBlePeer[i], 0, 0);
        lv_obj_set_style_pad_hor(rowBlePeer[i], 4, 0);
        lv_obj_set_style_pad_ver(rowBlePeer[i], 0, 0);
        lv_obj_set_scrollbar_mode(rowBlePeer[i], LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(rowBlePeer[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(rowBlePeer[i], LV_OBJ_FLAG_HIDDEN);

        lblBlePeer[i] = lv_label_create(rowBlePeer[i]);
        lv_obj_set_width(lblBlePeer[i], 164);
        lv_label_set_long_mode(lblBlePeer[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(lblBlePeer[i], FONT_XS, 0);
        lv_obj_set_style_text_color(lblBlePeer[i], colText(), 0);
        lv_obj_align(lblBlePeer[i], LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *rm = lv_button_create(rowBlePeer[i]);
        lv_obj_set_size(rm, 28, 22);
        lv_obj_align(rm, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(rm, colSurface(), 0);
        lv_obj_set_style_border_width(rm, 1, 0);
        lv_obj_set_style_border_color(rm, colAccent(), 0);
        lv_obj_set_style_radius(rm, 0, 0);
        lv_obj_set_style_shadow_width(rm, 0, 0);
        lv_obj_add_event_cb(rm, [](lv_event_t *e) {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            if (idx >= 0 && idx < 3 && gBlePeerAddr[idx][0]) {
                blecomp::removePeer(gBlePeerAddr[idx]);
                instance.vibrator();
                updateBleUI();
                updateWatchLinkUI();
            }
        }, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *xl = lv_label_create(rm);
        lv_label_set_text(xl, "X");
        lv_obj_set_style_text_font(xl, FONT_XS, 0);
        lv_obj_set_style_text_color(xl, colAccent(), 0);
        lv_obj_center(xl);
    }

    // Row 1: Radio ON/OFF | Pair (advertise) | Ping Phone
    lv_obj_t *btnTog = lv_button_create(p);
    lv_obj_set_size(btnTog, 70, 30);
    lv_obj_align(btnTog, LV_ALIGN_CENTER, -75, 36);
    lv_obj_set_style_bg_color(btnTog, colSurface(), 0);
    lv_obj_set_style_border_width(btnTog, 1, 0);
    lv_obj_set_style_border_color(btnTog, colAccent(), 0);
    lv_obj_set_style_radius(btnTog, 0, 0);
    lv_obj_set_style_shadow_width(btnTog, 0, 0);
    lv_obj_add_event_cb(btnTog, [](lv_event_t *) {
        blecomp::setEnabled(!blecomp::enabled());
        updateBleUI();
        updateWatchLinkUI();
    }, LV_EVENT_CLICKED, nullptr);
    lblBleToggle = lv_label_create(btnTog);
    lv_label_set_text(lblBleToggle, blecomp::enabled() ? "ble:ON" : "ble:OFF");
    lv_obj_set_style_text_font(lblBleToggle, FONT_XS, 0);
    lv_obj_set_style_text_color(lblBleToggle, colText(), 0);
    lv_obj_center(lblBleToggle);

    mkButton(p, "pair", 0, 36, 70, 30, [](lv_event_t *) {
        blecomp::reAdvertise();
        instance.vibrator();
        updateBleUI();
    }, nullptr);

    mkButton(p, "ping", 75, 36, 70, 30, [](lv_event_t *) {
        blecomp::sendFindPhone(true);
    }, nullptr);

    // Row 2 (between < and > nav arrows): Clear Inbox | Media >>
    mkButton(p, "clear", -38, 86, 68, 28, [](lv_event_t *) {
        blecomp::clearUnread();
        updateBleUI();
        updateWatchLinkUI();
    }, nullptr);

    mkButton(p, "media>", 38, 86, 68, 28, [](lv_event_t *) {
        showScreen(SC_MEDIA);
    }, nullptr);
}

static void buildSteps(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);
    mkTag(p, "STEPS", 0, -94);
    lblStepsBig = mkLabel(p, "0", FONT_XL, PALETTES[cfgPalette].text, 0, -16);
    mkLabel(p, "today", FONT_S, PALETTES[cfgPalette].muted, 0, 26);
    mkButton(p, "reset", 0, 74, 92, 30, [](lv_event_t *) {
        instance.resetStepCounter();
        if (lblStepsBig) lv_label_set_text(lblStepsBig, "0");
        updateWatchLinkUI();
    }, nullptr);
}

static const char kMeshQuick[3][12] = {"hi", "ok?", "on my way"};

static void onMeshSend(lv_event_t *e)
{
    const char *text = (const char *)lv_event_get_user_data(e);
    if (text && meshsvc::sendText(text, mesh::kBroadcast)) instance.vibrator();
    updateMeshUI();
}

static void buildMesh(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 3, 0);

    mkTag(p, "LORA MESH", 0, 0);
    lblMeshId    = mkLabel(p, "!--------", FONT_XS, PALETTES[cfgPalette].accent, 0, 0);
    lblMeshState = mkLabel(p, "off", FONT_XS, PALETTES[cfgPalette].muted, 0, 0);
    lblMeshStats = mkLabel(p, "rx 0  tx 0", FONT_XS, PALETTES[cfgPalette].muted, 0, 0);

    lblMeshNodes = mkLabel(p, "nobody heard yet", FONT_XS, PALETTES[cfgPalette].text, 0, 0);
    lv_obj_set_style_text_align(lblMeshNodes, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(lblMeshNodes, 216);

    lblMeshLast = mkLabel(p, "", FONT_XS, PALETTES[cfgPalette].accent, 0, 0);
    lv_obj_set_width(lblMeshLast, 216);
    lv_label_set_long_mode(lblMeshLast, LV_LABEL_LONG_WRAP);

    lv_obj_t *row = lv_obj_create(p);
    lv_obj_set_size(row, 216, 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 4, 0);
    for (int i = 0; i < 3; i++)
        mkButton(row, kMeshQuick[i], 0, 0, 68, 26, onMeshSend, (void *)kMeshQuick[i]);

    lv_obj_t *crow = lv_obj_create(p);
    lv_obj_set_size(crow, 216, 26);
    lv_obj_set_style_bg_opa(crow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(crow, 0, 0);
    lv_obj_set_style_pad_all(crow, 0, 0);
    lv_obj_set_flex_flow(crow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(crow, 4, 0);
    mkButton(crow, "region", 0, 0, 104, 24, [](lv_event_t *) {
        meshsvc::setRegion((meshsvc::regionIndex() + 1) % meshsvc::kRegionCount);
        instance.vibrator();
        updateMeshUI();
    }, nullptr);
    mkButton(crow, "radio on/off", 0, 0, 108, 24, [](lv_event_t *) {
        meshsvc::setEnabled(!meshsvc::enabled());
        instance.vibrator();
        updateMeshUI();
        updateWatchLinkUI();
    }, nullptr);
}

static void buildSettings(lv_obj_t *scr)
{
    lv_obj_t *p = mkPage(scr);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 7, 0);

    mkTag(p, "SETTINGS", 0, 0);

    char tbuf[40];
    snprintf(tbuf, sizeof(tbuf), "scheme: %s  [pick]", PALETTES[cfgPalette].name);
    mkButton(p, tbuf, 0, 0, 216, 30, [](lv_event_t *) {
        showScreen(SC_THEMES);
    }, nullptr);

    lv_obj_t *rrow = lv_obj_create(p);
    lv_obj_set_size(rrow, 216, 30);
    lv_obj_set_style_bg_opa(rrow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rrow, 0, 0);
    lv_obj_set_style_pad_all(rrow, 0, 0);
    lv_obj_set_flex_flow(rrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(rrow, 6, 0);

    char bleLbl[20], meshLbl[20];
    snprintf(bleLbl,  sizeof(bleLbl),  "ble: %s",  blecomp::enabled() ? "ON" : "OFF");
    snprintf(meshLbl, sizeof(meshLbl), "mesh: %s", meshsvc::enabled() ? "ON" : "OFF");
    mkButton(rrow, bleLbl, 0, 0, 105, 28, [](lv_event_t *) {
        blecomp::setEnabled(!blecomp::enabled());
        buildUI();
    }, nullptr);
    mkButton(rrow, meshLbl, 0, 0, 105, 28, [](lv_event_t *) {
        meshsvc::setEnabled(!meshsvc::enabled());
        buildUI();
    }, nullptr);

    lv_obj_t *brow = lv_obj_create(p);
    lv_obj_set_size(brow, 216, 28);
    lv_obj_set_style_bg_opa(brow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brow, 0, 0);
    lv_obj_set_style_pad_all(brow, 0, 0);
    lv_obj_set_flex_flow(brow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sl = lv_slider_create(brow);
    lv_obj_set_width(sl, 152);
    lv_slider_set_range(sl, 10, 255);
    lv_slider_set_value(sl, cfgBright, LV_ANIM_OFF);
    styleSquareSlider(sl);
    lv_obj_add_event_cb(sl, onBright, LV_EVENT_VALUE_CHANGED, nullptr);
    lblBrightVal = lv_label_create(brow);
    lv_label_set_text_fmt(lblBrightVal, "%d", cfgBright);
    lv_obj_set_style_text_color(lblBrightVal, colText(), 0);

    lv_obj_t *trow = lv_obj_create(p);
    lv_obj_set_size(trow, 216, 28);
    lv_obj_set_style_bg_opa(trow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(trow, 0, 0);
    lv_obj_set_style_pad_all(trow, 0, 0);
    lv_obj_set_flex_flow(trow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(trow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sl2 = lv_slider_create(trow);
    lv_obj_set_width(sl2, 152);
    lv_slider_set_range(sl2, 5, 120);
    lv_slider_set_value(sl2, cfgTimeoutS, LV_ANIM_OFF);
    styleSquareSlider(sl2);
    lv_obj_add_event_cb(sl2, onTimeout, LV_EVENT_VALUE_CHANGED, nullptr);
    lblTimeoutVal = lv_label_create(trow);
    lv_label_set_text_fmt(lblTimeoutVal, "%ds", cfgTimeoutS);
    lv_obj_set_style_text_color(lblTimeoutVal, colText(), 0);

    lblAbout = mkLabel(p, "omarchy watch | wifi 0mA", FONT_XS, PALETTES[cfgPalette].muted, 0, 0);
}

// ----------------------------------------------------------------- UI updaters
static void updateWatchLinkUI()
{
    if (lblLinkFace) {
        if (!blecomp::enabled()) {
            lv_label_set_text_fmt(lblLinkFace, "ble off%s", meshsvc::enabled() ? " | mesh" : "");
            lv_obj_set_style_text_color(lblLinkFace, colMuted(), 0);
        } else if (blecomp::connected()) {
            lv_label_set_text_fmt(lblLinkFace, "* ble (%d)%s", blecomp::peerCount(), meshsvc::enabled() ? " | mesh" : "");
            lv_obj_set_style_text_color(lblLinkFace, colAccent(), 0);
        } else {
            lv_label_set_text_fmt(lblLinkFace, "o ble adv%s", meshsvc::enabled() ? " | mesh" : "");
            lv_obj_set_style_text_color(lblLinkFace, colMuted(), 0);
        }
    }
    if (lblStepsFace) {
        char line[128] = {0};
        blecomp::latestLine(line, sizeof(line));
        if (line[0]) {
            lv_label_set_text(lblStepsFace, line);
            lv_obj_set_style_text_color(lblStepsFace, colAccent(), 0);
        } else {
            lv_label_set_text_fmt(lblStepsFace, "%lu steps | %s",
                                  (unsigned long)instance.getStepCounter(),
                                  PALETTES[cfgPalette].name);
            lv_obj_set_style_text_color(lblStepsFace, colMuted(), 0);
        }
    }
}

static void updateMediaUI()
{
    char track[80] = {0}, artist[56] = {0}, album[56] = {0};
    bool playing = false;
    blecomp::mediaSnapshot(track, sizeof(track), artist, sizeof(artist), album, sizeof(album), &playing);

    if (lblMediaState) {
        if (playing) {
            lv_label_set_text(lblMediaState, "[ PLAYING ]");
            lv_obj_set_style_text_color(lblMediaState, colAccent(), 0);
        } else if (track[0]) {
            lv_label_set_text(lblMediaState, "[ PAUSED ]");
            lv_obj_set_style_text_color(lblMediaState, colMuted(), 0);
        } else {
            lv_label_set_text(lblMediaState, "[ IDLE ]");
            lv_obj_set_style_text_color(lblMediaState, colMuted(), 0);
        }
    }
    if (boxMediaCard) {
        lv_obj_set_style_border_color(boxMediaCard, playing ? colAccent() : colMuted(), 0);
    }
    if (lblMediaTrack) {
        lv_label_set_text(lblMediaTrack, track[0] ? track : "No track playing");
    }
    if (lblMediaArtist) {
        lv_label_set_text(lblMediaArtist, artist[0] ? artist : (blecomp::connected() ? "Connected via BLE" : "Waiting for BLE..."));
    }
    if (lblMediaAlbum) {
        if (album[0]) lv_label_set_text(lblMediaAlbum, album);
        else          lv_label_set_text_fmt(lblMediaAlbum, "peers: %d | tap play or skip", blecomp::peerCount());
    }
    if (lblMediaPlay) {
        lv_label_set_text(lblMediaPlay, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

static void setNavArrowGeometry(const char *action)
{
    String a(action ? action : "");
    a.toLowerCase();
    if (a.indexOf("uturn") >= 0) {
        gNavStemPts[0] = {54, 64}; gNavStemPts[1] = {54, 22}; gNavStemPts[2] = {26, 22}; gNavStemPts[3] = {26, 56};
        gNavHeadPts[0] = {14, 44}; gNavHeadPts[1] = {26, 56}; gNavHeadPts[2] = {38, 44};
    } else if (a.indexOf("roundabout") >= 0) {
        gNavStemPts[0] = {40, 66}; gNavStemPts[1] = {40, 46}; gNavStemPts[2] = {56, 32}; gNavStemPts[3] = {56, 16};
        gNavHeadPts[0] = {44, 26}; gNavHeadPts[1] = {56, 16}; gNavHeadPts[2] = {66, 28};
    } else if (a.indexOf("slight_right") >= 0 || a.indexOf("fork_right") >= 0) {
        gNavStemPts[0] = {28, 66}; gNavStemPts[1] = {28, 44}; gNavStemPts[2] = {56, 18}; gNavStemPts[3] = {56, 18};
        gNavHeadPts[0] = {40, 18}; gNavHeadPts[1] = {56, 18}; gNavHeadPts[2] = {56, 34};
    } else if (a.indexOf("slight_left") >= 0 || a.indexOf("fork_left") >= 0) {
        gNavStemPts[0] = {52, 66}; gNavStemPts[1] = {52, 44}; gNavStemPts[2] = {24, 18}; gNavStemPts[3] = {24, 18};
        gNavHeadPts[0] = {40, 18}; gNavHeadPts[1] = {24, 18}; gNavHeadPts[2] = {24, 34};
    } else if (a.indexOf("right") >= 0) {
        gNavStemPts[0] = {26, 66}; gNavStemPts[1] = {26, 30}; gNavStemPts[2] = {62, 30}; gNavStemPts[3] = {62, 30};
        gNavHeadPts[0] = {48, 16}; gNavHeadPts[1] = {62, 30}; gNavHeadPts[2] = {48, 44};
    } else if (a.indexOf("left") >= 0) {
        gNavStemPts[0] = {54, 66}; gNavStemPts[1] = {54, 30}; gNavStemPts[2] = {18, 30}; gNavStemPts[3] = {18, 30};
        gNavHeadPts[0] = {32, 16}; gNavHeadPts[1] = {18, 30}; gNavHeadPts[2] = {32, 44};
    } else if (a.indexOf("finish") >= 0 || a.indexOf("dest") >= 0) {
        gNavStemPts[0] = {40, 66}; gNavStemPts[1] = {40, 34}; gNavStemPts[2] = {40, 34}; gNavStemPts[3] = {40, 34};
        gNavHeadPts[0] = {24, 20}; gNavHeadPts[1] = {40, 34}; gNavHeadPts[2] = {56, 20};
    } else {
        // Straight / continue / idle
        gNavStemPts[0] = {40, 66}; gNavStemPts[1] = {40, 16}; gNavStemPts[2] = {40, 16}; gNavStemPts[3] = {40, 16};
        gNavHeadPts[0] = {24, 32}; gNavHeadPts[1] = {40, 16}; gNavHeadPts[2] = {56, 32};
    }
    if (lineNavStem) lv_line_set_points(lineNavStem, gNavStemPts, 4);
    if (lineNavHead) lv_line_set_points(lineNavHead, gNavHeadPts, 3);
}

static void updateNavUI()
{
    char action[24] = {0}, dist[24] = {0}, instr[96] = {0}, eta[20] = {0};
    bool active = false;
    blecomp::navSnapshot(action, sizeof(action), dist, sizeof(dist), instr, sizeof(instr), eta, sizeof(eta), &active);

    setNavArrowGeometry(action);

    if (boxNavArrow) {
        lv_obj_set_style_border_color(boxNavArrow, active ? colAccent() : colMuted(), 0);
    }
    if (lblNavEta) {
        if (active && eta[0]) lv_label_set_text_fmt(lblNavEta, "ETA %s", eta);
        else if (active)      lv_label_set_text(lblNavEta, "[ ACTIVE ]");
        else                  lv_label_set_text(lblNavEta, "[ IDLE ]");
        lv_obj_set_style_text_color(lblNavEta, active ? colAccent() : colMuted(), 0);
    }
    if (lblNavDist) {
        lv_label_set_text(lblNavDist, (active && dist[0]) ? dist : "--");
    }
    if (lblNavAction) {
        if (active && action[0]) {
            String act(action);
            act.replace("_", " ");
            act.toUpperCase();
            lv_label_set_text(lblNavAction, act.c_str());
        } else {
            lv_label_set_text(lblNavAction, active ? "CONTINUE" : "READY");
        }
    }
    if (lblNavInstr) {
        lv_label_set_text(lblNavInstr, (active && instr[0]) ? instr : "Start OsmAnd or Google Maps on S21");
    }
    if (lblNavSpeed) {
        if (blecomp::gpsValid()) {
            lv_label_set_text_fmt(lblNavSpeed, "%d km/h", (int)(blecomp::gpsSpeedKmh() + 0.5f));
            lv_obj_set_style_text_color(lblNavSpeed, colText(), 0);
        } else if (gRideMode) {
            uint32_t mins = (millis() - gRideStartMs) / 60000UL;
            lv_label_set_text_fmt(lblNavSpeed, "ride %lum", (unsigned long)mins);
            lv_obj_set_style_text_color(lblNavSpeed, colAccent(), 0);
        } else {
            lv_label_set_text(lblNavSpeed, "-- km/h");
            lv_obj_set_style_text_color(lblNavSpeed, colMuted(), 0);
        }
    }
    if (lblNavMusic) {
        char tr[80] = {0}, ar[56] = {0}, al[56] = {0};
        bool pl = false;
        blecomp::mediaSnapshot(tr, sizeof(tr), ar, sizeof(ar), al, sizeof(al), &pl);
        if (tr[0]) {
            lv_label_set_text_fmt(lblNavMusic, "%s %s - %s", pl ? ">>" : "||", ar[0] ? ar : "Media", tr);
            lv_obj_set_style_text_color(lblNavMusic, pl ? colAccent() : colText(), 0);
        } else {
            lv_label_set_text(lblNavMusic, "music: idle (tap to open)");
            lv_obj_set_style_text_color(lblNavMusic, colMuted(), 0);
        }
    }
    if (lblNavHdr) {
        int pct = (int)instance.getBatteryPercent();
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_label_set_text_fmt(lblNavHdr, "%s %d%%", gRideMode ? "RIDE" : "NAV", pct);
    }
    if (lblRideBtn) {
        lv_label_set_text(lblRideBtn, gRideMode ? "ride:ON" : "ride:OFF");
        lv_obj_set_style_text_color(lblRideBtn, gRideMode ? colAccent() : colText(), 0);
    }
}

static void updateNotifUI()
{
    char src[20] = {0}, title[36] = {0}, body[80] = {0};
    long id = 0; bool canReply = false;
    bool has = blecomp::latestNotif(src, sizeof(src), title, sizeof(title), body, sizeof(body), &id, &canReply);
    if (lblNotifTag) {
        if (!has) {
            lv_label_set_text(lblNotifTag, "[ IDLE ]");
            lv_obj_set_style_text_color(lblNotifTag, colMuted(), 0);
        } else if (canReply || id != 0) {
            lv_label_set_text(lblNotifTag, "[ REPLY ]");
            lv_obj_set_style_text_color(lblNotifTag, colAccent(), 0);
        } else {
            lv_label_set_text(lblNotifTag, "[ READ ]");
            lv_obj_set_style_text_color(lblNotifTag, colMuted(), 0);
        }
    }
    if (lblNotifSrc) {
        if (has) lv_label_set_text_fmt(lblNotifSrc, "%s%s%s", src[0] ? src : "Msg", title[0] ? ": " : "", title);
        else     lv_label_set_text(lblNotifSrc, "No messages");
    }
    if (lblNotifBody) {
        if (has) lv_label_set_text(lblNotifBody, body[0] ? body : "(empty)");
        else     lv_label_set_text(lblNotifBody, "WhatsApp/S21 messages appear here.");
    }
}

static void updateBleUI()
{
    if (lblBleState) {
        if (!blecomp::enabled()) {
            lv_label_set_text(lblBleState, "off (0 mA)");
        } else if (blecomp::connected()) {
            lv_label_set_text_fmt(lblBleState, "%d peer%s", blecomp::peerCount(), blecomp::peerCount() == 1 ? "" : "s");
        } else {
            lv_label_set_text(lblBleState, "adv | 0 peers");
        }
        lv_obj_set_style_text_color(lblBleState, blecomp::connected() ? colAccent() : colMuted(), 0);
    }
    if (lblBleToggle) {
        lv_label_set_text(lblBleToggle, blecomp::enabled() ? "ble:ON" : "ble:OFF");
    }
    if (lblBleInbox) {
        blecomp::PeerEntry peers[3];
        int n = blecomp::listPeers(peers, 3);
        if (n == 0) {
            lv_obj_remove_flag(lblBleInbox, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(lblBleInbox, blecomp::enabled()
                ? "No devices.\nTap [pair] -> 'Bangle.js oma'"
                : "BLE is OFF (0 mA)");
            for (int i = 0; i < 3; i++) {
                gBlePeerAddr[i][0] = 0;
                if (rowBlePeer[i]) lv_obj_add_flag(rowBlePeer[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(lblBleInbox, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < 3; i++) {
                if (i < n && rowBlePeer[i] && lblBlePeer[i]) {
                    strncpy(gBlePeerAddr[i], peers[i].addr, sizeof(gBlePeerAddr[i]) - 1);
                    gBlePeerAddr[i][sizeof(gBlePeerAddr[i]) - 1] = 0;
                    lv_label_set_text_fmt(lblBlePeer[i], "%s %s",
                                          peers[i].connected ? "*" : "o",
                                          peers[i].label);
                    lv_obj_set_style_text_color(lblBlePeer[i], peers[i].connected ? colAccent() : colMuted(), 0);
                    lv_obj_remove_flag(rowBlePeer[i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    gBlePeerAddr[i][0] = 0;
                    if (rowBlePeer[i]) lv_obj_add_flag(rowBlePeer[i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
}

static void updateMeshUI()
{
    if (!lblMeshId) return;
    const meshsvc::Counters &c = meshsvc::counters();
    char id[16];
    snprintf(id, sizeof(id), "!%08lx", (unsigned long)meshsvc::selfNum());
    lv_label_set_text(lblMeshId, id);
    lv_label_set_text_fmt(lblMeshState, "%s  %s  %.3f MHz  ch %02x",
                          meshsvc::enabled() ? "on" : "off", meshsvc::regionName(),
                          (double)meshsvc::frequency(), meshsvc::channelHash());
    lv_label_set_text_fmt(lblMeshStats, "rx %lu  tx %lu  err %lu  nodes %lu",
                          (unsigned long)c.rxFrames, (unsigned long)c.tx,
                          (unsigned long)c.crcErrors, (unsigned long)meshsvc::nodes().count());

    mesh::Node list[3];
    const size_t n = meshsvc::nodes().snapshot(list, 3);
    char text[190];
    if (!n) {
        snprintf(text, sizeof(text), "nobody heard yet");
    } else {
        size_t o = 0;
        for (size_t i = 0; i < n && o < sizeof(text) - 1; i++) {
            char name[24];
            mesh::NodeDb::displayName(list[i], name, sizeof(name));
            const uint32_t ago = (millis() / 1000UL) - list[i].lastHeardS;
            o += (size_t)snprintf(text + o, sizeof(text) - o, "%s%s  %dh  %ddB  %lus",
                                  i ? "\n" : "", name, (int)list[i].hops, (int)list[i].snr,
                                  (unsigned long)ago);
        }
    }
    lv_label_set_text(lblMeshNodes, text);

    const mesh::Msg *m = meshsvc::inbox().newest();
    if (m) {
        char who[24] = "me";
        if (!m->outgoing) {
            const mesh::Node *nd = meshsvc::nodes().find(m->from);
            if (nd) mesh::NodeDb::displayName(*nd, who, sizeof(who));
            else    mesh::NodeDb::hexId(m->from, who, sizeof(who));
        }
        char line[220];
        snprintf(line, sizeof(line), "%s %s: %s", m->outgoing ? "->" : "<-", who, m->text);
        lv_label_set_text(lblMeshLast, line);
    } else {
        lv_label_set_text(lblMeshLast, "");
    }
}

static void updateClock()
{
    time_t utc = rtcNowUtc();
    if (utc == 0) return;
    utc += (time_t)cfgTzOffsetMin * 60;
    struct tm t;
    tmFromUtc(utc, t);
    if (lblTime)    lv_label_set_text_fmt(lblTime, "%02d:%02d", t.tm_hour, t.tm_min);
    if (lblSeconds) lv_label_set_text_fmt(lblSeconds, ":%02d", t.tm_sec);
    if (lblDate) {
        static const char *wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char *mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        lv_label_set_text_fmt(lblDate, "%s %d %s", wd[t.tm_wday], t.tm_mday, mo[t.tm_mon]);
    }
}

static void updateBattery()
{
    float pct = instance.getBatteryPercent();
    if (pct < 0 || pct > 100) pct = 0;
    if (barBattery) lv_bar_set_value(barBattery, (int)pct, LV_ANIM_OFF);
    if (lblBattery) {
        if (instance.isCharging()) lv_label_set_text_fmt(lblBattery, "%d%%+", (int)pct);
        else                       lv_label_set_text_fmt(lblBattery, "%d%%", (int)pct);
    }
    if (lblNavHdr) {
        lv_label_set_text_fmt(lblNavHdr, "%s %d%%", gRideMode ? "RIDE" : "NAV", (int)pct);
    }
}

static void updateSteps()
{
    uint32_t s = instance.getStepCounter();
    if (lblStepsBig) lv_label_set_text_fmt(lblStepsBig, "%lu", (unsigned long)s);
    updateWatchLinkUI();
}

// ----------------------------------------------------------------- navigation
static void showScreen(int idx)
{
    if (idx < 0) idx = SC_COUNT - 1;
    if (idx >= SC_COUNT) idx = 0;
    curScreen = idx;
    for (int i = 0; i < SC_COUNT; i++) {
        if (!screens[i]) continue;
        if (i == idx) lv_obj_remove_flag(screens[i], LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_add_flag(screens[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void onNext(lv_event_t *) { showScreen(curScreen + 1); }
static void onPrev(lv_event_t *) { showScreen(curScreen - 1); }

static void pollSwipe()
{
    if (!gIndev) gIndev = lv_indev_get_next(nullptr);
    if (!gIndev) return;

    lv_indev_state_t st = lv_indev_get_state(gIndev);
    if (st == LV_INDEV_STATE_PRESSED) {
        if (!touchWasDown) {
            touchWasDown = true;
            lv_point_t pt;
            lv_indev_get_point(gIndev, &pt);
            touchX0 = pt.x;
            touchY0 = pt.y;
        }
        return;
    }
    if (!touchWasDown) return;
    touchWasDown = false;

    if (ateWakeTouch) {
        ateWakeTouch = false;
        return;
    }
    // In Ride Mode on SC_NAV, lock swipe & clear accidental-tap guards so bumps/gloves don't leave NAV
    if (gRideMode && curScreen == SC_NAV) return;
    if (!kSwipeOk[curScreen]) return;

    lv_point_t pt;
    lv_indev_get_point(gIndev, &pt);
    int16_t dx = (int16_t)(pt.x - touchX0);
    int16_t dy = (int16_t)(pt.y - touchY0);
    int16_t adx = dx < 0 ? (int16_t)-dx : dx;
    int16_t ady = dy < 0 ? (int16_t)-dy : dy;
    if (adx < 40 || ady > 30 || adx < ady) return;

    showScreen(curScreen + (dx < 0 ? 1 : -1));
}

// ----------------------------------------------------------------- snapshot & serial
static void dumpScreen(int face)
{
    if (face >= 0 && face < SC_COUNT) showScreen(face);
    for (int i = 0; i < 3; i++) { lv_timer_handler(); delay(20); }
    lv_obj_t *scr = lv_screen_active();
    lv_draw_buf_t *buf = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    if (!buf) { Serial.println("[snap] snapshot failed"); return; }
    const uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    Serial.printf("[snap] begin %lu %lu RGB565 %lu\n",
                  (unsigned long)w, (unsigned long)h, (unsigned long)(w * h * 2));
    Serial.flush();
    for (uint32_t y = 0; y < h; y++)
        Serial.write(buf->data + (size_t)y * stride, w * 2);
    Serial.flush();
    Serial.println("\n[snap] end");
    lv_free((void *)buf->data);
    lv_free(buf);
}

static void handleSerial()
{
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (serialBuf.length()) {
                if (serialBuf.startsWith("SNAP")) {
                    int face = -1;
                    if (serialBuf.length() > 5) face = atoi(serialBuf.c_str() + 5);
                    dumpScreen(face);
                } else if (serialBuf == "BLE" || serialBuf == "ble") {
                    char out[256];
                    blecomp::statusText(out, sizeof(out));
                    Serial.println(out);
                } else if (serialBuf == "M0") {
                    meshsvc::setEnabled(false);
                    buildUI();
                } else if (serialBuf == "M1") {
                    meshsvc::setEnabled(true);
                    buildUI();
                } else if (serialBuf.startsWith("MUS ")) {
                    String rest = serialBuf.substring(4);
                    int p1 = rest.indexOf('|');
                    int p2 = (p1 > 0) ? rest.indexOf('|', p1 + 1) : -1;
                    int p3 = (p2 > 0) ? rest.indexOf('|', p2 + 1) : -1;
                    if (p2 > 0) {
                        bool pl = (rest.substring(0, p1) == "play");
                        String ar = rest.substring(p1 + 1, p2);
                        String tr = (p3 > 0) ? rest.substring(p2 + 1, p3) : rest.substring(p2 + 1);
                        String al = (p3 > 0) ? rest.substring(p3 + 1) : String("");
                        blecomp::setMediaLocal(pl, ar.c_str(), tr.c_str(), al.c_str());
                        updateMediaUI();
                        updateWatchLinkUI();
                    }
                } else if (serialBuf.startsWith("NAV ")) {
                    String rest = serialBuf.substring(4);
                    if (rest == "stop" || rest == "clear") {
                        blecomp::setNavLocal(false, "", "", "", "");
                        updateNavUI();
                    } else {
                        int p1 = rest.indexOf('|');
                        int p2 = (p1 > 0) ? rest.indexOf('|', p1 + 1) : -1;
                        int p3 = (p2 > 0) ? rest.indexOf('|', p2 + 1) : -1;
                        if (p2 > 0) {
                            String ac = rest.substring(0, p1);
                            String di = rest.substring(p1 + 1, p2);
                            String in = (p3 > 0) ? rest.substring(p2 + 1, p3) : rest.substring(p2 + 1);
                            String et = (p3 > 0) ? rest.substring(p3 + 1) : String("");
                            blecomp::setNavLocal(true, ac.c_str(), di.c_str(), in.c_str(), et.c_str());
                            updateNavUI();
                        }
                    }
                } else if (serialBuf.startsWith("GPS ")) {
                    blecomp::setGpsSpeedLocal((float)atof(serialBuf.c_str() + 4));
                    updateNavUI();
                } else if (serialBuf.startsWith("RIDE ")) {
                    gRideMode = (serialBuf.substring(5) == "1" || serialBuf.substring(5) == "on");
                    if (gRideMode) gRideStartMs = millis();
                    updateNavUI();
                } else if (serialBuf.startsWith("TH ")) {
                    String rest = serialBuf.substring(3);
                    int p1 = rest.indexOf('|');
                    int p2 = (p1 > 0) ? rest.indexOf('|', p1 + 1) : -1;
                    int p3 = (p2 > 0) ? rest.indexOf('|', p2 + 1) : -1;
                    int p4 = (p3 > 0) ? rest.indexOf('|', p3 + 1) : -1;
                    int p5 = (p4 > 0) ? rest.indexOf('|', p4 + 1) : -1;
                    if (p5 > 0) {
                        blecomp::ThemePayload tp = {};
                        rest.substring(0, p1).toCharArray(tp.name, sizeof(tp.name));
                        tp.bg      = (uint32_t)strtoul(rest.substring(p1 + 1, p2).c_str(), nullptr, 16);
                        tp.surface = (uint32_t)strtoul(rest.substring(p2 + 1, p3).c_str(), nullptr, 16);
                        tp.text    = (uint32_t)strtoul(rest.substring(p3 + 1, p4).c_str(), nullptr, 16);
                        tp.muted   = (uint32_t)strtoul(rest.substring(p4 + 1, p5).c_str(), nullptr, 16);
                        tp.accent  = (uint32_t)strtoul(rest.substring(p5 + 1).c_str(), nullptr, 16);
                        applyThemePayload(tp);
                        Serial.printf("[theme] synced '%s' (#%06lx)\n", tp.name, (unsigned long)tp.accent);
                    }
                } else if (serialBuf[0] == 'T' || serialBuf[0] == 't') {
                    time_t epoch = (time_t)strtoll(serialBuf.c_str() + 1, nullptr, 10);
                    if (epoch > 1700000000) {
                        rtcSetUtc(epoch);
                        updateClock();
                        Serial.printf("[rtc] set to %ld (utc)\n", (long)epoch);
                    }
                } else if (serialBuf == "W" || serialBuf == "w") {
                    Serial.printf("[status] batt=%.0f%% %.0fmV  steps=%lu  screen=%d  ble=%d  peers=%d  mesh=%d  cpu=%luMHz\n",
                                  instance.getBatteryPercent(), instance.getBattVoltage(),
                                  (unsigned long)instance.getStepCounter(), curScreen,
                                  (int)blecomp::enabled(), blecomp::peerCount(),
                                  (int)meshsvc::enabled(), (unsigned long)getCpuFrequencyMhz());
                }
                serialBuf = "";
            }
        } else if (serialBuf.length() < 256) {
            serialBuf += c;
        }
    }
}

// ----------------------------------------------------------------- buildUI
static void buildUI()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, colBg(), 0);

    for (int i = 0; i < SC_COUNT; i++) {
        screens[i] = lv_obj_create(scr);
        lv_obj_set_size(screens[i], 240, 240);
        lv_obj_center(screens[i]);
        lv_obj_set_style_bg_opa(screens[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(screens[i], 0, 0);
        lv_obj_set_style_pad_all(screens[i], 0, 0);
        lv_obj_set_scrollbar_mode(screens[i], LV_SCROLLBAR_MODE_OFF);
    }

    buildWatch(screens[SC_WATCH]);
    buildMedia(screens[SC_MEDIA]);
    buildNav(screens[SC_NAV]);
    buildNotif(screens[SC_NOTIF]);
    buildLauncher(screens[SC_LAUNCHER]);
    buildBle(screens[SC_BLE]);
    buildThemes(screens[SC_THEMES]);
    buildSteps(screens[SC_STEPS]);
    buildMesh(screens[SC_MESH]);
    buildSettings(screens[SC_SETTINGS]);

    for (int i = 0; i < SC_COUNT; i++) {
        lv_obj_t *arrowL = lv_button_create(screens[i]);
        lv_obj_set_size(arrowL, 34, 26);
        lv_obj_align(arrowL, LV_ALIGN_BOTTOM_LEFT, 2, -2);
        lv_obj_set_style_bg_color(arrowL, colSurface(), 0);
        lv_obj_set_style_border_width(arrowL, 1, 0);
        lv_obj_set_style_border_color(arrowL, colMuted(), 0);
        lv_obj_set_style_radius(arrowL, 0, 0);
        lv_obj_set_style_shadow_width(arrowL, 0, 0);
        lv_obj_add_event_cb(arrowL, onPrev, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *ll = lv_label_create(arrowL);
        lv_label_set_text(ll, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(ll, colText(), 0);
        lv_obj_center(ll);

        lv_obj_t *arrowR = lv_button_create(screens[i]);
        lv_obj_set_size(arrowR, 34, 26);
        lv_obj_align(arrowR, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
        lv_obj_set_style_bg_color(arrowR, colSurface(), 0);
        lv_obj_set_style_border_width(arrowR, 1, 0);
        lv_obj_set_style_border_color(arrowR, colMuted(), 0);
        lv_obj_set_style_radius(arrowR, 0, 0);
        lv_obj_set_style_shadow_width(arrowR, 0, 0);
        lv_obj_add_event_cb(arrowR, onNext, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *rr = lv_label_create(arrowR);
        lv_label_set_text(rr, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(rr, colText(), 0);
        lv_obj_center(rr);
    }

    showScreen(curScreen);
    updateClock();
    updateBattery();
    updateSteps();
    updateWatchLinkUI();
    updateMediaUI();
    updateNavUI();
    updateBleUI();
    updateMeshUI();

    if (!wakeShield) {
        wakeShield = lv_obj_create(lv_layer_top());
        lv_obj_set_size(wakeShield, 240, 240);
        lv_obj_center(wakeShield);
        lv_obj_set_style_bg_opa(wakeShield, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(wakeShield, 0, 0);
        lv_obj_add_flag(wakeShield, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(wakeShield, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(wakeShield, [](lv_event_t *) {
            ateWakeTouch = true;
            wakeScreen();
        }, LV_EVENT_CLICKED, nullptr);
    }
}

// ----------------------------------------------------------------- power & side button
static void enterDeepOff()
{
    if (deepOff) return;
    deepOff = true;
    dimmed  = true;
    if (wakeShield) lv_obj_remove_flag(wakeShield, LV_OBJ_FLAG_HIDDEN);
    instance.setBrightness(0);
    instance.powerControl(POWER_DISPLAY_BACKLIGHT, false);  // cut ALDO2 boost quiescent draw
    instance.LilyGoDispSPI::sleep();
    setCpuFrequencyMhz(80);
}

static void wakeScreen()
{
    if (deepOff) {
        setCpuFrequencyMhz(240);
        instance.LilyGoDispSPI::wakeup();
        instance.powerControl(POWER_DISPLAY_BACKLIGHT, true);
    }
    dimmed  = false;
    deepOff = false;
    if (wakeShield) lv_obj_add_flag(wakeShield, LV_OBJ_FLAG_HIDDEN);
    instance.setBrightness(cfgBright);
    lv_display_trigger_activity(nullptr);
}

static void onPmuEvent(const DeviceEvent &ev, void *)
{
    PMUEventType_t pt = instance.getPMUEventType(ev);
    if (pt == PMU_EVENT_KEY_CLICKED)           gBtnShort = true;
    else if (pt == PMU_EVENT_KEY_LONG_PRESSED) gBtnLong  = true;
}

static void playNavCue(const char *action)
{
    String a(action ? action : "");
    a.toLowerCase();
    const bool isRight = (a.indexOf("right") >= 0);
    const bool isLeft  = (a.indexOf("left") >= 0);
    const bool isUturn = (a.indexOf("uturn") >= 0 || a.indexOf("finish") >= 0 || a.indexOf("dest") >= 0 || a.indexOf("roundabout") >= 0);

    // 1. Directional audio chime over MAX98357A speaker (POWER_SPEAK auto-gates off on close)
    static constexpr int kRate = 16000;
    static constexpr int kSamples = 1920;  // 120ms
    int16_t pcm[kSamples];
    if (instance._audioOutput.open(16, 1, kRate)) {
        instance._audioOutput.setVolume(80);
        for (int i = 0; i < kSamples; i++) {
            int freq = 1046;
            if (isRight)      freq = (i < kSamples / 2) ? 880 : 1320;          // rising
            else if (isLeft)  freq = (i < kSamples / 2) ? 1320 : 880;          // falling
            else if (isUturn) freq = (i % 640 < 320) ? 1175 : 880;             // triple alert
            int period = kRate / freq;
            pcm[i] = ((i % period) < (period / 2)) ? 9000 : -9000;
        }
        instance._audioOutput.write((const uint8_t *)pcm, sizeof(pcm));
        instance._audioOutput.close();
    }

    // 2. Distinct tactile vibration pattern per direction:
    //    Right = 2 crisp buzzes | Left = 1 firm buzz | U-turn/Dest = 3 rapid buzzes | Straight = 1 tap
    if (isUturn) {
        for (int i = 0; i < 3; i++) { instance.vibrator(); delay(85); }
    } else if (isRight) {
        instance.vibrator(); delay(110); instance.vibrator();
    } else {
        instance.vibrator();
    }
}

// ----------------------------------------------------------------- setup/loop
void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[watch] LilyGo T-Watch S3 — Omarchy Minimalist Companion");

    WiFi.mode(WIFI_OFF);  // Strict 0 mA Wi-Fi idle baseline; no SoftAP web server

    instance.begin();
    instance.setBrightness(cfgBright);
    beginLvglHelper(instance);

    prefs.begin("badge", false);
    {
        String oname = prefs.getString("oname", "");
        if (oname.length()) {
            oname.toCharArray(gOmaThemeName, sizeof(gOmaThemeName));
            PALETTES[1].bg      = prefs.getUInt("obg",   PALETTES[1].bg);
            PALETTES[1].surface = prefs.getUInt("osurf", PALETTES[1].surface);
            PALETTES[1].text    = prefs.getUInt("otxt",  PALETTES[1].text);
            PALETTES[1].muted   = prefs.getUInt("omut",  PALETTES[1].muted);
            PALETTES[1].accent  = prefs.getUInt("oacc",  PALETTES[1].accent);
        }
    }
    cfgPalette  = prefs.getInt("palette", 0);
    cfgBright   = prefs.getInt("bright", cfgBright);
    cfgTimeoutS = prefs.getInt("timeout", cfgTimeoutS);
    {
        int tzStored = prefs.getInt("tz", cfgTzOffsetMin);
        cfgTzOffsetMin = (tzStored > -15 && tzStored < 15) ? tzStored * 60 : tzStored;
    }
    if (cfgPalette < 0 || cfgPalette >= PALETTE_COUNT) cfgPalette = 0;
    if (!rtcValid()) rtcFromBuildTime();

    gLastStepCount = instance.getStepCounter();

    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        const uint32_t nodeNum = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                                 ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
        meshsvc::begin(nodeNum, "Leonardo Bora", "BORA");
        blecomp::begin(true);
    }

    instance.powerControl(POWER_SPEAK, false);
    instance.powerControl(POWER_GPS, false);
    if (!meshsvc::enabled()) instance.powerControl(POWER_RADIO, false);
    instance.onEvent(POWER_EVENT, onPmuEvent);

    instance.setBrightness(cfgBright);
    buildUI();

    instance.bma423.selectPlatform(SensorBMA423::Platform::WRISTBAND);
    instance.bma423.enableTiltDetector(true, true, InterruptPinMap::PIN1);
    gTiltSeen = instance.getRuntimeStatus().tiltEvents;
}

void loop()
{
    instance.loop();
    blecomp::loop();
    if (!deepOff) lv_timer_handler();
    handleSerial();
    if (meshsvc::enabled()) meshsvc::loop();

    uint32_t now = millis();

    // Physical side button (AXP2101 PEKEY):
    //   Normal mode:
    //     Short press: wake -> Watch Face; if on another screen -> Home; if on Watch Face -> Sleep
    //     Long press:  wake -> jump to Media Player (or toggle Play/Pause if already on Media)
    //   Ride / Nav mode (eyes-free handlebar control):
    //     Short press: wake -> NAV; toggle between NAV <-> MEDIA
    //     Long press:  toggle Play/Pause immediately without leaving current screen
    const bool bikeCtx = (gRideMode || blecomp::navActive());
    if (gBtnShort) {
        gBtnShort = false;
        if (deepOff || dimmed) {
            wakeScreen();
            showScreen(bikeCtx ? SC_NAV : SC_WATCH);
        } else if (bikeCtx) {
            showScreen(curScreen == SC_NAV ? SC_MEDIA : SC_NAV);
        } else if (curScreen != SC_WATCH) {
            showScreen(SC_WATCH);
        } else {
            enterDeepOff();
        }
    }
    if (gBtnLong) {
        gBtnLong = false;
        bool wasSleeping = (deepOff || dimmed);
        wakeScreen();
        if (bikeCtx || (!wasSleeping && curScreen == SC_MEDIA)) {
            blecomp::sendMusicCmd(blecomp::musicPlaying() ? "pause" : "play");
            updateMediaUI();
            updateNavUI();
        } else {
            showScreen(SC_MEDIA);
        }
        instance.vibrator();
    }

    uint32_t sc = instance.getStepCounter();
    if (sc != gLastStepCount) {
        gLastStepCount = sc;
        updateSteps();
    }

    if (!deepOff) pollSwipe();

    // Wrist raise wakes display cleanly (no Matrix rain overhead)
    uint32_t tilts = instance.getRuntimeStatus().tiltEvents;
    if (tilts != gTiltSeen) {
        gTiltSeen = tilts;
        wakeScreen();
    }

    // Direct battery-first idle ladder: dim at timeout -> panel sleep + 80MHz at 2x timeout
    // In Ride Mode, stay at readable dim brightness so handlebar glance always works.
    uint32_t inactive = lv_display_get_inactive_time(nullptr);
    uint32_t limitMs  = (uint32_t)cfgTimeoutS * 1000U;
    if (!dimmed && inactive > limitMs) {
        dimmed = true;
        if (wakeShield) lv_obj_remove_flag(wakeShield, LV_OBJ_FLAG_HIDDEN);
        int dim = gRideMode ? (cfgBright / 2) : (cfgBright / 4);
        int floorB = gRideMode ? 48 : 10;
        instance.setBrightness(dim > floorB ? dim : floorB);
    } else if (dimmed && !deepOff && !gRideMode && inactive > limitMs * 2U) {
        enterDeepOff();
    } else if ((dimmed || deepOff) && (inactive < 800 || instance.getTouched())) {
        ateWakeTouch = true;
        wakeScreen();
    }

    if (now - lastClockMs > 1000) {
        lastClockMs = now;
        updateClock();
        const time_t wall = rtcNowUtc();
        if (wall > 1700000000) meshsvc::setWallClock((uint32_t)wall);
    }
    if (now - lastBatteryMs > 10000) {
        lastBatteryMs = now;
        updateBattery();
    }

    if (meshsvc::takeMessageFlag()) {
        instance.vibrator();
        updateMeshUI();
    }
    if (curScreen == SC_MESH && now - meshUiAt > 1000) {
        meshUiAt = now;
        updateMeshUI();
    }

    // BLE S21 + Omarchy live sync
    {
        time_t bleUtc = 0;
        int bleTzMin = 0;
        bool hasUtc = false, hasTz = false;
        if (blecomp::takeClockSync(&bleUtc, &bleTzMin, &hasUtc, &hasTz)) {
            if (hasUtc) rtcSetUtc(bleUtc);
            if (hasTz) {
                cfgTzOffsetMin = bleTzMin;
                prefs.putInt("tz", cfgTzOffsetMin);
            }
            updateClock();
        }
        blecomp::ThemePayload tp;
        if (blecomp::takeThemeSync(&tp)) applyThemePayload(tp);
        static bool prevNavActive = false;
        bool nowNav = blecomp::navActive();
        if (nowNav && !prevNavActive) {
            if (!gRideMode) {
                gRideMode = true;
                gRideStartMs = millis();
            }
            wakeScreen();
            if (curScreen != SC_NAV) showScreen(SC_NAV);
        }
        prevNavActive = nowNav;

        if (blecomp::takeBuzzFlag()) {
            if (blecomp::callActive()) {
                wakeScreen();
                showScreen(SC_BLE);
                playNavCue("uturn");  // distinct ring chime + triple buzz
            } else if (nowNav) {
                char act[24] = {0}, di[24] = {0}, in[96] = {0}, et[20] = {0};
                bool on = false;
                blecomp::navSnapshot(act, sizeof(act), di, sizeof(di), in, sizeof(in), et, sizeof(et), &on);
                wakeScreen();
                if (curScreen != SC_NAV && curScreen != SC_MEDIA) showScreen(SC_NAV);
                playNavCue(act);
            } else {
                char s[20], t[36], b[80];
                long id = 0; bool rep = false;
                if (blecomp::latestNotif(s, sizeof(s), t, sizeof(t), b, sizeof(b), &id, &rep) && (rep || id != 0)) {
                    wakeScreen();
                    if (curScreen != SC_NAV && curScreen != SC_MEDIA && !gRideMode) showScreen(SC_NOTIF);
                }
                instance.vibrator();
            }
        }
        if (blecomp::takeUiDirty()) {
            updateWatchLinkUI();
            updateMediaUI();
            updateNavUI();
            updateNotifUI();
            updateBleUI();
        }
    }

    delay(deepOff ? (instance.isCharging() ? 25 : 65) : 5);
}

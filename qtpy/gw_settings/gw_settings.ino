// G-Wolves settings page on an Adafruit QT Py ESP32-S3, plugged into the HID Remapper Feather's
// STEMMA QT port. The QT Py is the I2C master; the Feather (gw_link.cc) answers at 0x42 with a
// status block (128 bytes read every poll; 192 with the chord table, read when that changes) and takes
// command frames (layouts in firmware/src/gw_mouse.cc and gw_chords.h).
//
// Radio: off at power-up. Holding Mid+Right on the mouse bumps a counter in the Feather's status
// block; each bump switches Wi-Fi on or off (the QT Py's Boot button does the same). It switches
// itself off after 10 minutes without a page action; the page can change that to 1 hour or never
// ("never" also brings Wi-Fi up at power-up). With no saved network, or when the saved one can't
// be joined after a switch-on by hand, it opens its own network "GW-Settings" for setup
// (192.168.4.1). That open network is never started unasked and always times out.
// Page: http://gwolves.local (or the IP shown on the setup page).
// LED: off = radio off, yellow = joining, blue = on your network, magenta = setup network.
// Blinking over that while the Feather isn't answering properly: red = nothing answers at all,
// orange = something answers, but not with a valid status block.
// New firmware over Wi-Fi: POST the .bin to /api/update (home network only); see flash-qtpy.ps1.

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_system.h>

#include "page.h"

#define LINK_ADDR 0x42
#define LINK_SDA 41  // STEMMA QT (Wire1)
#define LINK_SCL 40
#define LINK_HZ 50000  // only the pins' built-in pull-ups hold the lines up, so keep it slow
#define STATUS_LEN 128  // Feather v14+ fills 64-127 (own crc); older ones send 0xFF there
#define STATUS_FULL 512 // Feather v15+: 128-191 = the chord table (own crc), read only when byte 124 changes; v21+:
                        // 192-255 = recent actions (own crc), read when byte 117's high nibble changes (v15-v20: 192);
                        // v23+: 256-511 = the USB host capture (own crc), read on the same nibble
#define QT_FW 31      // this firmware's number, shown in /api/status
#define POLL_MS 100
#define RADIO_IDLE_MS (10UL * 60 * 1000)
#define RADIO_IDLE_LONG_MS (60UL * 60 * 1000)
#define JOIN_TIMEOUT_MS 15000
#define CMD_RETRY_MS 300
#define CMD_TRIES 5
#define BOOT_BUTTON 0

// command codes (gw_mouse.cc)
#define C_SET_STAGE 0x01
#define C_SET_ANGLE 0x02
#define C_SET_POS 0x03
#define C_SET_PALETTE 0x04
#define C_SET_DPI 0x05
#define C_SET_TUNING 0x06
#define C_RADIO_STATE 0x07
#define C_REFRESH 0x08
#define C_FLASH 0x09
#define C_PALETTE_DEFAULT 0x0A
#define C_SET_OPTION 0x0B  // Feather v14+: [option 0-4, value as the mouse stores it]
#define C_RAW 0x0C         // Feather v14+: [16 bytes of report 8: a read (08) or the status query (04)]
#define C_SET_CHORDS 0x0D  // Feather v15+: [part, total, rules] (gw_chords.h)
#define C_SET_DIRGAIN 0x0E  // Feather v24+: [on, 8 gains uint16 per mille] sensitivity per direction (RAM only there)

// chord table limits and action count (gw_chords.h)
#define CH_MAX 12
#define CH_ACTIONS 18

static WebServer server(80);
static DNSServer dns;
static Preferences prefs;

static uint8_t st[STATUS_LEN];
static bool st_ok = false;
static bool st_ext = false;  // bytes 64-127 valid (Feather v14+)
static uint8_t info_seq_seen = 0;    // the Feather's status-query reply count when last noticed
static uint32_t info_at = 0;         // millis() when a new reply was noticed (0 = none)
static bool info_first = true;       // the first status after a restart: a reply already there is of unknown age
static bool info_age_known = false;
static uint32_t st_at = 0;       // last good status
static bool link_up = false;
static bool toggles_known = false;
static uint8_t last_toggles = 0;
static bool feather_init_done = false;  // tuning/palette pushed since the Feather (re)started
// link diagnostics (/api/status "diag")
static uint32_t d_polls = 0, d_ok = 0, d_short = 0, d_bad = 0, d_i2c_resets = 0;
static uint8_t d_last_n = 0, d_b0 = 0, d_b1 = 0;
static bool last_answered = false;  // the last read returned a full block, valid or not
// the Feather's chord table (status bytes 128-191), as last read
static uint8_t ch[64];
static bool ch_ok = false;

enum class Radio : uint8_t { OFF, JOINING, STA, AP };
static Radio radio = Radio::OFF;
static uint32_t radio_since = 0;
static uint32_t last_http = 0;
// fw22: the last EDIT_LOG edits made through the page (any /api/set, dpi, palette, tune, opt, chords), with who sent
// them and when. Kept in NVS ("edits"), so they outlive power cycles (the ATEN switching ports) and QT Py updates; the
// Feather's own recent-changes list is RAM only. fw21 kept the sender in RAM only.
#define EDIT_LOG 20
#define EK_STAGE 1  // the kinds; 1-3 are the Feather's link commands, so they match its "Page" events
#define EK_ANGLE 2
#define EK_POS 3
#define EK_DPI 4
#define EK_COLOURS 5
#define EK_TUNE 6
#define EK_OPT 7
#define EK_CHORDS 8
#define EK_CHORDS_DEFAULT 9
#define EK_AUTO_STAGE 10  // fw24: the stage set for the host on this power-on ("name=stage 0-4"; dev 7 = automatic)
#define EK_HOST_STAGE 11  // fw24: a host's stage chosen on the page ("name=stage 0-5")
#define EK_DIRGAIN 12     // fw26: the direction gains or their hosts saved ("N hosts")
#define EK_TEMP_DPI 13    // fw28: the calibration's temporary DPI ("N") or the table put back ("restored", dev 7)
struct __attribute__((packed)) EditLog {  // 32 bytes
    uint32_t t;     // Unix time (s): the browser's clock sent with the request, else NTP, else 0 = unknown
    uint32_t ms;    // millis() then: ages within the same boot when the time is unknown
    uint32_t ip;    // sender IPv4, as IPAddress's uint32_t
    uint16_t boot;  // the QT Py boot it was made in (low 16 bits of "boots")
    uint8_t kind;   // EK_*
    uint8_t dev;    // 0 other, 1 Windows, 2 Android, 3 iPhone/iPad, 4 Mac, 5 Linux, 6 a script (PowerShell, curl, ...),
                    // 7 the QT Py itself (fw24's automatic stage)
    char text[16];  // the value: "-14", "poll=1000", "6 rules"; empty when the kind says it all
};
static EditLog edit_log[EDIT_LOG];  // edit_log[0] is the newest
static uint8_t edit_n = 0;
static uint32_t clk_base = 0;  // Unix time at millis() 0, from a page request's "t"; 0 = not known yet this boot
static bool ntp_want = false;
#define HCAP_KEEP 8
#define HCAP_ENTRIES 29  // Feather GW_HOSTCAP_MAX
struct __attribute__((packed)) HostCap {
    uint32_t t;        // Unix time it was first read (0 = unknown; filled in once the clock is known that boot)
    uint32_t ms;       // millis() then
    uint16_t boot;     // the QT Py boot (= the power-on: the Feather and QT Py lose power together)
    uint8_t blk[256];  // the Feather's status bytes 256-511 as read
};
static HostCap hcap[HCAP_KEEP];  // [0] newest
static uint8_t hcap_n = 0;
static bool hcap_dirty = false;
static uint32_t hcap_changed_at = 0;
static uint8_t hcap_seq = 0;  // in /api/status: the page fetches /api/hosts again when it moves
// ---- fw24: automatic DPI stage per host
// A host's signature: FNV-1a over its first HSIG_REQS requests as sent, skipping bus resets and with SET_ADDRESS's
// address zeroed: those vary between connections of the same host (seen 10-20: XIM1/XIM2 and FLOW twice each gave the
// same first 12 otherwise). 0 = not enough requests yet. The user picks a stage per signature on the page; on each
// power-on the QT Py sets it once the Warg is there, checks the mouse took it, and retries a few times.
#define HSIG_REQS 12
#define HPROF_MAX 8
struct __attribute__((packed)) HostProf {  // 16 bytes, NVS "hprof"
    uint32_t sig;
    uint8_t stage;  // 0 = leave the stage alone, 1-5 = that stage
    char name[11];
};
static HostProf hprof[HPROF_MAX];
static uint8_t hprof_n = 0;
static uint32_t auto_sig = 0;     // this power-on's host, once known
static uint8_t auto_want = 0xFF;  // the stage (0-4) to set, 0xFF = nothing to do
static uint8_t auto_tries = 0;
static uint32_t auto_at = 0;
static uint8_t auto_state = 0;  // 0 waiting, 1 sent, 2 confirmed, 3 gave up, 4 no stage for this host
// fw26: sensitivity per direction (Feather v24, like Corsair's PureVector): 8 gains in per mille, one per 45 degrees
// clockwise from "right" on screen, and the hosts they apply on (by signature). NVS "dgain". The QT Py keeps the
// Feather's copy right: it sends the table, on for an enabled host and off otherwise, whenever the Feather's status
// (242 applying/got, 243 crc of the gains) doesn't match, e.g. after every power-on.
struct __attribute__((packed)) DirGainCfg {
    uint16_t g[8];
    uint8_t n;
    uint32_t sig[HPROF_MAX];
};
static DirGainCfg dg = { { 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000 }, 0, { 0 } };
static uint32_t dg_sent_at = 0;
static bool dg_reread = false;
static uint16_t dg_sends = 0;  // since boot, for /api/status
// fw27: the flick calibrations' summaries (the page sends one after each run), newest first; NVS "calib"
#define CALIB_KEEP 6
struct __attribute__((packed)) CalibRec {  // 46 bytes
    uint32_t t;         // Unix time
    uint8_t n[8];       // flicks measured per direction
    uint16_t med[8];    // median reach per mille (0 = too few)
    uint16_t g[8];      // the gains this host applied while measuring, per mille
    uint8_t flags;      // bit 0 raw input, bit 1 coarse browser clock
    uint8_t skipped;    // flicks skipped (at most 255)
};
static CalibRec calib[CALIB_KEEP];
// fw28: a temporary DPI for the flick calibration. The page lowers the current stage's DPI so flicks as long as the
// ones in game fit on the screen; the whole table goes back afterwards: when the page says so, when it stops checking
// in for 12 s (closed, crashed), or after a power cut mid-test (the ATEN): the original table is kept in NVS "tdpi"
// until the mouse has it back.
static uint16_t tdpi_orig[10];      // X,Y per stage as they were
static bool tdpi_active = false;    // lowered now, or a restore is still owed
static bool tdpi_restore = false;   // put the table back
static uint16_t tdpi_val = 0;
static uint32_t tdpi_keep_at = 0;
static uint32_t tdpi_sent_at = 0;
static uint32_t tdpi_read_at = 0;
// fw29: a restore counts only once the MOUSE reports the test stage's original DPI (a raw read of its register): the
// Feather echoes a written table at once, even when a sleeping mouse lost the write (review finding, 10-21)
static uint8_t tdpi_stage = 0xFF;   // the stage the test lowered (0xFF = not recorded: use the current one)
static uint8_t tdpi_phase = 0;      // 0 send the table, 1 read it back, 2 wait for the answer
static uint8_t tdpi_probe_n = 0;
static uint32_t tdpi_phase_at = 0;
static String calib_diag;           // fw29: the page's last calibration diagnostics (RAM only), GET /api/calibdiag
static uint8_t calib_n = 0;
static uint8_t wmode = 0;         // Wi-Fi switches itself off: 0 = 10 min idle, 1 = 1 hour idle, 2 = never
static bool radio_manual = true;   // this radio session was started by hand (button, chord, page)
// Wi-Fi power save. Off by default: with it on the board can go unreachable for a minute or more
// on some routers (broadcast ARP and mDNS queries get lost while it dozes). /api/wifisleep sets it.
static bool wsleep = false;
static int16_t ui_folds = -1;  // the page's open fold-outs (bit 0 Chords, 1 Change Wi-Fi network, 2 Recent changes); -1 = never set
// diagnostics (/api/status "wifi", "boot")
static uint32_t w_disc = 0, w_disc_at = 0;
static uint8_t w_disc_reason = 0;
static uint32_t b_boots = 0, b_brownouts = 0, b_crashes = 0;
static uint8_t b_reason = 0;

// A dropout = losing a working connection, once per outage. Not counted: the board's own switch-off
// (reason ASSOC_LEAVE) or the failed retries that follow a drop (no address in between).
static bool w_had_ip = false;

static void on_wifi_got_ip(WiFiEvent_t, WiFiEventInfo_t) {
    w_had_ip = true;
    ntp_want = true;  // fw22: a clock for edits sent without the page's "t"; started from loop()
}

static void on_wifi_disconnect(WiFiEvent_t, WiFiEventInfo_t info) {
    uint8_t reason = info.wifi_sta_disconnected.reason;
    if (w_had_ip && (reason != WIFI_REASON_ASSOC_LEAVE)) {
        w_disc++;
        w_disc_at = millis();
        w_disc_reason = reason;
    }
    w_had_ip = false;
}

// for strings that go into the status JSON
static String json_str(const String& s) {
    String o;
    o.reserve(s.length() + 2);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if ((c == '"') || (c == '\\')) {
            o += '\\';
            o += c;
        } else if ((uint8_t) c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", (uint8_t) c);
            o += b;
        } else {
            o += c;
        }
    }
    return o;
}

static const char* reset_name(uint8_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_SW: return "restart";
        case ESP_RST_PANIC: return "crash";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "usb";
        default: return "other";
    }
}

// ---- command queue (one frame in flight, acked by status byte 55)

struct Cmd {
    uint8_t f[48];
    uint8_t n;
    uint8_t seq;
};
#define CMDQ 8
static Cmd cmdq[CMDQ];
static uint8_t cq_head = 0, cq_n = 0;
static uint8_t next_seq = 1;
static uint32_t cmd_sent_at = 0;
static uint8_t cmd_tries = 0;

static uint8_t crc8(const uint8_t* d, uint8_t n) {
    uint8_t c = 0;
    for (uint8_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) {
            c = (c & 0x80) ? (uint8_t) ((c << 1) ^ 0x07) : (uint8_t) (c << 1);
        }
    }
    return c;
}

static bool queue_cmd(uint8_t cmd, const uint8_t* payload, uint8_t len) {
    if ((cq_n == CMDQ) || (len > 44)) {
        return false;
    }
    Cmd& c = cmdq[(cq_head + cq_n) % CMDQ];
    c.seq = next_seq;
    next_seq = (next_seq == 255) ? 1 : next_seq + 1;
    c.f[0] = cmd;
    c.f[1] = c.seq;
    c.f[2] = len;
    if (len) {
        memcpy(c.f + 3, payload, len);
    }
    c.f[3 + len] = crc8(c.f, 3 + len);
    c.n = 4 + len;
    cq_n++;
    if (cq_n == 1) {
        cmd_tries = 0;
        cmd_sent_at = 0;
    }
    return true;
}

static void send_head() {
    Cmd& c = cmdq[cq_head];
    Wire1.beginTransmission(LINK_ADDR);
    Wire1.write(c.f, c.n);
    Wire1.endTransmission();
    cmd_sent_at = millis() | 1;
    cmd_tries++;
}

static void pump_cmds() {
    if (cq_n == 0) {
        return;
    }
    Cmd& c = cmdq[cq_head];
    if (st_ok && (st[55] == c.seq)) {  // acked
        cq_head = (cq_head + 1) % CMDQ;
        cq_n--;
        cmd_tries = 0;
        cmd_sent_at = 0;
        if (cq_n) {
            send_head();
        }
        return;
    }
    if ((cmd_sent_at == 0) || (millis() - cmd_sent_at >= CMD_RETRY_MS)) {
        if (cmd_tries >= CMD_TRIES) {  // give up on this one
            cq_head = (cq_head + 1) % CMDQ;
            cq_n--;
            cmd_tries = 0;
            cmd_sent_at = 0;
            return;
        }
        send_head();
    }
}

// ---- Feather status

static bool poll_status() {
    uint8_t b[STATUS_LEN];
    d_polls++;
    uint8_t n = Wire1.requestFrom((uint8_t) LINK_ADDR, (uint8_t) STATUS_LEN);
    d_last_n = n;
    if (n != STATUS_LEN) {
        while (Wire1.available()) {
            Wire1.read();
        }
        d_short++;
        last_answered = false;
        return false;
    }
    for (int i = 0; i < STATUS_LEN; i++) {
        b[i] = Wire1.read();
    }
    last_answered = true;
    d_b0 = b[0];
    d_b1 = b[1];
    if ((b[0] != 'G') || (b[1] != 1) || (crc8(b, 63) != b[63])) {
        d_bad++;
        return false;
    }
    d_ok++;
    memcpy(st, b, STATUS_LEN);
    st_ext = (b[64] == 'X') && (crc8(b + 64, 63) == b[127]);
    if (st_ext && info_first) {
        info_first = false;
        info_seq_seen = b[65];
        info_at = (b[66] > 0) ? 1 : 0;
        info_age_known = false;
    } else if (st_ext && (b[66] > 0) && (b[65] != info_seq_seen)) {
        info_seq_seen = b[65];
        info_at = millis() | 1;
        info_age_known = true;
    }
    st_ok = true;
    st_at = millis();
    return true;
}

// recent actions (Feather v21+): its block 192-255 as read, when, and the change counter it went with
static uint8_t evb[64];
static bool ev_ok = false;
static uint32_t ev_read_at = 0;
static uint8_t ev_nib = 0xFF;

static bool events_supported() {
    return st_ok && st_ext && (st[56] >= 21);
}

// the full block, for its chord table (when the table number, byte 124, moved) and, from Feather v21, its recent
// actions (when byte 117's high nibble moved)
static void hcap_take(const uint8_t* u);

static void read_chords() {
    static uint8_t b[STATUS_FULL];
    size_t len = hostcap_supported() ? 512 : events_supported() ? 256 : 192;
    if (len == 512) {
        Wire1.setTimeOut(150);  // 512 bytes take about 92 ms at 50 kHz; polls keep the short timeout
    }
    size_t n = Wire1.requestFrom((uint16_t) LINK_ADDR, len);
    if (len == 512) {
        Wire1.setTimeOut(50);
    }
    if (n != len) {
        while (Wire1.available()) {
            Wire1.read();
        }
        return;
    }
    for (size_t i = 0; i < len; i++) {
        b[i] = Wire1.read();
    }
    if ((b[0] != 'G') || (crc8(b, 63) != b[63]) || (b[128] != 'C') || (crc8(b + 128, 63) != b[191]) || (b[130] > CH_MAX)) {
        return;
    }
    memcpy(ch, b + 128, 64);
    ch_ok = true;
    if ((len >= 256) && (b[192] == 'E') && (crc8(b + 192, 63) == b[255]) && (b[193] <= 8)) {
        memcpy(evb, b + 192, 64);
        ev_ok = true;
        ev_read_at = millis();
        ev_nib = b[117] >> 4;
    }
    if ((len == 512) && (b[256] == 'U') && (crc8(b + 256, 255) == b[511]) && (b[257] != 0) && (b[259] <= HCAP_ENTRIES)) {
        hcap_take(b + 256);
    }
}

static bool hostcap_supported() {
    return st_ok && st_ext && (st[56] >= 23);
}

// fw23: the Feather's capture of the USB host's requests (Feather v23, status 256-511). The ATEN cuts the power when
// it switches, so each power-on is one host: keep the last HCAP_KEEP captures in NVS ("hcaps"), newest first.
static void hcap_take(const uint8_t* u) {
    bool same = hcap_n && (hcap[0].boot == (uint16_t) b_boots) && (hcap[0].blk[1] == u[1]);
    if (same && (hcap[0].blk[2] == u[2])) {
        return;  // nothing new
    }
    // fw24: only the QT Py restarted (an update): the Feather still holds the capture already saved, perhaps with more
    // requests since. Same capture number, resets and ms to configuration, and it starts with the saved requests: take
    // it over (a real new power-on differs in its timing at least).
    if (!same && hcap_n && (hcap[0].blk[1] == u[1]) && (hcap[0].blk[4] == u[4]) && (hcap[0].blk[5] == u[5]) && (hcap[0].blk[6] == u[6])) {
        uint8_t k = (hcap[0].blk[3] < u[3]) ? hcap[0].blk[3] : u[3];
        if (k > HCAP_ENTRIES) {
            k = HCAP_ENTRIES;
        }
        if (!memcmp(hcap[0].blk + 16, u + 16, 8 * k)) {
            hcap[0].boot = (uint16_t) b_boots;
            memcpy(hcap[0].blk, u, 256);
            hcap_dirty = true;
            hcap_changed_at = millis();
            hcap_seq++;
            return;
        }
    }
    if (!same) {
        memmove(hcap + 1, hcap, sizeof(HostCap) * (HCAP_KEEP - 1));
        if (hcap_n < HCAP_KEEP) {
            hcap_n++;
        }
        hcap[0].t = now_unix();
        hcap[0].ms = millis();
        hcap[0].boot = (uint16_t) b_boots;
    }
    memcpy(hcap[0].blk, u, 256);
    hcap_dirty = true;
    hcap_changed_at = millis();
    hcap_seq++;
}

// saved once a capture has been quiet for 2 s; a capture read before the clock was known gets its time once it is
static void hcap_tick() {
    uint32_t now = now_unix();
    if (now) {
        for (uint8_t k = 0; k < hcap_n; k++) {
            if (!hcap[k].t && (hcap[k].boot == (uint16_t) b_boots)) {
                hcap[k].t = now - (millis() - hcap[k].ms) / 1000;
                hcap_dirty = true;
            }
        }
    }
    if (hcap_dirty && (millis() - hcap_changed_at >= 2000)) {
        hcap_dirty = false;
        prefs.putBytes("hcaps", hcap, sizeof(HostCap) * hcap_n);
    }
}



static bool chords_supported() {
    return st_ok && st_ext && (st[56] >= 15);
}

static int16_t s8(uint8_t i) {
    return (int8_t) st[i];
}
static uint16_t u16(uint8_t i) {
    return st[i] | st[i + 1] << 8;
}

// ---- LED

static void led(uint8_t r, uint8_t g, uint8_t b) {
#ifdef PIN_NEOPIXEL
    static uint32_t shown = 0xffffffff;
    uint32_t v = (uint32_t) r << 16 | (uint32_t) g << 8 | b;
    if (v == shown) {
        return;
    }
    shown = v;
    rgbLedWrite(PIN_NEOPIXEL, r, g, b);
#endif
}

// the radio's colour; while the link is down it alternates with red (nothing answers) or orange
// (answers, but not with a valid block)
static void show_state() {
    static uint32_t blink = 0;
    static bool on = false;
    if (millis() - blink > 500) {
        blink = millis();
        on = !on;
    }
    uint8_t r = 0, g = 0, b = 0;
    switch (radio) {
        case Radio::OFF: break;
        case Radio::JOINING: r = 12; g = 8; break;
        case Radio::STA: b = 14; break;
        case Radio::AP: r = 12; b = 12; break;
    }
    if (!link_up && on) {
        r = 14;
        g = last_answered ? 5 : 0;
        b = 0;
    }
    led(r, g, b);
}

// ---- radio

static String ap_name() {
    return String("GW-Settings");
}

static void radio_off() {
    if (radio == Radio::OFF) {
        return;
    }
    server.stop();
    dns.stop();
    MDNS.end();
    WiFi.disconnect(true);
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    radio = Radio::OFF;
    uint8_t v = 0;
    queue_cmd(C_RADIO_STATE, &v, 1);
}

static void start_mdns() {
    MDNS.end();
    MDNS.begin("gwolves");
    MDNS.addService("http", "tcp", 80);
}

static void start_ap() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_name().c_str());
    dns.start(53, "*", WiFi.softAPIP());
    radio = Radio::AP;
    start_mdns();
}

static void radio_on(bool manual = true) {
    if (radio != Radio::OFF) {
        return;
    }
    String ssid = prefs.getString("ssid", "");
    if (!ssid.length() && !manual) {
        return;  // nothing to join, and the setup network is never opened unasked
    }
    radio_manual = manual;
    radio_since = millis();
    last_http = millis();
    if (ssid.length()) {
        WiFi.setHostname("gwolves");  // the name the router lists it under (set before the mode)
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(wsleep);
        WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
        radio = Radio::JOINING;
    } else {
        start_ap();
    }
    server.begin();
    uint8_t v = 1;
    queue_cmd(C_RADIO_STATE, &v, 1);
}

// how long the radio stays on without a page action; 0 = no limit
static uint32_t idle_limit() {
    if ((radio == Radio::AP) || (wmode == 0)) {
        return RADIO_IDLE_MS;
    }
    return (wmode == 1) ? RADIO_IDLE_LONG_MS : 0;
}

static void radio_tick() {
    if ((radio == Radio::JOINING) && (WiFi.status() == WL_CONNECTED)) {
        radio = Radio::STA;
        start_mdns();
    }
    // switched on by hand and can't join: setup network instead. Started by itself (power-up,
    // after an update): keep trying, the Wi-Fi stack retries on its own.
    if ((radio == Radio::JOINING) && radio_manual && (millis() - radio_since > JOIN_TIMEOUT_MS)) {
        WiFi.disconnect(true);
        start_ap();
    }
    uint32_t lim = idle_limit();
    if ((radio != Radio::OFF) && lim && (millis() - last_http > lim)) {
        radio_off();
    }
    if (radio == Radio::AP) {
        dns.processNextRequest();
    }
}

static void radio_toggle() {
    if (radio == Radio::OFF) {
        radio_on();
    } else {
        radio_off();
    }
}

// a chord table as one or two frames: [part, total, up to 8 rules]
static bool queue_chords(const uint8_t* rules, uint8_t n) {
    if ((cq_n + ((n > 8) ? 2 : 1)) > CMDQ) {
        return false;
    }
    for (uint8_t part = 0; (part == 0) || (part * 8 < n); part++) {
        uint8_t d[2 + 8 * 5];
        uint8_t first = part * 8, cnt = (n - first > 8) ? 8 : n - first;
        d[0] = part;
        d[1] = n;
        memcpy(d + 2, rules + first * 5, cnt * 5);
        queue_cmd(C_SET_CHORDS, d, 2 + cnt * 5);
    }
    return true;
}

// ---- tuning / palette kept on the QT Py, given back to the Feather after it restarts

static void push_saved() {
    uint8_t t[12];
    if (prefs.getBytes("tune", t, 12) == 12) {
        queue_cmd(C_SET_TUNING, t, 12);
    } else {
        memcpy(t, st + 43, 12);  // nothing saved: confirm the Feather's own, so a restart shows up
        queue_cmd(C_SET_TUNING, t, 12);
    }
    uint8_t p[15];
    if (prefs.getBytes("pal", p, 15) == 15) {
        queue_cmd(C_PALETTE_DEFAULT, p, 15);
    }
    // chords: the Feather keeps its own in flash; a Feather on its built-in table (new, or its record
    // lost) gets the copy saved here, if there is one
    uint8_t c[1 + CH_MAX * 5];
    size_t cn = prefs.getBytes("chords", c, sizeof(c));
    if (chords_supported() && !(st[125] & 1) && (cn >= 1) && (cn == (size_t) (1 + c[0] * 5)) && (c[0] <= CH_MAX)) {
        queue_chords(c + 1, c[0]);
    }
}

static void link_tick() {
    static uint32_t last_poll = 0;
    uint32_t period = (radio == Radio::OFF) ? 2 * POLL_MS : POLL_MS;
    if (millis() - last_poll < period) {
        return;
    }
    last_poll = millis();
    bool ok = poll_status();
    bool was_up = link_up;
    link_up = ok || (millis() - st_at < 1000);
    if (!link_up) {
        toggles_known = false;
        feather_init_done = false;
        ch_ok = false;  // the Feather may come back restarted: its table number starts at 1 again
        // A Feather that resets in the middle of a read (flashed, or restarted on its own) can leave this I2C master
        // stuck: every read then comes back empty until the QT Py restarts (seen 2026-10-16). Start the bus over every
        // 3 s while nothing answers.
        static uint32_t reinit_at = 0;
        if ((millis() - st_at >= 3000) && (millis() - reinit_at >= 3000)) {
            reinit_at = millis();
            Wire1.end();
            Wire1.begin(LINK_SDA, LINK_SCL, LINK_HZ);
            Wire1.setTimeOut(50);
            d_i2c_resets++;
        }
        return;
    }
    if (!ok) {
        return;
    }
    if (chords_supported() && (!ch_ok || (ch[1] != st[124]) || (events_supported() && ((st[117] >> 4) != ev_nib)))) {
        read_chords();
    }
    // a restarted Feather has lost its tuning flag: re-baseline the counter, give back the settings
    if (feather_init_done && !(st[2] & 1)) {
        feather_init_done = false;
        toggles_known = false;
        ch_ok = false;
    }
    if (!feather_init_done && (cq_n == 0)) {
        push_saved();
        feather_init_done = true;
    }
    if (!toggles_known || !was_up) {
        last_toggles = st[7];
        toggles_known = true;
    } else if (st[7] != last_toggles) {
        uint8_t d = st[7] - last_toggles;
        last_toggles = st[7];
        if (d & 1) {
            radio_toggle();
        }
    }
    pump_cmds();
}

// ---- web

static String hex6(uint32_t c) {
    char b[8];
    snprintf(b, sizeof(b), "%06x", (unsigned) (c & 0xffffff));
    return String(b);
}

// fw22: the page sends its clock as "t" (Unix seconds) with every request
static void clock_sync() {
    long t = server.hasArg("t") ? server.arg("t").toInt() : 0;
    if (t > 1700000000L) {
        clk_base = (uint32_t) t - millis() / 1000;
    }
}

static uint32_t now_unix() {
    if (clk_base) {
        return clk_base + millis() / 1000;
    }
    time_t t = time(nullptr);  // NTP, once Wi-Fi has had internet
    return (t > 1700000000L) ? (uint32_t) t : 0;
}

static void touch() {
    last_http = millis();
    clock_sync();
}

static void ok_json(bool ok, const char* err = nullptr) {
    String s = ok ? "{\"ok\":true}" : String("{\"ok\":false,\"err\":\"") + (err ? err : "error") + "\"}";
    server.send(ok ? 200 : 400, "application/json", s);
}

// ---- the mouse's other settings (Feather v14+, status bytes 57-62)
// polling rate byte <-> Hz, lift-off byte (1-5) <-> tenths of a mm
static const uint8_t POLL_RAW[7] = { 0x08, 0x04, 0x02, 0x01, 0x10, 0x20, 0x40 };
static const uint16_t POLL_HZ[7] = { 125, 250, 500, 1000, 2000, 4000, 8000 };
static const uint8_t LOD_TENTHS[5] = { 7, 9, 12, 14, 16 };

static String opt_json() {
    if (!st_ok || !link_up || (st[56] < 14)) {
        return "null";  // no current status from the Feather, or its firmware predates these settings
    }
    uint8_t known = st[62];
    String o = "{\"poll\":";
    String p = "null";
    for (int i = 0; i < 7; i++) {
        if ((known & 1) && (st[57] == POLL_RAW[i])) {
            p = String(POLL_HZ[i]);
        }
    }
    o += p + ",\"lod\":";
    o += ((known & 2) && (st[58] >= 1) && (st[58] <= 5)) ? String(LOD_TENTHS[st[58] - 1] / 10.0, 1) : String("null");
    o += ",\"snap\":" + String((known & 4) ? (st[59] ? "true" : "false") : "null");
    o += ",\"sync\":" + String((known & 8) ? (st[60] ? "true" : "false") : "null");
    o += ",\"spdt_l\":" + String((known & 16) ? ((st[61] & 1) ? "true" : "false") : "null");
    o += ",\"spdt_r\":" + String((known & 16) ? ((st[61] & 2) ? "true" : "false") : "null");
    return o + "}";
}

static bool arg_int(const char* name, long lo, long hi, long* out);

static String hex_bytes(const uint8_t* p, uint8_t n) {
    String o;
    o.reserve(2 * n);
    for (uint8_t i = 0; i < n; i++) {
        char b[3];
        snprintf(b, sizeof(b), "%02x", p[i]);
        o += b;
    }
    return o;
}

// Battery, from the Warg's reply to the status query (command 04), seen 2026-10-09:
//   08 04 00 00 00 02 | 64 00 | 10 27 ...  -> 0x64 = 100 %, then 0x1027 = 4135 mV (big-endian)
// The second byte (00) is not known yet (charging?).
static String battery_json() {
    if (!st_ok || !st_ext || (st[66] < 10) || (st[67] != 8) || (st[68] != 0x04) || (st[73] > 100) || !info_at) {
        return "null";
    }
    uint16_t mv = (uint16_t) (st[75] << 8 | st[76]);
    return "{\"pct\":" + String(st[73]) + ",\"mv\":" + String(mv) + ",\"age_s\":" + String(info_age_known ? (long) ((millis() - info_at) / 1000) : -1L) + "}";
}

// the mouse's reply to the status query (each refresh) and to the last probe, as they came (report ID first)
static String ext_json() {
    if (!st_ok || !st_ext) {
        return ",\"info\":null,\"probe\":null";
    }
    uint8_t il = (st[66] <= 24) ? st[66] : 0, pl = (st[93] <= 24) ? st[93] : 0;
    static const char* const PSTATE[4] = { "none", "waiting", "answered", "no answer" };
    return ",\"info\":{\"n\":" + String(st[65]) + ",\"hex\":\"" + hex_bytes(st + 67, il) + "\"}" +
           ",\"probe\":{\"n\":" + String(st[91]) + ",\"state\":\"" + String(PSTATE[st[92] & 3]) + "\",\"cmd\":" + String(st[119]) +
           ",\"addr\":\"" + hex_bytes(st + 120, 2) + "\",\"len\":" + String(st[122]) + ",\"hex\":\"" + hex_bytes(st + 94, pl) +
           "\",\"refused\":" + String(st[118]) + ",\"rejected\":" + String(st[123]) + "}";
}

// POST /api/probe?cmd=08&addr=00e1&len=2 | cmd=04   one read or the status query, reply in /api/status "probe"
static void h_probe() {
    touch();
    if (!st_ok || !link_up) {
        return ok_json(false, "no answer from the Feather");
    }
    if (!st_ext) {
        return ok_json(false, "needs Feather firmware v14");
    }
    long cmd = strtol(server.arg("cmd").c_str(), nullptr, 16);
    long addr = strtol(server.arg("addr").c_str(), nullptr, 16);
    long len = server.arg("len").toInt();
    if (!(((cmd == 0x08) && (addr >= 0) && (addr <= 0xffff) && (len >= 1) && (len <= 10)) || ((cmd == 0x04) && (addr == 0) && (len == 0)))) {
        return ok_json(false, "cmd=08 with addr (hex) and len 1-10, or cmd=04");
    }
    uint8_t r[16] = { (uint8_t) cmd, 0, (uint8_t) (addr >> 8), (uint8_t) addr, (uint8_t) len };
    uint8_t sum = 8;
    for (int i = 0; i < 15; i++) {
        sum += r[i];
    }
    r[15] = 0x55 - sum;
    ok_json(queue_cmd(C_RAW, r, 16));
}

// POST /api/opt?k=poll&v=1000 | k=lod&v=0.7..1.6 | k=snap|sync&v=0|1 | k=spdt&v=0-3 (bit 0 left, bit 1 right)
static void h_opt() {
    touch();
    if (!st_ok || !link_up) {
        return ok_json(false, "no answer from the Feather");
    }
    if (st[56] < 14) {
        return ok_json(false, "needs Feather firmware v14");
    }
    String k = server.arg("k"), v = server.arg("v");
    uint8_t d[2];
    if (k == "poll") {
        long hz = v.toInt();
        int i = 0;
        while ((i < 7) && (POLL_HZ[i] != hz)) {
            i++;
        }
        if (i == 7) {
            return ok_json(false, "bad rate");
        }
        d[0] = 0;
        d[1] = POLL_RAW[i];
    } else if (k == "lod") {
        int tenths = (int) lround(v.toFloat() * 10);
        int i = 0;
        while ((i < 5) && (LOD_TENTHS[i] != tenths)) {
            i++;
        }
        if (i == 5) {
            return ok_json(false, "bad distance");
        }
        d[0] = 1;
        d[1] = i + 1;
    } else if ((k == "snap") || (k == "sync")) {
        long b;
        if (!arg_int("v", 0, 1, &b)) {
            return ok_json(false, "bad value");
        }
        d[0] = (k == "snap") ? 2 : 3;
        d[1] = b;
    } else if (k == "spdt") {
        long b;
        if (!arg_int("v", 1, 3, &b)) {  // both off (0) was never captured; the Feather refuses it too
            return ok_json(false, "bad value");
        }
        d[0] = 4;
        d[1] = b;
    } else {
        return ok_json(false, "unknown setting");
    }
    bool ok = queue_cmd(C_SET_OPTION, d, 2);
    if (ok) {
        log_edit(EK_OPT, k + "=" + v);
    }
    ok_json(ok);
}

// ---- chords (Feather v15+): the table it uses, as [modifier, trigger, tap, hold, hold ms] rows

static bool chords_valid(const uint8_t* r, uint8_t n) {  // as gw_chords_valid on the Feather
    if (n > CH_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t* x = r + 5 * i;
        // tap bit 7 (Feather v21): only a double tap acts; button triggers only
        uint8_t tap = x[2] & 0x7F;
        if ((x[0] < 1) || (x[0] > 5) || (x[1] < 1) || (x[1] > 6) || (x[0] == x[1]) || (tap >= CH_ACTIONS) || (x[3] >= CH_ACTIONS) ||
            ((tap == 0) && (x[3] == 0)) || ((x[1] != 6) && (x[3] != 0) && (x[4] < 10)) || ((x[2] & 0x80) && ((x[1] == 6) || (tap == 0)))) {
            return false;
        }
        for (uint8_t j = 0; j < i; j++) {
            if ((r[5 * j] == x[0]) && (r[5 * j + 1] == x[1])) {
                return false;
            }
        }
    }
    return true;
}

static String chords_json() {
    if (!chords_supported()) {
        return "null";
    }
    String o = "{\"seq\":" + String(st[124]) + ",\"flags\":" + String(st[125]) + ",\"count\":" + String(st[126]);
    o += ",\"fresh\":" + String((ch_ok && (ch[1] == st[124])) ? "true" : "false");
    o += ",\"saved_here\":" + String(prefs.isKey("chords") ? "true" : "false");
    o += ",\"rules\":";
    if (!ch_ok) {
        return o + "null}";
    }
    o += "[";
    for (uint8_t i = 0; i < ch[2]; i++) {
        const uint8_t* x = ch + 3 + 5 * i;
        o += (i ? ",[" : "[") + String(x[0]) + "," + String(x[1]) + "," + String(x[2]) + "," + String(x[3]) + "," + String(x[4] * 10) + "]";
    }
    return o + "]}";
}

// POST /api/chords?r=mod.trig.tap.hold.ms,...   the whole table (r= empty: no chords at all)
//      /api/chords?defaults=1                   back to the Feather's built-in table
// Kept on the Feather (flash) and here, so a Feather on its built-in table gets it back.
static void h_chords() {
    touch();
    if (!st_ok || !link_up) {
        return ok_json(false, "no answer from the Feather");
    }
    if (!chords_supported()) {
        return ok_json(false, "needs Feather firmware v15");
    }
    if (server.hasArg("defaults")) {
        uint8_t d[2] = { 0, 0xFF };
        if (!queue_cmd(C_SET_CHORDS, d, 2)) {
            return ok_json(false, "busy, try again");
        }
        prefs.remove("chords");
        log_edit(EK_CHORDS_DEFAULT, String());
        return ok_json(true);
    }
    if (!server.hasArg("r")) {
        return ok_json(false, "need r");
    }
    String s = server.arg("r");
    uint8_t rules[CH_MAX * 5];
    uint8_t n = 0;
    int at = 0;
    while (at < (int) s.length()) {
        int comma = s.indexOf(',', at);
        String part = (comma < 0) ? s.substring(at) : s.substring(at, comma);
        long v[5];
        char extra;
        if (sscanf(part.c_str(), "%ld.%ld.%ld.%ld.%ld%c", &v[0], &v[1], &v[2], &v[3], &v[4], &extra) != 5) {
            return ok_json(false, "each chord: modifier.trigger.tap.hold.ms");
        }
        if (n == CH_MAX) {
            return ok_json(false, "at most 12 chords");
        }
        if ((v[0] < 0) || (v[0] > 255) || (v[1] < 0) || (v[1] > 255) || (v[2] < 0) || (v[2] > 255) || (v[3] < 0) || (v[3] > 255) ||
            (v[4] < 0) || (v[4] > 2550)) {
            return ok_json(false, "a value is out of range");
        }
        uint8_t* x = rules + 5 * n;
        x[0] = v[0];
        x[1] = v[1];
        x[2] = v[2];
        x[3] = v[3];
        x[4] = (uint8_t) ((v[4] + 5) / 10);
        n++;
        if (comma < 0) {
            break;
        }
        at = comma + 1;
    }
    if (!chords_valid(rules, n)) {
        return ok_json(false, "invalid chord table");
    }
    if (!queue_chords(rules, n)) {
        return ok_json(false, "busy, try again");
    }
    uint8_t c[1 + CH_MAX * 5];
    c[0] = n;
    memcpy(c + 1, rules, n * 5);
    prefs.putBytes("chords", c, 1 + n * 5);
    log_edit(EK_CHORDS, String(n) + (n == 1 ? " rule" : " rules"));
    ok_json(true);
}

// not counted as use: an open tab alone shouldn't keep the radio on while playing
static void h_status() {
    clock_sync();
    String j;
    j.reserve(2200);
    j += "{\"link\":";
    j += link_up ? "true" : "false";
    if (st_ok) {
        uint8_t f = st[3];
        j += ",\"fw\":" + String(st[56]);
        j += ",\"warg\":" + String((f & 1) ? "true" : "false");
        j += ",\"vuk\":" + String((f & 2) ? "true" : "false");
        j += ",\"reads\":" + String((f & 4) ? "true" : "false");
        j += ",\"stage\":" + String((f & 8) ? String(st[4]) : String("null"));
        j += ",\"angle\":" + String((f & 16) ? String(s8(5)) : String("null"));
        j += ",\"pos\":" + String((f & 32) ? String(s8(6)) : String("null"));
        // Feather v20+: whether the mouse confirmed them (read, or a write read back), or a write waits for it to wake (byte 117)
        if (st_ext && (st[56] >= 20)) {
            uint8_t c = st[117];
            j += String(",\"sens\":{\"angle_ok\":") + ((c & 1) ? "true" : "false") + ",\"pos_ok\":" + ((c & 2) ? "true" : "false") +
                 ",\"angle_wait\":" + ((c & 4) ? "true" : "false") + ",\"pos_wait\":" + ((c & 8) ? "true" : "false") + "}";
        }
        j += ",\"colours_read\":" + String((f & 64) ? "true" : "false");
        j += ",\"dpi_known\":" + String((f & 128) ? "true" : "false");
        j += ",\"refreshing\":" + String((st[2] & 4) ? "true" : "false");
        j += ",\"paused\":" + String((st[2] & 8) ? "true" : "false");  // Feather v13+: the last read timed out
        j += ",\"palette\":[";
        for (int i = 0; i < 5; i++) {
            uint32_t c = (uint32_t) st[8 + 3 * i] << 16 | st[9 + 3 * i] << 8 | st[10 + 3 * i];
            j += (i ? ",\"" : "\"") + hex6(c) + "\"";
        }
        j += "],\"dpi\":[";
        for (int i = 0; i < 5; i++) {
            j += (i ? ",[" : "[") + String(u16(23 + 4 * i)) + "," + String(u16(25 + 4 * i)) + "]";
        }
        j += "],\"tune\":{\"angle_step\":" + String(st[43]) + ",\"pos_step\":" + String(st[44]) +
             ",\"angle_home\":" + String(s8(45)) + ",\"pos_home\":" + String(s8(46)) +
             ",\"flash_ms\":" + String(u16(47)) + ",\"pos_hold_ms\":" + String(u16(49)) +
             ",\"wheel_hold_ms\":" + String(u16(51)) + ",\"radio_hold_ms\":" + String(u16(53)) + "}";
    }
    j += ",\"opt\":" + opt_json();
    j += ",\"chords\":" + chords_json();
    // Feather v21+: recent actions, newest first: [age s, source, buttons, action, value]
    if (events_supported() && ev_ok) {
        uint32_t extra = (millis() - ev_read_at) / 1000;
        j += ",\"events\":[";
        for (uint8_t k = 0; k < evb[1]; k++) {
            const uint8_t* q = evb + 2 + 6 * k;
            j += (k ? ",[" : "[") + String((uint32_t) (q[0] | q[1] << 8) + extra) + "," + String(q[2]) + "," + String(q[3]) + "," + String(q[4]) + "," +
                 String((int8_t) q[5]) + "]";
        }
        j += "]";
    } else {
        j += ",\"events\":null";
    }
    // fw22: the saved page edits, newest first: [Unix time or 0, age s (this boot only, else -1), kind, device, "ip", "text"]
    j += ",\"hcap\":" + String(hcap_seq) + ",\"hcap_n\":" + String(hcap_n);  // fw23: /api/hosts changed / how many
    {  // fw26: direction gains: the table, its hosts, and the Feather's copy (sup = Feather v24+, on = applying now)
        j += ",\"dg\":{\"g\":[";
        for (int k = 0; k < 8; k++) {
            j += (k ? "," : "") + String(dg.g[k]);
        }
        j += "],\"hosts\":[";
        char h[12];
        for (uint8_t k = 0; k < dg.n; k++) {
            snprintf(h, sizeof(h), "%08lx", (unsigned long) dg.sig[k]);
            j += (k ? ",\"" : "\"") + String(h) + "\"";
        }
        bool sup = dg_supported() && ev_ok;
        j += "],\"sup\":" + String(dg_supported() ? "true" : "false") + ",\"on\":" + String(sup && (evb[50] & 1) ? "true" : "false") +
             ",\"got\":" + String(sup && (evb[50] & 2) ? "true" : "false") + ",\"want\":" + String(dg_wanted() ? "true" : "false") +
             ",\"sends\":" + String(dg_sends) + "}";
    }
    j += String(",\"tdpi\":{\"on\":") + (tdpi_active ? "true" : "false") + ",\"dpi\":" + String(tdpi_val) + ",\"restoring\":" + (tdpi_restore ? "true" : "false") + "}";  // fw28: the calibration's temporary DPI
    {
        char h[12];  // fw24: this power-on's host signature, the automatic stage's state (auto_state) and stage (1-5, 0 none)
        snprintf(h, sizeof(h), "%08lx", (unsigned long) auto_sig);
        j += ",\"auto\":[\"" + String(h) + "\"," + String(auto_state) + "," + String(auto_want == 0xFF ? 0 : auto_want + 1) + "]";
        // fw25: every known host for the DPI stage rows: [sig, stage 0-5, name ("" = not named: guess)], profiles first,
        // then hosts only seen in the captures; the guess is from the first request (Windows asks 64 bytes, the XIM 8)
        j += ",\"hosts\":[";
        uint8_t m = 0;
        for (uint8_t k = 0; k < hprof_n; k++) {
            snprintf(h, sizeof(h), "%08lx", (unsigned long) hprof[k].sig);
            j += (m++ ? ",[\"" : "[\"") + String(h) + "\"," + String(hprof[k].stage) + ",\"" + String(hprof[k].name) + "\"]";
        }
        uint32_t done[HCAP_KEEP];
        uint8_t nd = 0;
        for (uint8_t k = 0; k < hcap_n; k++) {
            uint32_t sig = hcap_sig(hcap[k]);
            bool seen = !sig || (hprof_find(sig) >= 0);
            for (uint8_t d = 0; (d < nd) && !seen; d++) {
                seen = (done[d] == sig);
            }
            if (seen) {
                continue;
            }
            done[nd++] = sig;
            uint8_t first = 0;
            hcap_reqs(hcap[k], &first);
            snprintf(h, sizeof(h), "%08lx", (unsigned long) sig);
            j += (m++ ? ",[\"" : "[\"") + String(h) + "\",0,\"\",\"" + (first == 64 ? "PC" : first == 8 ? "XIM" : "Host") + "\"]";
        }
        j += "]";
    }
    j += ",\"now\":" + String(now_unix()) + ",\"edits\":[";
    for (uint8_t k = 0; k < edit_n; k++) {
        const EditLog& e = edit_log[k];
        long age = (e.boot == (uint16_t) b_boots) ? (long) ((millis() - e.ms) / 1000) : -1L;
        j += (k ? ",[" : "[") + String(e.t) + "," + String(age) + "," + String(e.kind) + "," + String(e.dev) + ",\"" + IPAddress(e.ip).toString() +
             "\",\"" + String(e.text) + "\"]";
    }
    j += "]";
    // Feather v15+: its processing time per 1 ms frame over the last second (status bytes 84-89); v19+ also the
    // whole frame: ticks merged (frames never processed, 90 and 111-112) and, in 4 us steps, how long after its tick each
    // frame was done (113-114) and the SOF interrupt (115-116)
    if (chords_supported()) {
        j += ",\"perf\":{\"avg_us\":" + String(u16(84) / 10.0, 1) + ",\"max_us\":" + String(u16(86)) + ",\"chords_avg_us\":" +
             String(st[88] / 10.0, 1) + ",\"chords_max_us\":" + String(st[89]);
        if (st[56] >= 19) {
            j += ",\"missed_1s\":" + String(st[90]) + ",\"missed_total\":" + String(u16(111)) + ",\"tick_avg_us\":" +
                 String(st[113] * 4) + ",\"tick_max_us\":" + String(st[114] * 4) + ",\"usb_avg_us\":" + String(st[115] * 4) +
                 ",\"usb_max_us\":" + String(st[116] * 4);
        }
        j += "}";
    } else {
        j += ",\"perf\":null";
    }
    j += ext_json();
    j += ",\"battery\":" + battery_json();
    j += ",\"qt_fw\":" + String(QT_FW);
    j += ",\"diag\":{\"polls\":" + String(d_polls) + ",\"ok\":" + String(d_ok) + ",\"short\":" + String(d_short) +
         ",\"bad\":" + String(d_bad) + ",\"i2c_resets\":" + String(d_i2c_resets) + ",\"last_n\":" + String(d_last_n) + ",\"b0\":" + String(d_b0) + ",\"b1\":" + String(d_b1) +
         ",\"up_s\":" + String(millis() / 1000) + "}";
    j += ",\"pending\":" + String(cq_n);
    j += ",\"radio\":{\"mode\":\"";
    j += (radio == Radio::STA) ? "sta" : (radio == Radio::AP) ? "ap" : (radio == Radio::JOINING) ? "joining" : "off";
    j += "\",\"ssid\":\"" + json_str(prefs.getString("ssid", "")) + "\"";
    j += ",\"ip\":\"" + ((radio == Radio::AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"";
    uint32_t lim = idle_limit();
    long left = !lim ? -1 : (millis() - last_http < lim) ? (long) ((lim - (millis() - last_http)) / 1000) : 0;
    j += ",\"keep\":" + String(wmode);
    j += ",\"off_in_s\":" + String(left) + "}";
    bool up = (WiFi.status() == WL_CONNECTED);
    j += ",\"ui\":" + String(ui_folds);
    j += ",\"wifi\":{\"rssi\":" + String(up ? WiFi.RSSI() : 0) + ",\"ch\":" + String(up ? WiFi.channel() : 0) +
         ",\"bssid\":\"" + (up ? WiFi.BSSIDstr() : String("")) + "\",\"sleep\":" + String(wsleep ? "true" : "false") +
         ",\"drops\":" + String(w_disc) + ",\"drop_reason\":" + String(w_disc_reason) +
         ",\"drop_ago_s\":" + String(w_disc ? (long) ((millis() - w_disc_at) / 1000) : -1) + "}";
    j += ",\"boot\":{\"reason\":\"" + String(reset_name(b_reason)) + "\",\"boots\":" + String(b_boots) +
         ",\"brownouts\":" + String(b_brownouts) + ",\"crashes\":" + String(b_crashes) +
         ",\"heap\":" + String(ESP.getFreeHeap()) + ",\"heap_min\":" + String(ESP.getMinFreeHeap()) + "}}";
    server.send(200, "application/json", j);
}

static bool arg_int(const char* name, long lo, long hi, long* out) {
    if (!server.hasArg(name)) {
        return false;
    }
    String s = server.arg(name);
    char* end;
    long v = strtol(s.c_str(), &end, 10);
    if ((end == s.c_str()) || (*end != 0) || (v < lo) || (v > hi)) {
        return false;
    }
    *out = v;
    return true;
}

// fw21: the kind of device that sent this request (SetLog.dev), from its User-Agent
static uint8_t ua_device() {
    String ua = server.header("User-Agent");
    if (!ua.length() || (ua.indexOf("PowerShell") >= 0) || (ua.indexOf("curl") >= 0) || (ua.indexOf("python") >= 0) || (ua.indexOf("Wget") >= 0)) {
        return 6;
    }
    if (ua.indexOf("Android") >= 0) {
        return 2;
    }
    if ((ua.indexOf("iPhone") >= 0) || (ua.indexOf("iPad") >= 0)) {
        return 3;
    }
    if (ua.indexOf("Windows") >= 0) {
        return 1;
    }
    if (ua.indexOf("Mac OS") >= 0) {
        return 4;
    }
    return (ua.indexOf("Linux") >= 0) ? 5 : 0;
}

// fw22: record an edit the Feather accepted into its queue, newest first, and save the log
static void log_edit_as(uint8_t kind, const String& text, uint8_t dev, uint32_t ip) {
    memmove(edit_log + 1, edit_log, sizeof(EditLog) * (EDIT_LOG - 1));
    EditLog& e = edit_log[0];
    memset(&e, 0, sizeof(e));
    e.t = now_unix();
    e.ms = millis();
    e.ip = ip;
    e.boot = (uint16_t) b_boots;
    e.kind = kind;
    e.dev = dev;
    strncpy(e.text, text.c_str(), sizeof(e.text) - 1);
    for (char* c = e.text; *c; c++) {
        if ((*c < 32) || (*c > 126) || (*c == '"') || (*c == '\\')) {
            *c = '?';  // it goes into the status JSON as is
        }
    }
    if (edit_n < EDIT_LOG) {
        edit_n++;
    }
    prefs.putBytes("edits", edit_log, sizeof(EditLog) * edit_n);
}

static void log_edit(uint8_t kind, const String& text) {
    log_edit_as(kind, text, ua_device(), (uint32_t) server.client().remoteIP());
}

static void set_cmd(uint8_t cmd, long v) {
    uint8_t b = (uint8_t) (int8_t) v;
    bool ok = queue_cmd(cmd, &b, 1);
    if (ok) {
        log_edit(cmd, String(v));  // EK_STAGE/ANGLE/POS are the command numbers
    }
    ok_json(ok);
}

static uint8_t hcap_reqs(const HostCap& c, uint8_t* first_len) {  // requests other than bus resets
    uint8_t n = c.blk[3] <= HCAP_ENTRIES ? c.blk[3] : HCAP_ENTRIES, k = 0;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t* e = c.blk + 16 + 8 * i;
        if ((e[0] == 0xFF) && (e[1] == 0xFF)) {
            continue;
        }
        if (!k && first_len) {
            *first_len = e[6];
        }
        k++;
    }
    return k;
}

static uint32_t hcap_sig(const HostCap& c) {
    uint8_t n = c.blk[3] <= HCAP_ENTRIES ? c.blk[3] : HCAP_ENTRIES, k = 0;
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; (i < n) && (k < HSIG_REQS); i++) {
        const uint8_t* e = c.blk + 16 + 8 * i;
        if ((e[0] == 0xFF) && (e[1] == 0xFF)) {
            continue;
        }
        bool addr = (e[0] == 0x00) && (e[1] == 0x05);
        for (uint8_t b = 0; b < 8; b++) {
            h = (h ^ ((addr && (b == 2 || b == 3)) ? 0 : e[b])) * 16777619u;
        }
        k++;
    }
    return (k < HSIG_REQS) ? 0 : (h ? h : 1);
}

static int hprof_find(uint32_t sig) {  // index, -1 = none (an int: Arduino's prototypes can't name HostProf)
    for (uint8_t k = 0; k < hprof_n; k++) {
        if (hprof[k].sig == sig) {
            return k;
        }
    }
    return -1;
}

static HostProf* hprof_at(int k) {
    return (k >= 0) ? &hprof[k] : nullptr;
}

static void auto_tick() {
    if (!auto_sig && hcap_n && (hcap[0].boot == (uint16_t) b_boots)) {
        auto_sig = hcap_sig(hcap[0]);
        if (auto_sig) {
            HostProf* p = hprof_at(hprof_find(auto_sig));
            auto_want = (p && p->stage >= 1 && p->stage <= 5) ? p->stage - 1 : 0xFF;
            auto_state = (auto_want == 0xFF) ? 4 : 0;
        }
    }
    if ((auto_want == 0xFF) || (auto_state >= 2) || !link_up || !st_ok || !(st[3] & 1)) {
        return;  // nothing to set, done, or the Warg isn't there yet
    }
    if ((auto_state == 1) && (st[4] == auto_want)) {
        auto_state = 2;
        HostProf* p = hprof_at(hprof_find(auto_sig));
        log_edit_as(EK_AUTO_STAGE, String(p ? p->name : "") + "=" + String(auto_want), 7, 0);
        return;
    }
    if ((cq_n == 0) && (millis() - auto_at >= 1500)) {
        if (auto_tries >= 6) {
            auto_state = 3;
            return;
        }
        uint8_t b = auto_want;
        if (queue_cmd(C_SET_STAGE, &b, 1)) {
            auto_tries++;
            auto_at = millis();
            auto_state = 1;
        }
    }
}

// GET /api/hosts (fw23): the saved captures, newest first, with each one's signature (fw24); the page decodes the
// requests. fw24 also: the stage profiles and what happened on this power-on.
static void h_hosts() {
    clock_sync();
    String j;
    j.reserve(600 * (hcap_n + 1) + 40 * hprof_n);
    char h[12];
    j += "{\"boot\":" + String((uint16_t) b_boots) + ",\"now\":" + String(now_unix());
    snprintf(h, sizeof(h), "%08lx", (unsigned long) auto_sig);
    j += ",\"auto\":{\"sig\":\"" + String(h) + "\",\"stage\":" + String(auto_want == 0xFF ? 0 : auto_want + 1) + ",\"state\":" +
         String(auto_state) + ",\"tries\":" + String(auto_tries) + "},\"profiles\":[";
    for (uint8_t k = 0; k < hprof_n; k++) {
        snprintf(h, sizeof(h), "%08lx", (unsigned long) hprof[k].sig);
        j += (k ? ",{\"sig\":\"" : "{\"sig\":\"") + String(h) + "\",\"stage\":" + String(hprof[k].stage) + ",\"name\":\"" + String(hprof[k].name) + "\"}";
    }
    j += "],\"caps\":[";
    for (uint8_t k = 0; k < hcap_n; k++) {
        const HostCap& c = hcap[k];
        uint8_t n = c.blk[3] <= HCAP_ENTRIES ? c.blk[3] : HCAP_ENTRIES, first = 0;
        hcap_reqs(c, &first);
        snprintf(h, sizeof(h), "%08lx", (unsigned long) hcap_sig(c));
        j += k ? ",{" : "{";
        j += "\"t\":" + String(c.t) + ",\"boot\":" + String(c.boot) + ",\"id\":" + String(c.blk[1]) + ",\"resets\":" + String(c.blk[4]) +
             ",\"cfg_ms\":" + String(c.blk[5] | c.blk[6] << 8) + ",\"seen\":" + String(c.blk[7] | c.blk[8] << 8) + ",\"flags\":" + String(c.blk[9]) +
             ",\"sig\":\"" + String(h) + "\",\"req\":\"";
        char x[3];
        for (uint16_t i = 16; i < 16 + 8 * n; i++) {
            snprintf(x, sizeof(x), "%02x", c.blk[i]);
            j += x;
        }
        j += "\"}";
    }
    j += "]}";
    server.send(200, "application/json", j);
}

// POST /api/hostprof?sig=8 hex&stage=0-5&name=... (fw24): set a host's stage and name; stage 0 with no name removes it
static void h_hostprof() {
    touch();
    uint32_t sig = strtoul(server.arg("sig").c_str(), nullptr, 16);
    long stage;
    if (!sig || !arg_int("stage", 0, 5, &stage)) {
        return ok_json(false, "need sig and stage 0-5");
    }
    String name = server.arg("name");
    name.trim();
    HostProf* p = hprof_at(hprof_find(sig));
    if (!stage && !name.length()) {
        if (p) {
            *p = hprof[hprof_n - 1];
            hprof_n--;
        }
    } else {
        if (!p) {
            if (hprof_n == HPROF_MAX) {
                return ok_json(false, "8 hosts at most: clear one first");
            }
            p = &hprof[hprof_n++];
            p->sig = sig;
        }
        p->stage = (uint8_t) stage;
        memset(p->name, 0, sizeof(p->name));
        for (uint8_t i = 0; (i < name.length()) && (i < sizeof(p->name) - 1); i++) {
            char c = name[i];
            p->name[i] = ((c < 32) || (c > 126) || (c == '"') || (c == '\\') || (c == '=')) ? '?' : c;
        }
    }
    prefs.putBytes("hprof", hprof, sizeof(HostProf) * hprof_n);
    log_edit(EK_HOST_STAGE, String(p && (stage || name.length()) ? p->name : "") + "=" + String(stage));
    if (sig == auto_sig) {  // the host on now: apply the new choice this power-on too
        auto_want = (stage >= 1) ? stage - 1 : 0xFF;
        auto_state = (auto_want == 0xFF) ? 4 : 0;
        auto_tries = 0;
        auto_at = 0;
    }
    ok_json(true);
}

// ---- fw26: direction gains
static bool dg_supported() {
    return st_ok && st_ext && (st[56] >= 24);
}

static bool dg_wanted() {  // on for the host connected now
    for (uint8_t k = 0; auto_sig && (k < dg.n); k++) {
        if (dg.sig[k] == auto_sig) {
            return true;
        }
    }
    return false;
}

static bool dg_flat() {
    for (int k = 0; k < 8; k++) {
        if (dg.g[k] != 1000) {
            return false;
        }
    }
    return true;
}

static uint8_t dg_crc() {  // as the Feather computes it: crc8 of the 16 gain bytes, little-endian
    uint8_t raw[16];
    for (int k = 0; k < 8; k++) {
        raw[2 * k] = dg.g[k] & 0xFF;
        raw[2 * k + 1] = dg.g[k] >> 8;
    }
    return crc8(raw, 16);
}

// the Feather's copy is in status 242-243, which come with the 192-255 block: read only when its change nibble moves,
// so after sending, read it once more ourselves
static void dg_tick() {
    if (!link_up || !dg_supported() || (cq_n > 0)) {
        return;
    }
    if (dg_reread) {
        if (millis() - dg_sent_at >= 300) {
            dg_reread = false;
            read_chords();
        }
        return;
    }
    if (!ev_ok) {
        return;
    }
    uint8_t f = evb[50];
    bool want = dg_wanted(), expect_on = want && !dg_flat();
    bool ok = (f & 2) && (evb[51] == dg_crc()) && (((f & 1) != 0) == expect_on);
    if (ok || (millis() - dg_sent_at < 1500)) {
        return;
    }
    uint8_t d[17];
    d[0] = want ? 1 : 0;
    for (int k = 0; k < 8; k++) {
        d[1 + 2 * k] = dg.g[k] & 0xFF;
        d[2 + 2 * k] = dg.g[k] >> 8;
    }
    if (queue_cmd(C_SET_DIRGAIN, d, 17)) {
        dg_sent_at = millis();
        dg_reread = true;
        dg_sends++;
    }
}

// POST /api/dirgain?g=g0,...,g7 (per mille, 500-2000)&hosts=sig,sig (8 hex each; empty = off everywhere)
static void h_dirgain() {
    touch();
    uint16_t g[8];
    String s = server.arg("g");
    int at = 0;
    for (int k = 0; k < 8; k++) {
        int comma = s.indexOf(',', at);
        long v = ((comma < 0) ? s.substring(at) : s.substring(at, comma)).toInt();
        if ((v < 500) || (v > 2000) || ((comma < 0) && (k < 7))) {
            return ok_json(false, "need 8 gains, 500-2000 per mille");
        }
        g[k] = v;
        at = comma + 1;
    }
    uint32_t sig[HPROF_MAX];
    uint8_t n = 0;
    String h = server.arg("hosts");
    at = 0;
    while (at < (int) h.length()) {
        int comma = h.indexOf(',', at);
        String one = (comma < 0) ? h.substring(at) : h.substring(at, comma);
        uint32_t v = strtoul(one.c_str(), nullptr, 16);
        if (v && (n < HPROF_MAX)) {
            sig[n++] = v;
        }
        if (comma < 0) {
            break;
        }
        at = comma + 1;
    }
    memcpy(dg.g, g, sizeof(g));
    dg.n = n;
    memset(dg.sig, 0, sizeof(dg.sig));
    memcpy(dg.sig, sig, sizeof(uint32_t) * n);
    prefs.putBytes("dgain", &dg, sizeof(dg));
    log_edit(EK_DIRGAIN, String(n) + (n == 1 ? " host" : " hosts"));
    dg_sent_at = 0;  // send now
    ok_json(true);
}

// POST /api/calib?n=8 counts&med=8 per mille&g=8 per mille&f=flags&s=skipped (fw27): a calibration run's summary
static bool csv8(const String& s, long lo, long hi, long* out) {
    int at = 0;
    for (int k = 0; k < 8; k++) {
        int comma = s.indexOf(',', at);
        long v = ((comma < 0) ? s.substring(at) : s.substring(at, comma)).toInt();
        if ((v < lo) || (v > hi) || ((comma < 0) && (k < 7))) {
            return false;
        }
        out[k] = v;
        at = comma + 1;
    }
    return true;
}

static void h_calib_post() {
    touch();
    long n[8], med[8], g[8], fl = server.arg("f").toInt(), sk = server.arg("s").toInt();
    if (!csv8(server.arg("n"), 0, 255, n) || !csv8(server.arg("med"), 0, 3000, med) || !csv8(server.arg("g"), 500, 2000, g)) {
        return ok_json(false, "need n, med and g: 8 numbers each");
    }
    memmove(calib + 1, calib, sizeof(CalibRec) * (CALIB_KEEP - 1));
    if (calib_n < CALIB_KEEP) {
        calib_n++;
    }
    CalibRec& c = calib[0];
    c.t = now_unix();
    for (int k = 0; k < 8; k++) {
        c.n[k] = n[k];
        c.med[k] = med[k];
        c.g[k] = g[k];
    }
    c.flags = fl & 0xFF;
    c.skipped = (sk < 0) ? 0 : (sk > 255) ? 255 : sk;
    prefs.putBytes("calib", calib, sizeof(CalibRec) * calib_n);
    ok_json(true);
}

// GET /api/calib (fw27): the saved summaries, newest first
static void h_calib_get() {
    String j = "{\"runs\":[";
    for (uint8_t k = 0; k < calib_n; k++) {
        const CalibRec& c = calib[k];
        j += (k ? ",{\"t\":" : "{\"t\":") + String(c.t) + ",\"flags\":" + String(c.flags) + ",\"skipped\":" + String(c.skipped) + ",\"n\":[";
        for (int d = 0; d < 8; d++) {
            j += (d ? "," : "") + String(c.n[d]);
        }
        j += "],\"med\":[";
        for (int d = 0; d < 8; d++) {
            j += (d ? "," : "") + String(c.med[d]);
        }
        j += "],\"g\":[";
        for (int d = 0; d < 8; d++) {
            j += (d ? "," : "") + String(c.g[d]);
        }
        j += "]}";
    }
    j += "]}";
    server.send(200, "application/json", j);
}

// ---- fw28: temporary DPI for the flick calibration
static bool tdpi_table_is(const uint16_t* v) {
    for (int i = 0; i < 5; i++) {
        if ((u16(23 + 4 * i) != v[2 * i]) || (u16(25 + 4 * i) != v[2 * i + 1])) {
            return false;
        }
    }
    return true;
}

static bool tdpi_send(const uint16_t* v) {
    uint8_t d[20];
    for (int i = 0; i < 10; i++) {
        d[2 * i] = v[i] & 0xFF;
        d[2 * i + 1] = v[i] >> 8;
    }
    return queue_cmd(C_SET_DPI, d, 20);
}

// POST /api/tempdpi?dpi=50-50000 (lower the current stage's DPI; Y keeps its ratio to X) | keep=1 (the page is still
// testing) | restore=1 (done: put the table back)
static void h_tempdpi() {
    touch();
    if (server.hasArg("restore")) {
        if (tdpi_active) {
            tdpi_restore = true;
            tdpi_sent_at = 0;
            tdpi_phase = 0;
        }
        return ok_json(true);
    }
    if (server.hasArg("keep")) {
        tdpi_keep_at = millis();
        return ok_json(tdpi_active && !tdpi_restore);
    }
    long v;
    if (!arg_int("dpi", 50, 50000, &v)) {
        return ok_json(false, "dpi 50-50000");
    }
    if (!link_up || !st_ok || !(st[3] & 128) || (st[4] > 4)) {
        return ok_json(false, "the DPI table isn't known yet: move the mouse, then Read from mouse");
    }
    if (tdpi_restore) {
        return ok_json(false, "still putting the last test's DPI back: try again in a moment");
    }
    if (cq_n > 0) {
        return ok_json(false, "busy, try again");  // a queued DPI or stage change would make the copy below stale
    }
    if (!tdpi_active) {
        tdpi_stage = st[4];
        prefs.putUChar("tdpis", tdpi_stage);
        for (int i = 0; i < 5; i++) {
            tdpi_orig[2 * i] = u16(23 + 4 * i);
            tdpi_orig[2 * i + 1] = u16(25 + 4 * i);
        }
        prefs.putBytes("tdpi", tdpi_orig, sizeof(tdpi_orig));
    }
    uint16_t tb[10];
    memcpy(tb, tdpi_orig, sizeof(tb));
    uint8_t s = st[4];
    long ox = tdpi_orig[2 * s], oy = tdpi_orig[2 * s + 1];
    if ((ox > 0) && (oy > 0) && (oy < ox) && (v * oy < 50L * ox)) {
        v = (50L * ox + oy - 1) / oy;  // fw29: keep Y at 50 or more without bending the X/Y ratio
    }
    long vy = ox ? lround((double) v * oy / ox) : v;
    tb[2 * s] = v;
    tb[2 * s + 1] = (vy < 50) ? 50 : (vy > 50000) ? 50000 : vy;
    if (!tdpi_send(tb)) {
        if (!tdpi_active) {
            prefs.remove("tdpi");
        }
        return ok_json(false, "busy, try again");
    }
    tdpi_active = true;
    tdpi_val = v;
    tdpi_keep_at = millis();
    log_edit(EK_TEMP_DPI, String(v));
    ok_json(true);
}

// owed a restore: send the original table, then read the test stage's register straight from the mouse (0x1b00 + 6 per
// stage: X-1, Y-1, 16-bit LE); done only when the mouse itself reports the original. A lost write or a sleeping mouse
// just means another round (every 2-5 s) with the saved copy kept. After a power-on the stage is read first, so the
// Feather re-selects it and the mouse runs at the restored DPI, not only stores it.
static void tdpi_probe_send() {
    uint16_t addr = 0x1b00 + 6 * tdpi_stage;
    uint8_t r[16] = { 0x08, 0, (uint8_t) (addr >> 8), (uint8_t) addr, 4 };
    uint8_t sum = 8;
    for (int i = 0; i < 15; i++) {
        sum += r[i];
    }
    r[15] = 0x55 - sum;
    tdpi_probe_n = st[91];
    if (queue_cmd(C_RAW, r, 16)) {
        tdpi_phase = 2;
        tdpi_phase_at = millis();
    }
}

static void tdpi_tick() {
    if (tdpi_active && !tdpi_restore && (millis() - tdpi_keep_at > 12000)) {
        tdpi_restore = true;
        tdpi_sent_at = 0;
        tdpi_phase = 0;
    }
    if (!tdpi_restore || !link_up || !st_ok || !st_ext || !(st[3] & 1) || (cq_n > 0)) {
        return;
    }
    if (!(st[3] & 8)) {  // stage not known yet (just powered on): read the mouse first
        if (millis() - tdpi_read_at >= 5000) {
            tdpi_read_at = millis();
            queue_cmd(C_REFRESH, nullptr, 0);
        }
        return;
    }
    if (tdpi_stage > 4) {
        tdpi_stage = st[4];  // a fw28 test saved no stage: check the one in use
    }
    if (tdpi_phase == 0) {
        if ((millis() - tdpi_sent_at >= 2000) && tdpi_send(tdpi_orig)) {
            tdpi_sent_at = millis();
            tdpi_phase = 1;
            tdpi_phase_at = millis();
        }
    } else if (tdpi_phase == 1) {
        if (millis() - tdpi_phase_at >= 300) {
            tdpi_probe_send();
        }
    } else {
        uint8_t state = st[92] & 3;
        if ((st[91] == tdpi_probe_n) || (state < 2)) {
            if (millis() - tdpi_phase_at > 3000) {
                tdpi_phase = 0;
            }
            return;
        }
        uint16_t addr = 0x1b00 + 6 * tdpi_stage;
        const uint8_t* d = st + 94 + 6;
        bool mine = (state == 2) && (st[93] >= 10) && (st[94 + 3] == (addr >> 8)) && (st[94 + 4] == (addr & 0xFF));
        uint16_t x = (d[0] | d[1] << 8) + 1, y = (d[2] | d[3] << 8) + 1;
        if (mine && (x == tdpi_orig[2 * tdpi_stage]) && (y == tdpi_orig[2 * tdpi_stage + 1])) {
            tdpi_restore = false;
            tdpi_active = false;
            tdpi_phase = 0;
            prefs.remove("tdpi");
            prefs.remove("tdpis");
            log_edit_as(EK_TEMP_DPI, "restored", 7, 0);
            return;
        }
        tdpi_phase = 0;  // not there yet: send it again
    }
}

// POST /api/calibdiag (body: text, 4000 chars at most) | GET: the page's last calibration diagnostics, RAM only (fw29)
static void h_calibdiag_post() {
    touch();
    String d = server.arg("plain");
    calib_diag = d.substring(0, 4000);
    ok_json(true);
}

static void h_calibdiag_get() {
    server.send(200, "text/plain", calib_diag.length() ? calib_diag : String("none"));
}
// /api/set?stage=0..4 | angle=-30..30 | pos=-100..101
static void h_set() {
    touch();
    long v;
    if (arg_int("stage", 0, 4, &v)) {
        return set_cmd(C_SET_STAGE, v);
    }
    if (arg_int("angle", -30, 30, &v)) {
        return set_cmd(C_SET_ANGLE, v);
    }
    if (arg_int("pos", -100, 101, &v)) {
        return set_cmd(C_SET_POS, v);
    }
    ok_json(false, "bad value");
}

static bool parse_colours(const String& s, uint8_t* out) {  // "rrggbb,rrggbb,..." x5
    int at = 0;
    for (int i = 0; i < 5; i++) {
        if ((int) s.length() < at + 6) {
            return false;
        }
        uint32_t c = strtoul(s.substring(at, at + 6).c_str(), nullptr, 16);
        out[3 * i] = c >> 16;
        out[3 * i + 1] = c >> 8;
        out[3 * i + 2] = c;
        at += 7;
    }
    return true;
}

// /api/palette?c=rrggbb,rrggbb,rrggbb,rrggbb,rrggbb
static void h_palette() {
    touch();
    uint8_t p[15];
    if (!parse_colours(server.arg("c"), p)) {
        return ok_json(false, "need 5 colours");
    }
    prefs.putBytes("pal", p, 15);
    bool ok = queue_cmd(C_SET_PALETTE, p, 15);
    if (ok) {
        log_edit(EK_COLOURS, String());
    }
    ok_json(ok);
}

// /api/dpi?v=x1,y1,x2,y2,...  (10 numbers, 50..50000)
static void h_dpi() {
    touch();
    if (tdpi_active) {
        return ok_json(false, "a calibration has the DPI lowered for its test: finish it first (the table comes back by itself)");
    }
    String s = server.arg("v");
    uint8_t d[20];
    int at = 0;
    for (int i = 0; i < 10; i++) {
        int comma = s.indexOf(',', at);
        String part = (comma < 0) ? s.substring(at) : s.substring(at, comma);
        long v = part.toInt();
        if ((v < 50) || (v > 50000) || ((comma < 0) && (i < 9))) {
            return ok_json(false, "need 10 values 50..50000");
        }
        d[2 * i] = v & 0xff;
        d[2 * i + 1] = v >> 8;
        at = comma + 1;
    }
    bool ok = queue_cmd(C_SET_DPI, d, 20);
    if (ok) {
        log_edit(EK_DPI, String());
    }
    ok_json(ok);
}

// /api/tune?as=&ps=&ah=&ph=&fl=&poh=&wh=&rh=
static void h_tune() {
    touch();
    long as, ps, ah, ph, fl, poh, wh, rh;
    if (!(arg_int("as", 1, 10, &as) && arg_int("ps", 1, 25, &ps) && arg_int("ah", -30, 30, &ah) && arg_int("ph", -100, 101, &ph) &&
            arg_int("fl", 100, 5000, &fl) && arg_int("poh", 150, 3000, &poh) && arg_int("wh", 0, 3000, &wh) && arg_int("rh", 200, 3000, &rh))) {
        return ok_json(false, "a value is out of range");
    }
    uint8_t t[12] = { (uint8_t) as, (uint8_t) ps, (uint8_t) (int8_t) ah, (uint8_t) (int8_t) ph,
        (uint8_t) (fl & 0xff), (uint8_t) (fl >> 8), (uint8_t) (poh & 0xff), (uint8_t) (poh >> 8),
        (uint8_t) (wh & 0xff), (uint8_t) (wh >> 8), (uint8_t) (rh & 0xff), (uint8_t) (rh >> 8) };
    prefs.putBytes("tune", t, 12);
    bool ok = queue_cmd(C_SET_TUNING, t, 12);
    if (ok) {
        log_edit(EK_TUNE, String());
    }
    ok_json(ok);
}

// ?auto=1 (fw22): the page reading by itself while it lacks the angle or position. Not counted as use, like a status
// poll, so an open tab doesn't keep the radio on while the mouse sleeps.
static void h_refresh() {
    if (server.hasArg("auto")) {
        clock_sync();
    } else {
        touch();
    }
    if (server.hasArg("auto") && (cq_n > 0)) {
        return ok_json(true);  // something is already queued: the page asks again on its next round
    }
    ok_json(queue_cmd(C_REFRESH, nullptr, 0));
}

// /api/flash?c=rrggbb
static void h_flash() {
    touch();
    String s = server.arg("c");
    if (s.length() != 6) {
        return ok_json(false, "need rrggbb");
    }
    uint32_t c = strtoul(s.c_str(), nullptr, 16);
    uint8_t d[3] = { (uint8_t) (c >> 16), (uint8_t) (c >> 8), (uint8_t) c };
    ok_json(queue_cmd(C_FLASH, d, 3));
}

// POST /api/wifisleep?on=0|1   Wi-Fi power save (kept; applies at once)
static void h_wifisleep() {
    touch();
    long v;
    if (!arg_int("on", 0, 1, &v)) {
        return ok_json(false, "bad value");
    }
    wsleep = (v == 1);
    prefs.putBool("wsleep", wsleep);
    if (radio != Radio::OFF) {
        WiFi.setSleep(wsleep);
    }
    ok_json(true);
}

// POST /api/ui?f=0-255   which fold-outs are open on the page, kept here so a reload (on any device, at any
// address the QT Py gets) opens the same ones
static void h_ui() {
    touch();
    long v;
    if (!arg_int("f", 0, 255, &v)) {
        return ok_json(false, "bad value");
    }
    if (v != ui_folds) {
        ui_folds = (int16_t) v;
        prefs.putUChar("ui", (uint8_t) v);
    }
    ok_json(true);
}

// POST /api/wifimode?m=0|1|2   Wi-Fi switches itself off after 10 min idle / 1 hour idle / never
static void h_wifimode() {
    touch();
    long m;
    if (!arg_int("m", 0, 2, &m)) {
        return ok_json(false, "bad value");
    }
    wmode = (uint8_t) m;
    prefs.putUChar("wmode", wmode);
    ok_json(true);
}

static void h_radio_off() {
    ok_json(true);
    delay(100);
    radio_off();
}

// POST /api/wifi  ssid=&pass=   (saved; the QT Py then joins it)
static void h_wifi() {
    touch();
    String ssid = server.arg("ssid");
    if (!ssid.length() || (ssid.length() > 32)) {
        return ok_json(false, "need a network name");
    }
    prefs.putString("ssid", ssid);
    prefs.putString("pass", server.arg("pass"));
    ok_json(true);
    delay(300);
    radio_off();
    radio_on();
}

// POST /api/update  (multipart, one file field holding the .bin): new QT Py firmware over Wi-Fi.
// Home network only, never on the open setup network. Comes back onto Wi-Fi after the restart.
static bool upd_started = false;
static bool upd_ok = false;

static void h_update_upload() {
    HTTPUpload& up = server.upload();
    if (up.status == UPLOAD_FILE_START) {
        upd_ok = false;
        upd_started = (radio == Radio::STA) && Update.begin(UPDATE_SIZE_UNKNOWN);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        touch();
        if (upd_started && (Update.write(up.buf, up.currentSize) != up.currentSize)) {
            Update.abort();
            upd_started = false;
        }
    } else if (up.status == UPLOAD_FILE_END) {
        upd_ok = upd_started && Update.end(true);
        upd_started = false;
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (upd_started) {
            Update.abort();
        }
        upd_started = false;
    }
}

static void h_update_done() {
    touch();
    if (!upd_ok) {
        return ok_json(false, (radio == Radio::STA) ? "update failed" : "only on the home network");
    }
    upd_ok = false;
    prefs.putBool("resume", true);
    ok_json(true);
    delay(400);
    ESP.restart();
}

static void h_page() {
    touch();
    queue_cmd(C_REFRESH, nullptr, 0);  // opening the page reads the mouse's current settings
    server.send_P(200, "text/html", PAGE_HTML);
}

static void h_not_found() {
    if (radio == Radio::AP) {  // captive portal: everything leads to the page
        server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
        return;
    }
    server.send(404, "text/plain", "not found");
}

void setup() {
#ifdef NEOPIXEL_POWER
    pinMode(NEOPIXEL_POWER, OUTPUT);
    digitalWrite(NEOPIXEL_POWER, HIGH);
#endif
    pinMode(BOOT_BUTTON, INPUT_PULLUP);
    led(0, 0, 0);
    WiFi.mode(WIFI_OFF);
    prefs.begin("gw", false);
    wmode = prefs.getUChar("wmode", 0);
    wsleep = prefs.getBool("wsleep", false);
    ui_folds = prefs.isKey("ui") ? prefs.getUChar("ui", 0) : -1;
    if (prefs.isKey("rotsign")) {
        prefs.remove("rotsign");  // fw19's learnt swipe direction; the page knows it now
    }
    b_reason = (uint8_t) esp_reset_reason();
    b_boots = prefs.getULong("boots", 0) + 1;
    prefs.putULong("boots", b_boots);
    edit_n = prefs.getBytes("edits", edit_log, sizeof(edit_log)) / sizeof(EditLog);  // fw22: the page edits so far
    hcap_n = prefs.getBytes("hcaps", hcap, sizeof(hcap)) / sizeof(HostCap);         // fw23: USB host captures so far
    hprof_n = prefs.getBytes("hprof", hprof, sizeof(hprof)) / sizeof(HostProf);     // fw24: a stage per host
    calib_n = prefs.getBytes("calib", calib, sizeof(calib)) / sizeof(CalibRec);   // fw27: calibration summaries
    tdpi_stage = prefs.getUChar("tdpis", 0xFF);                                        // fw29: which stage the test lowered
    if (prefs.getBytes("tdpi", tdpi_orig, sizeof(tdpi_orig)) == sizeof(tdpi_orig)) { // fw28: a test's DPI still owed back
        tdpi_active = true;
        tdpi_restore = true;
    }
    if (prefs.getBytes("dgain", &dg, sizeof(dg)) != sizeof(dg)) {                     // fw26: direction gains
        for (int k = 0; k < 8; k++) {
            dg.g[k] = 1000;
        }
        dg.n = 0;
    }
    b_brownouts = prefs.getULong("brownouts", 0);
    b_crashes = prefs.getULong("crashes", 0);
    if (b_reason == ESP_RST_BROWNOUT) {
        prefs.putULong("brownouts", ++b_brownouts);
    } else if ((b_reason == ESP_RST_PANIC) || (b_reason == ESP_RST_INT_WDT) || (b_reason == ESP_RST_TASK_WDT) || (b_reason == ESP_RST_WDT)) {
        prefs.putULong("crashes", ++b_crashes);
    }
    WiFi.onEvent(on_wifi_disconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent(on_wifi_got_ip, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    if (wmode > 2) {
        wmode = 0;
    }
    Wire1.setBufferSize(512);  // the 192-byte block with the chord table (default 128)
    Wire1.begin(LINK_SDA, LINK_SCL, LINK_HZ);
    Wire1.setTimeOut(50);
    server.on("/", HTTP_GET, h_page);
    server.on("/api/status", HTTP_GET, h_status);
    server.on("/api/hosts", HTTP_GET, h_hosts);
    server.on("/api/hostprof", HTTP_POST, h_hostprof);
    server.on("/api/dirgain", HTTP_POST, h_dirgain);
    server.on("/api/tempdpi", HTTP_POST, h_tempdpi);
    server.on("/api/calibdiag", HTTP_POST, h_calibdiag_post);
    server.on("/api/calibdiag", HTTP_GET, h_calibdiag_get);
    server.on("/api/calib", HTTP_POST, h_calib_post);
    server.on("/api/calib", HTTP_GET, h_calib_get);
    server.on("/api/set", HTTP_POST, h_set);
    server.on("/api/palette", HTTP_POST, h_palette);
    server.on("/api/dpi", HTTP_POST, h_dpi);
    server.on("/api/tune", HTTP_POST, h_tune);
    server.on("/api/refresh", HTTP_POST, h_refresh);
    server.on("/api/flash", HTTP_POST, h_flash);
    server.on("/api/radio_off", HTTP_POST, h_radio_off);
    server.on("/api/wifi", HTTP_POST, h_wifi);
    server.on("/api/wifimode", HTTP_POST, h_wifimode);
    server.on("/api/wifisleep", HTTP_POST, h_wifisleep);
    server.on("/api/ui", HTTP_POST, h_ui);
    server.on("/api/opt", HTTP_POST, h_opt);
    server.on("/api/probe", HTTP_POST, h_probe);
    server.on("/api/chords", HTTP_POST, h_chords);
    server.on("/api/update", HTTP_POST, h_update_done, h_update_upload);
    server.onNotFound(h_not_found);
    static const char* hdrs[] = { "User-Agent" };  // fw21: SetLog.dev
    server.collectHeaders(hdrs, 1);
    bool resume = prefs.getBool("resume", false);  // restarted by an update: back onto Wi-Fi
    if (resume) {
        prefs.putBool("resume", false);
    }
    if (resume || (wmode == 2)) {
        radio_on(false);
    }
}

void loop() {
    link_tick();
    radio_tick();
    hcap_tick();
    auto_tick();
    dg_tick();
    tdpi_tick();
    if (ntp_want) {
        ntp_want = false;
        configTime(0, 0, "pool.ntp.org", "time.google.com");  // fw22: UTC; the page shows local time
    }
    if (radio != Radio::OFF) {
        server.handleClient();
    }
    static bool btn_prev = true;
    static uint32_t btn_at = 0;
    bool btn = digitalRead(BOOT_BUTTON);
    if (!btn && btn_prev && (millis() - btn_at > 300)) {
        btn_at = millis();
        radio_toggle();
    }
    btn_prev = btn;
    show_state();
    delay(2);
}

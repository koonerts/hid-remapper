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
#include <nvs.h>  // fw62: nvs_get_stats (diag)
#include <FFat.h>
#include "ev_diff.h"  // fw111: which of the Feather's recent actions are new

// fw35: the page gzipped (page_gz.ps1 makes it from page.h); 11-07: its bytes live in page_data.cpp, compiled on their own
extern const uint8_t* const gw_page_gz;
extern const size_t gw_page_gz_len;
extern const uint32_t gw_page_src_fnv;
#define PAGE_GZ gw_page_gz
#define PAGE_GZ_LEN gw_page_gz_len
#define PAGE_SRC_FNV gw_page_src_fnv

#define LINK_ADDR 0x42
#define LINK_SDA 41  // STEMMA QT (Wire1)
#define LINK_SCL 40
#define LINK_HZ 50000  // only the pins' built-in pull-ups hold the lines up, so keep it slow
#define STATUS_LEN 128  // Feather v14+ fills 64-127 (own crc); older ones send 0xFF there
#define STATUS_FULL 512 // Feather v15+: 128-191 = the chord table (own crc), read only when byte 124 changes; v21+:
                        // 192-255 = recent actions (own crc), read when byte 117's high nibble changes (v15-v20: 192);
                        // v23+: 256-511 = the USB host capture (own crc), read on the same nibble
#define QT_FW 113    // this firmware's number, shown in /api/status; fw113: the status's drst also has the Feather's uptime (fup, s; -1 = not read yet), so the page can say when it restarted; fw112: the frame-timing numbers can be reset (/api/diagreset: the minute / hour / 24 h windows start over, and Feather v74+ starts its since-start counts over; status "drst"); fw111: a chord entry's press count from the presses the Feather listed (the status showed 3 quick presses as 2 changes); fw110: the flick threshold logged once per run of changes (from and to), not per click; fw109: the slow-down gain's flick threshold (all curves, NVS "dcmin", default 20 cm/s) sent to Feather v73+ (/api/dcmin); fw108: a DPI stage, angle or position changed by a mouse chord goes into the edit log (quick presses merged into one entry, at most 6 kept); fw107: a curve Replace answers before the history files are written, and writes the library once (it took 4.1 s against the page's 4 s wait: "no answer" for a save that went through); fw106: review fixes: a USB-A restart from the button logged once it happened, the USB-A flags dropped when old, Restore can clear cm/360 and may match another saved curve; fw105: the Feather's USB-A port (Feather v70): restarts logged, /api/usbreset, block 8 layout 3; fw104: a star and a note on a curve save (/api/cvnote, /api/cvnotes); fw103: the edit log and curve history keep 80 more of each in flash (the page's Show older); fw102: automatic DPI stage changes hold at most 4 places in the 20-entry edit log; fw101: the Downhill slow-down preset (Ramp mirrored left to right); fw100: the Boost ramp slow-down preset (Ramp's opposite), the page's bytes in page_data.cpp; fw99: review fixes for fw98 (a table longer than the Feather takes is sent as none, cv.dcmax; 7-10 points refused until the Feather answers; the page never shows fewer boxes than points; slow-down presets); fw98: slow-down gains up to 10 points (Feather v68; 6 before), library file v3, NVS and /cvhist.bin's 6-point records converted; fw97: the USB frame interrupt's jitter (Feather v67, block 8 30-31 / 40-41) in /api/perfwin gaps jit_max / jit_n50; fw96: block 8 layout 2 (Feather v66: frames checked since the start), restarts by its own uptime; fw95: the mouse's report pauses mid-movement (Feather v63 block 8) per second / minute / hour / 24 h in /api/perfwin "gaps"; fw94: the top bar's Stage chip in the stage's colour; fw93: frames skipped while the Feather settles (Feather v62: its start, USB changes) kept apart (settle_missed); fw92: review fixes for fw91 (sup:false only from an answering older Feather, down flag, skips after a gap kept out of the second/minute, restart in a long gap, link interrupt over the seconds that have it, nodpi from Feather v60); fw91: frame timing windows from Feather v59's still / moving blocks 6-7 (k=a|i|m, w=s too), time slots (no old data after a gap), uncapped skipped frames; fw90: frame timing over the last minute, hour and 24 h (/api/perfwin, RAM only; the profile block read twice a second); fw89: the mouse's sleep time and competitive mode (Feather v58: options 5-6, values in the 192-255 block) in /api/status "opt" (sleep s, comp) and /api/opt k=sleep|comp; fw88: battery kept from the refresh's status query and the QT Py's own (a cmd-04 probe once a minute while a page or tool polls), with the charging byte ("chg"); fw87: the live edit is owned by its page (tok) and leased (90 s), BallSheet's gains/DPI tests end it too, /cvhist.bin saved through /cvhist.tmp, prof drops stale numbers (+ age_ms); fw86: curve Replace logs which curve and what changed (/api/curvehist keeps before and after); fw85: the profile's layout 3 (Feather v55: the loop pass that takes the tick); fw84: the profile's gw_tick parts as Feather v52 times them; fw83: the profile block's layout 2 (Feather v51); fw82: where the Feather's frames go (Feather v50 block 5, /api/status prof); fw81: the curve editor's curve runs live while it's open (/api/cvtry); fw80: the curve editor types a point's values, Linear (all 1.00); fw79: BEGIN sent again for 7.5 s, not 2.5; fw78: an update waits 6 s for a Feather that doesn't answer BEGIN before a slower clock; fw77: what a Feather freeze left (Feather v48, block 4) in /api/status "fz"; fw75: audit 11-01 fixes (library kept off NVS when its file doesn't load, /api/pagereset, update APPLY polled, Wi-Fi drops shown); fw76: slow-down shares in 0.1 % steps
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
#define C_SET_CURVE 0x10    // Feather v30+: [on, tau ms, dpi fallback uint16, n x (speed 0.1 cm/s uint16, gain per mille uint16)] the speed curve
#define C_CURVE_HIST_RESET 0x11  // Feather v32+: [] its speed histogram starts over
#define C_SET_DECEL 0x12    // Feather v39+: [on, n x (share of the peak speed %, gain per mille uint16)] the slow-down gain;
                            // v47+ also [on | 2, n x (share 0.1 % uint16, gain per mille uint16)] (fw76: dc_payload)
#define C_USB_RESET 0x18   // Feather v70+: [] its USB-A port off and on
#define C_DIAG_RESET 0x19  // Feather v74+: [] its since-start diagnostics (frames skipped, report pauses, USB-A restarts and slowest send) start over
#define C_TURN 0x0F       // Feather v27+: [total int32, counts a second uint32, hand counts uint32] a measured turn; [] stops it

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
// fw88: the battery, from whichever status query (command 04) answered last: the refresh's (info, st 67-) or the QT Py's
// own once a minute while something polls (a probe, st 94-). Reply: 08 04 00 00 00 02 | pct | chg | mV (big-endian).
// chg: 1 while the cable charges it, 0 off the cable or full (11-05: 1 at 65-95 % on the cable, 0 from 100 %; the user confirmed it was plugged in, then unplugged).
static uint8_t bat_pct = 0xFF, bat_chg = 0;  // 0xFF = not read yet
static uint16_t bat_mv = 0;
static uint32_t bat_at = 0;        // millis() when read
static bool bat_age_known = false;
static uint8_t bat_probe_seen = 0;  // the probe count when last looked at
static uint32_t bat_asked_at = 0;
static uint32_t bat_polled_at = 0;  // the last /api/status (not last_http: status polls don't hold Wi-Fi's auto-off)
#define BAT_EVERY_MS 60000
static void bat_note(const uint8_t* b, bool first);
static uint32_t st_at = 0;       // last good status
static bool link_up = false;
static bool toggles_known = false;
static uint8_t last_toggles = 0;
static bool feather_init_done = false;  // tuning/palette pushed since the Feather (re)started
// link diagnostics (/api/status "diag")
static uint32_t d_polls = 0, d_ok = 0, d_short = 0, d_bad = 0, d_i2c_resets = 0;
static uint32_t d_rc_fail = 0, d_rc_ms = 0;  // fw33: full-block reads that failed, and the longest one (ms)
static uint32_t d_rc256 = 0, d_rc512 = 0;   // fw35: block reads by size
static uint32_t d_nvs_fail = 0;              // fw62: putBytes that didn't write the whole blob (NVS full?)
static String d_nvs_fail_key;                // fw65: the key of the last save that failed
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
// fw103: what falls off the end of the edit log and of the curve history goes to a file in flash, newest first: EDIT_OLD more of
// each, read only when the page asks (/api/editsold, /api/curvehist?old=1). Automatic stage changes aren't kept there
#define EDIT_OLD 80
#define EDIT_OLD_BIN "/edits_old.bin"
#define CVH_OLD_BIN "/cvhist_old.bin"
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
#define EK_REFUSED 14     // fw32: a change refused from an out-of-date page (no "t": before fw22); text = the endpoint
#define EK_CURVE 15       // fw46: the speed curve or its hosts saved ("N hosts"), or removed ("off")
#define EK_CHORD 17       // fw108: a mouse chord changed a setting ("g:from:to:presses", g = s stage 0-4, a angle, p position; dev 7)
#define EK_USB_RESET 16  // fw105: the Feather's USB-A port restarted ("auto", dev 7, or "button")
#define EK_TEMP_DPI 13   // fw28: the calibration's temporary DPI ("N") or the table put back ("restored", dev 7)
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
// fw105 (Feather v70): the USB-A port, from block 8 layout 3 (usb_note)
static bool usb_ok = false;
static int16_t usb_seen = -1;
static uint8_t usb_flags = 0, usb_mounts = 0, usb_btn = 0;
static uint16_t usb_worst = 0;
static uint32_t usb_btn_at = 0, usb_btn_ip = 0, usb_at = 0;
static uint8_t usb_btn_dev = 0;
// fw112: the last reset of the frame-timing numbers (/api/diagreset): t = when (Unix s; 0 = the clock wasn't set), up = the
// Feather's uptime then, on = its since-start counts still start there (Feather v74+, no restart of it since); NVS "drst",
// so a QT Py restart still knows. drst_ms: millis() at a reset made since this QT Py started (0 = none)
struct __attribute__((packed)) DiagRst {
    uint32_t t, up;
    uint8_t on;
};
static DiagRst drst = { 0, 0, 0 };
static uint32_t drst_ms = 0;
static int32_t feather_up_s();  // fw113: the Feather's uptime now (s), -1 = no second read yet
static uint8_t edit_old_n = 0, cvh_old_n = 0;  // fw103: the records in the two files
// fw38: setting changes as short texts for a PC to show as an on-screen pill (GET /api/osd?since=N): the DPI stage,
// sensor angle and position whenever the Feather reports a new value (chord, page, mouse button), and the automatic
// stage on a host switch. A value only becoming known (power-on, a Feather restart, the mouse waking) isn't a change.
#define OSD_KEEP 8
struct OsdItem {
    uint16_t seq;
    uint32_t ms;
    char text[48];
};
static OsdItem osdq[OSD_KEEP];  // osdq[0] newest
// fw41: the page can be replaced without a firmware update. POST /api/page (multipart: the gzipped page, as page_gz.ps1
// --push sends it) stores it in the flash FAT partition ("ffat", 3.7 MB, unused until now) and / serves it from there;
// the built-in copy is the fallback. The partition is formatted on the first upload (never at boot), and a page uploaded
// under another firmware number is dropped at boot: a firmware update brings its own matching page (audit 11-01: also when
// the built-in page changed under the same number, PAGE_SRC_FNV; POST /api/pagereset goes back to the built-in one).
static bool fs_ok = false;
static bool page_fs = false;
static uint32_t page_fs_t = 0, page_fs_len = 0;
static File page_up;
static uint32_t page_up_len = 0;
static bool page_up_ok = false;  // fw47: this request's upload reached its end with every byte written
static uint8_t osd_n = 0;
static uint16_t osd_seq = 0;
static int16_t osd_stage = -1, osd_angle = 0x7FFF, osd_pos = 0x7FFF;  // last known values (sentinels: not yet)
// fw33: the queued command each edit went with (RAM, this boot): one the Feather never acknowledged marks its entry "not
// applied" (dev bit 7) instead of leaving it in the history as made
static uint16_t edit_seq[EDIT_LOG];
static uint16_t q_fresh = 0;  // 0x100 | seq of the command queued by the current request, taken by the next log_edit
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
// fw34: the first 6 DISTINCT requests (fw24-33: the first 12 as sent). Windows sometimes reads the strings again before
// configuring (seen 10-21 with the Warg wired: FLOW matched its usual 9 requests, then strings 0/3/1/2 again instead of
// the configuration), which gave FLOW a second signature with no stage. All 8 saved captures checked: FLOW's two
// variants give one signature, XIM's another. Saved stages and gain hosts are moved over once (NVS "sigv" = 2).
#define HSIG_REQS 6
#define HSIG_V1_REQS 12
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
// fw57: the auto stage's timeline this power-on (ms since the QT Py started, which is about when the ATEN switched),
// for /api/hosts: 1 capture read, 2 host known, 3 Feather answering, 4 Warg there, 5 stage read sent, 6 answer (stage),
// 7 read unanswered, 8 stage written (stage), 9 the Feather took it, 10 confirmed
struct AutoEv { uint32_t ms; uint8_t code; int8_t v; };
static AutoEv auto_tl[20];
static uint8_t auto_tl_n = 0;
static void atl(uint8_t code, int v = 0) {
    if (auto_tl_n && (auto_tl[auto_tl_n - 1].code == code) && (auto_tl[auto_tl_n - 1].v == (int8_t) v)) {
        return;  // fw62: the same answer read again (a failed queue retries) fills no slots
    }
    if (auto_tl_n < 20) {
        auto_tl[auto_tl_n++] = { millis(), code, (int8_t) v };
    }
}
// fw34: what the automatic stage did, kept across a QT Py-only restart (RTC memory survives a software reset, not a power
// cut). A restart keeps a stage that was already set for this host (it may have been changed by hand since) and still
// sets one that wasn't (fw33 kept whatever was there: after the signature fix, FLOW would have stayed on XIM's stage).
struct AutoRtc {
    uint32_t magic, boots, sig;
    uint8_t state;
};
#define AUTO_RTC_MAGIC 0x47574153u
static RTC_NOINIT_ATTR AutoRtc auto_rtc;
static AutoRtc auto_prev;  // the previous boot's, taken in setup()
static uint8_t auto_state = 0;  // 0 waiting, 1 sent, 2 confirmed, 3 gave up, 4 no stage for this host; fw33: 5 checking
                                // with the mouse, 6 kept (only the QT Py restarted)
static uint8_t auto_cap = 0;      // fw33: the capture (its number) auto_sig came from; a newer one this power-on is checked too
static uint8_t auto_from = 0xFF;  // fw33: the mouse's stage before the first send (0xFF = unknown): logged only on a change
static uint8_t auto_probe_n = 0;  // fw33: the probe count when the stage register read went out
static uint16_t auto_lost = 0;    // fw33: rounds the mouse didn't answer (asleep); fw35: only read again, never given up
static uint32_t auto_since = 0;   // fw35: when this power-on's host was taken (DPI chords after it = the user's choice)
static bool feather_ran = false;  // fw33: the Feather was already set up at this boot's first status (only the QT Py restarted)
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
// fw39: where the gains in use came from, as the sender says ("pool:<run times>", "run:<time>", "flat", "hand", ...;
// ";hand" appended when edited after loading a suggestion), and when they were saved. NVS "dgsrc" / "dgsrct".
static String dg_src;
static uint32_t dg_src_t = 0;
// fw42: temporary gains for BallSheet's blind A/B accuracy test: applied on the host connected now instead of the saved
// table, never saved or logged, back to the saved table on restore=1 or 12 s after the last keep-alive
static bool dgt_on = false;
static uint16_t dgt_g[8];
static uint32_t dgt_keep_at = 0;
static uint32_t dg_sent_at = 0;
static bool dg_reread = false;
static uint16_t dg_sends = 0;  // since boot, for /api/status
static bool dg_force = false;  // fw47: send once even if the Feather's crc matches (an 8-bit crc can match a changed table)
// fw46: the speed curve (Feather v30, gw_curve.cc): the XIM's ballistic curve done on the Feather, so the XIM can stay
// linear. Points of hand speed (0.1 cm/s, rising) and gain (per mille), the speed's smoothing, the DPI when saved (the
// Feather's fallback until it knows the mouse's), and the hosts it applies on. NVS "curve"; where it came from (the
// page's import: the XIM's string, its 100% speed, shape and fit) in "cvsrc". Kept right on the Feather like the gains.
#define CV_N 10
struct __attribute__((packed)) CurveCfg {
    uint8_t n;  // 0 = none
    uint8_t tau;
    uint16_t dpi;
    uint16_t sp[CV_N];
    uint16_t g[CV_N];
    uint8_t hn;
    uint32_t sig[HPROF_MAX];
};
static CurveCfg cv = { 0, 8, 0, { 0 }, { 0 }, 0, { 0 } };
static String cv_src;
static uint32_t cv_sent_at = 0;
static bool cv_reread = false;
static uint16_t cv_sends = 0;
static bool cv_force = false;  // fw47: as dg_force
// fw48: BallSheet can run with the curve off (its reach-by-speed check compares runs with and without): off on the host
// connected now until restore=1 or 12 s after the last keep-alive, never saved
static bool cvt_off = false;
static uint32_t cvt_keep_at = 0;
static uint32_t cvt_ab_at = 0;  // fw59: BallSheet runs a blind curve A/B (ab=1 on any cvtemp call, both arms): the page hides the arm
// fw81: the page's curve editor runs its table (and slow-down gain) live while it's open: sent to the Feather in place of
// the running curve, on the same hosts, until end=1 (the editor closed, the page went away or another one loaded), a
// BallSheet run or a restart; never saved. fw81 had no timeout (a page's keep-alive only finds out whether to send it again)
// fw87: owned by the page that sent it (tok, one per page load: a newer try takes over, keep/end from another page change
// nothing) and leased: 90 s without its page's try/keep (a page gone without pagehide) ends it (cv_tick)
static bool cvt_try = false;
static char cvt_tok[17] = "";
static uint32_t cvt_seen = 0;
#define CVT_LEASE_MS 90000
static uint8_t try_n = 0, try_tau = 8;
static uint16_t try_sp[CV_N], try_g[CV_N];
// fw49: named curves: each saved curve table gets an id (1-16) that cm/360 results carry and BallSheet stamps on its runs,
// so results under different curves stay apart; one may be the candidate a curve-vs-curve A/B compares the active one
// with (BallSheet switches to it through /api/cvtemp?cand=1). NVS "cvid", "cvcand"; fw65: each curve under "cvl<id>"
// and its gains under "cvg<id>" (fw49-64 kept all 16 slots in one blob each, "cvlib" 1488 bytes and "cvdg" 2240: NVS
// writes a whole new copy before erasing the old one, so any change needed 49 and 72 free entries, and once the 20 KB
// partition filled up the saves failed unseen and new curves were gone at the next power-off (10-29))
#define CVLIB_N 16
#define NVS_KEEP 72  // fw65: free entries an added curve must leave, so the largest blob left ("hcaps", 8 x 266 bytes) can still be rewritten
static bool nvs_put(const char* key, const void* p, size_t len) {  // fw65: a blob save, failures counted with their key
    if (prefs.putBytes(key, p, len) == len) {
        return true;
    }
    d_nvs_fail++;
    d_nvs_fail_key = key;
    return false;
}
static int nvs_room() {  // fw65: entries a save can still take (NVS keeps one page free for its garbage collection)
    nvs_stats_t ns;
    return (nvs_get_stats(NULL, &ns) == ESP_OK) ? (int) ns.free_entries - 126 : 0;
}
struct __attribute__((packed)) CurveLib {  // 93 bytes (fw49 first had 16-byte names: 61, converted at boot)
    uint8_t used;
    char name[48];
    uint8_t n, tau;
    uint16_t s10;  // the XIM-linear cm/360 it was fitted with, x10 (0 = not known)
    uint16_t sp[CV_N];
    uint16_t g[CV_N];
};
static CurveLib cvlib[CVLIB_N];
static_assert(sizeof(CurveLib) == 93, "the boot conversion assumes this layout");
static uint8_t cv_id = 0, cv_cand = 0;  // the active curve's id, the candidate's; 0 = none
// fw60: each library curve's own direction gains (the table saved while it ran), with where they came from. Use swaps
// them in with the curve; BallSheet's candidate arm runs the candidate's. A curve without its own keeps the current ones.
struct __attribute__((packed)) CurveGains {
    uint8_t has;
    uint16_t g[8];
    uint32_t src_t;
    char src[121];
};
static CurveGains cvdg[CVLIB_N];
static CurveGains cvdg0;  // fw62: the gains in use while no library curve runs (cv_id 0), so a Use doesn't replace them for good; NVS "cvdg0"
// a macro: the Arduino build puts its function prototypes above this struct
#define cvdg_slot(id) ((id) == 0 ? &cvdg0 : ((id) <= CVLIB_N ? &cvdg[(id) - 1] : (CurveGains*) nullptr))
// fw61: per library curve: when it was added, when last used or changed (Use, rename, linear, its gains), its place in
// the page's custom order. 0 = not known (curves from before fw61, or no clock yet). NVS "cvmeta".
struct __attribute__((packed)) CurveMeta {
    uint32_t added, used;
    uint8_t ord;
};
static CurveMeta cvmeta[CVLIB_N];
static CurveLib cvlib_nv[CVLIB_N];   // fw65: what NVS holds per curve, so a save writes only the curves that changed
static CurveGains cvdg_nv[CVLIB_N];
static bool cvlib_blob = false, cvdg_blob = false;  // fw65: the old 16-slot blob is still there (removed once every curve is under its own key)
// fw65: write the changed slots under "<pre><id>" (an unused slot's key removed), then drop the old blob; false = a save failed
static bool slots_save(const char* pre, void* ram, void* nv, size_t sz, bool (*used)(const void*), const char* blob, bool* blob_left) {
    bool ok = true;
    for (int k = 0; k < CVLIB_N; k++) {
        uint8_t* r = (uint8_t*) ram + sz * k;
        uint8_t* s = (uint8_t*) nv + sz * k;
        if (!memcmp(r, s, sz)) {
            continue;
        }
        char key[8];
        snprintf(key, sizeof(key), "%s%d", pre, k + 1);
        if (used(r) ? !nvs_put(key, r, sz) : (prefs.isKey(key) && !prefs.remove(key))) {
            ok = false;
            continue;
        }
        memcpy(s, r, sz);
    }
    if (ok && *blob_left) {
        *blob_left = !prefs.remove(blob);
    }
    return ok;
}
static bool cvlib_used(const void* p) { return ((const CurveLib*) p)->used; }
static bool cvdg_used(const void* p) { return ((const CurveGains*) p)->has; }
static bool lib_fs = false;  // fw71: the library is in /curves.bin (more about it with lib_write)
static uint32_t lib_writes = 0, lib_fails = 0, lib_bad = 0;  // lib_bad: library files that didn't read back, set aside at boot
// audit 11-01: the library lives in its file (NVS "libfs") but no copy read back at this boot (FFat didn't mount, or every
// file failed its check). NVS no longer holds it, so nothing falls back there or is written anywhere: the running curve
// (NVS "curve") still applies, its id and the candidate's stay as saved, and library edits are refused (LIB_OFF_WHY)
static bool lib_off = false;
static bool lib_dmg = false;  // audit 11-01: of those, the storage mounted but every copy failed its check (a restart won't help)
#define LIB_OFF_WHY (lib_dmg ? "the curve library's files are damaged: 'Start a new library' on the page sets them aside and starts one with the running curve" : "the curve library didn't load at start-up (its file storage): restart the QT Py; nothing in it was changed")
static bool lib_fs_on();
static bool lib_write();
static bool cvdg_save() {
    if (lib_off) {
        return false;  // audit 11-01
    }
    return lib_fs_on() ? lib_write() : slots_save("cvg", cvdg, cvdg_nv, sizeof(CurveGains), cvdg_used, "cvdg", &cvdg_blob);  // fw71: the file
}
// fw67: each library curve's slow-down gain (Feather v39, C_SET_DECEL): once a stroke slows from its peak speed, a gain
// by the speed's share of that peak (points in %, rising, below 100; 100% = 1.0 implied); speeding up is left alone.
// Part of the curve like its table: a different slow-down gain is a different library curve, so BallSheet's results and
// its curve-vs-curve A/B keep them apart. NVS "cvd<id>".
// fw76: shares in 0.1 % (10-990, so 2.5 %); fw67-75 kept whole % (uint8, DC_V1_SIZE bytes a curve: LIB_VER 1's file and
// NVS "cvd<id>"), converted at boot by cvdc_from_v1 (share x 10)
// fw98: 10 points (Feather v68); fw67-97 had 6 (DC_OLD_N): LIB_VER 1-2 files, NVS "cvd<id>" and /cvhist.bin records of
// that size are converted when read (cvdc_from_v1 / cvdc_from_old)
#define DC_N 10
#define DC_OLD_N 6
#define DC_FEATHER_N(v) (((v) >= 68) ? DC_N : DC_OLD_N)  // points the Feather (status byte 56) takes
struct __attribute__((packed)) CurveDecel {
    uint8_t n;  // 0 = none
    uint16_t p10[DC_N];  // fw76: share of the peak speed, 0.1 %
    uint16_t g[DC_N];
};
static_assert(sizeof(CurveDecel) == 1 + 4 * DC_N, "LIB_VER 3's layout");
#define DC_V1_SIZE (1 + 3 * DC_OLD_N)  // fw76: fw67-75's CurveDecel { uint8 n; uint8 pct[6]; uint16 g[6] }, 19 bytes
#define DC_V2_SIZE (1 + 4 * DC_OLD_N)  // fw98: fw76-97's { uint8 n; uint16 p10[6]; uint16 g[6] }, 25 bytes
static_assert(DC_V1_SIZE == 19 && DC_V2_SIZE == 25, "the old layouts");
static CurveDecel cvdc[CVLIB_N];
static CurveDecel cvdc_nv[CVLIB_N];
static bool cvdc_blob = false;  // never an old blob (slots_save asks)
static uint16_t dc_sends = 0;
static uint16_t dc_min10 = 200;  // fw109: a stroke's peak must reach this (0.1 cm/s) for its slow-down gain (Feather v73+); 0 = any stroke
static bool dc_force = false;  // send it after every curve send (a Use or BallSheet's switch changes both)
static CurveDecel try_dc;  // fw81: the editor's slow-down gain while it's live (cvt_try)
static bool cvt_stop() {  // fw87: end the editor's live edit (the running curve and its slow-down gain back); true if it was live
    if (!cvt_try) {
        return false;
    }
    cvt_try = false;
    cvt_tok[0] = 0;
    cv_force = true;
    dc_force = true;
    cv_sent_at = 0;
    return true;
}
static bool cvdc_used(const void* p) { return ((const CurveDecel*) p)->n != 0; }
static bool lib_write();
static bool cvdc_save() {
    if (lib_off) {
        return false;  // audit 11-01
    }
    return lib_fs_on() ? lib_write() : slots_save("cvd", cvdc, cvdc_nv, sizeof(CurveDecel), cvdc_used, "cvdcx", &cvdc_blob);  // fw71: the file
}
// fw76: slot k's slow-down gain from fw67-75's DC_V1_SIZE bytes (raw bytes, not a struct: the Arduino build puts function
// prototypes above the structs); shares x 10, everything else as it was
static void cvdc_from_v1(int k, const uint8_t* o) {
    CurveDecel& d = cvdc[k];
    memset(&d, 0, sizeof(d));
    d.n = (o[0] > DC_OLD_N) ? DC_OLD_N : o[0];
    for (int i = 0; i < DC_OLD_N; i++) {  // fw98: the old point count (the offsets are by it)
        d.p10[i] = (uint16_t) o[1 + i] * 10;
        d.g[i] = (uint16_t) (o[1 + DC_OLD_N + 2 * i] | (o[2 + DC_OLD_N + 2 * i] << 8));
    }
}
// fw98: slot k's slow-down gain from fw76-97's DC_V2_SIZE bytes (6 points: n, p10[6], g[6]); the 4 new points stay empty
static void cvdc_from_old(void* dp, const uint8_t* o) {
    CurveDecel& d = *(CurveDecel*) dp;
    memset(&d, 0, sizeof(d));
    d.n = (o[0] > DC_OLD_N) ? DC_OLD_N : o[0];
    for (int i = 0; i < DC_OLD_N; i++) {
        d.p10[i] = (uint16_t) (o[1 + 2 * i] | (o[2 + 2 * i] << 8));
        d.g[i] = (uint16_t) (o[1 + 2 * DC_OLD_N + 2 * i] | (o[2 + 2 * DC_OLD_N + 2 * i] << 8));
    }
}
static bool cvt_cand = false;           // BallSheet's A/B block runs the candidate
// fw27: the flick calibrations' summaries (the page sends one after each run), newest first; NVS "calib"
// fw36: + the cursor speed the run used (BallSheet's %, 0 = not known: the page's runs, and runs from before fw36), so
// the page and BallSheet can pool runs made at a similar speed (NVS "calib2"; "calib" is moved over once)
#define CALIB_KEEP 32  // 10-24: was 6 (pooling, now often within one curve setting, wants more); fw64: 32 (the accuracy test's runs come too)
#define CALIB_V1_SIZE 46
#define CALIB_V2_SIZE 47  // fw36-63 (NVS "calib2"): before src
struct __attribute__((packed)) CalibRec {  // 47 bytes
    uint32_t t;         // Unix time
    uint8_t n[8];       // flicks measured per direction
    uint16_t med[8];    // median reach per mille (0 = too few)
    uint16_t g[8];      // the gains this host applied while measuring, per mille
    uint8_t flags;      // bit 0 raw input, bit 1 coarse browser clock, bit 2 the speed curve on; 10-24: bits 3-7 its curve id (0 = not recorded)
    uint8_t skipped;    // flicks skipped (at most 255)
    uint8_t speed;      // fw36: cursor speed %, 0 = not known
    uint8_t src;        // fw64: 0 a calibration run, 1 BallSheet's accuracy test (one arm's first movements, pooled over sessions)
};
static_assert(sizeof(CalibRec) == CALIB_V2_SIZE + 1, "fw64 appended one byte to the fw36 record");
static CalibRec calib[CALIB_KEEP];
// fw43: the cm/360 card's results (measured with the Feather's steady turn, v27), newest first; NVS "c360"
#define C360_KEEP 24  // fw45: was 12; a curve with ~12 speeds plus by-hand counts outgrew it
struct __attribute__((packed)) C360Rec {  // 12 bytes
    uint32_t t;      // Unix time
    uint16_t cm100;  // cm/360 x 100
    uint16_t sp10;   // the turn's speed, cm/s x 10
    uint32_t dpi;    // the DPI it was measured at (low 24 bits); fw48: bits 24-31 = what it was measured as (the page's
                     // context list: 0 ADS 4.5x, the only one before, 1 hip, ...), so hip and ADS results never mix
};
static C360Rec c360[C360_KEEP];
static uint8_t c360_n = 0;
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
static uint8_t tdpi_bad = 0;        // fw33: rounds the mouse answered with another value; 10 = give up (it can't store it)
static uint32_t tdpi_phase_at = 0;
static String calib_diag;           // fw29: the page's last calibration diagnostics (RAM only), GET /api/calibdiag
static uint8_t calib_n = 0;
static uint8_t wmode = 0;         // Wi-Fi switches itself off: 0 = 10 min idle, 1 = 1 hour idle, 2 = never
static bool radio_manual = true;   // this radio session was started by hand (button, chord, page)
static bool radio_rejoin = false;  // audit 11-01: JOINING again after a dropped connection (not the first join: no setup-network fallback)
// Wi-Fi power save. Off by default: with it on the board can go unreachable for a minute or more
// on some routers (broadcast ARP and mDNS queries get lost while it dozes). /api/wifisleep sets it.
static bool wsleep = false;
static int8_t ui_tab = -1;     // fw37: the page's open tab (0 Mouse, 1 Aim, 2 Setup, 3 Diagnostics); -1 = never set
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
    q_fresh = 0x100 | c.seq;
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


static void edit_dropped(uint8_t seq) {
    for (uint8_t k = 0; k < edit_n; k++) {
        if ((edit_seq[k] == (0x100 | seq)) && (edit_log[k].boot == (uint16_t) b_boots) && (millis() - edit_log[k].ms < 30000)) {
            edit_log[k].dev |= 0x80;
            edit_seq[k] = 0;
            nvs_put("edits", edit_log, sizeof(EditLog) * edit_n);
            return;
        }
    }
}

static void pump_cmds() {
    if (cq_n == 0) {
        return;
    }
    Cmd& c = cmdq[cq_head];
    if (st_ok && cmd_sent_at && (st[55] == c.seq)) {  // acked (audit 11-01: only once it went out)
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
            edit_dropped(c.seq);
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
    bool info_first_now = st_ext && info_first;
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
    bat_note(b, info_first_now);
    st_ok = true;
    st_at = millis();
    return true;
}

// recent actions (Feather v21+): its block 192-255 as read, when, and the change counter it went with
static uint8_t evb[64];
static bool ev_ok = false;
static uint32_t ev_read_at = 0;
static uint8_t ev_nib = 0xFF;
// fw111: when each of the last 16 mouse-chord presses per setting (0 stage, 1 angle, 2 position) was first seen in the
// list (millis | 1, 0 = none), and up to when they were counted into the edit log: an entry's press count was the
// number of changes the status showed, and the mouse can take two quick presses as one change (3 presses logged as 2)
static uint32_t pr_ms[3][16];
static uint8_t pr_w[3];
static uint32_t pr_done[3];
static void pr_seen(const uint8_t* b, uint32_t el_ms) {  // b = the new list, compared with evb (the previous one)
    uint8_t k = ev_new_count(evb, b, el_ms);
    for (uint8_t j = 0; j < k; j++) {
        const uint8_t* q = b + 2 + 6 * j;
        uint8_t g = ev_group(q[4]);
        if (ev_is_chord(q) && (g < 3)) {
            pr_ms[g][pr_w[g]++ & 15] = millis() | 1;
        }
    }
}
// the presses of setting g first seen after the last count and no earlier than 1.5 s before t0 (the first change of them)
static uint8_t pr_count(uint8_t g, uint32_t t0) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < 16; i++) {
        uint32_t m = pr_ms[g][i];
        n += m && ((int32_t) (m - pr_done[g]) > 0) && ((int32_t) (m - (t0 - 1500)) >= 0);
    }
    pr_done[g] = millis() | 1;
    return n;
}

static bool events_supported() {
    return st_ok && st_ext && (st[56] >= 21);
}

// the full block, for its chord table (when the table number, byte 124, moved) and, from Feather v21, its recent
// actions (when byte 117's high nibble moved)
static void hcap_take(const uint8_t* u);

// fw35 (Feather v26+): status 244/245 carry the capture block's change counter and number, so after one full read this
// boot the block (256-511) is read only when they moved: 256 bytes after a chord action instead of 512 (~46 vs ~92 ms
// of the loop and of the Feather's I2C interrupts). Reset whenever the Feather may have restarted.
static bool hc_known = false;
static uint8_t hc_upd = 0, hc_id = 0;

static bool read_block(uint8_t* b, size_t len) {  // the status from byte 0; false = short or bad (counted)
    if (len == 512) {
        Wire1.setTimeOut(150);  // 512 bytes take about 92 ms at 50 kHz; polls keep the short timeout
    }
    uint32_t t0 = millis();
    size_t n = Wire1.requestFrom((uint16_t) LINK_ADDR, len);
    if (len == 512) {
        Wire1.setTimeOut(50);
    }
    if (millis() - t0 > d_rc_ms) {
        d_rc_ms = millis() - t0;
    }
    if (n != len) {
        while (Wire1.available()) {
            Wire1.read();
        }
        d_rc_fail++;
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        b[i] = Wire1.read();
    }
    if (len == 512) {
        d_rc512++;
    } else if (len == 256) {
        d_rc256++;
    }
    if ((b[0] != 'G') || (crc8(b, 63) != b[63]) || (b[128] != 'C') || (crc8(b + 128, 63) != b[191]) || (b[130] > CH_MAX)) {
        d_rc_fail++;
        return false;
    }
    return true;
}

static void read_chords() {
    static uint8_t b[STATUS_FULL];
    bool cap = hostcap_supported(), counters = cap && (st[56] >= 26);
    size_t len = cap ? ((counters && hc_known) ? 256 : 512) : events_supported() ? 256 : 192;
    if (!read_block(b, len)) {
        return;
    }
    memcpy(ch, b + 128, 64);
    ch_ok = true;
    if ((len >= 256) && (b[192] == 'E') && (crc8(b + 192, 63) == b[255]) && (b[193] <= 8)) {
        if (ev_ok) {
            pr_seen(b + 192, millis() - ev_read_at);  // fw111
        }
        memcpy(evb, b + 192, 64);
        ev_ok = true;
        ev_read_at = millis();
        ev_nib = b[117] >> 4;
    }
    bool ev_good = (len >= 256) && (b[192] == 'E') && (crc8(b + 192, 63) == b[255]);
    if ((len == 256) && cap && ev_good && ((b[244] != hc_upd) || (b[245] != hc_id))) {
        if (!read_block(b, 512)) {  // the capture moved: read it all
            return;
        }
        len = 512;
    }
    if ((len == 512) && (b[256] == 'U') && (crc8(b + 256, 255) == b[511]) && (b[257] != 0) && (b[259] <= HCAP_ENTRIES)) {
        hcap_take(b + 256);
        if (counters && (b[192] == 'E') && (crc8(b + 192, 63) == b[255])) {
            hc_upd = b[244];
            hc_id = b[245];
            hc_known = true;
        }
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
    if (same) {  // fw33: a later request (an LED report, a tool): save only when the part kept here changed, not its counters
        uint8_t k = (u[3] <= HCAP_ENTRIES) ? u[3] : HCAP_ENTRIES;
        bool kept = (hcap[0].blk[3] != u[3]) || memcmp(hcap[0].blk + 4, u + 4, 3) || ((hcap[0].blk[9] ^ u[9]) & 1) ||
                    memcmp(hcap[0].blk + 16, u + 16, 8 * k);
        memcpy(hcap[0].blk, u, 256);
        if (kept) {
            hcap_dirty = true;
            hcap_changed_at = millis();
            hcap_seq++;
        }
        return;
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
            if (!hcap[0].t) {
                hcap[0].ms = millis();  // fw33: the old boot's millis() would date it wrongly once the clock is known
            }
            hcap[0].boot = (uint16_t) b_boots;
            memcpy(hcap[0].blk, u, 256);
            hcap_dirty = true;
            hcap_changed_at = millis();
            hcap_seq++;
            return;
        }
    }
    if (!same) {
        atl(1, u[3]);
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
        nvs_put("hcaps", hcap, sizeof(HostCap) * hcap_n);
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
    radio_rejoin = false;
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
        radio_rejoin = false;
        start_mdns();  // audit 11-01: on a reconnect too
    }
    if ((radio == Radio::STA) && (WiFi.status() != WL_CONNECTED)) {  // audit 11-01: the connection dropped: show it (the
        radio = Radio::JOINING;                                        // stack reconnects by itself), never the setup network
        radio_rejoin = true;
    }
    // switched on by hand and can't join: setup network instead. Started by itself (power-up,
    // after an update): keep trying, the Wi-Fi stack retries on its own.
    if ((radio == Radio::JOINING) && radio_manual && !radio_rejoin && (millis() - radio_since > JOIN_TIMEOUT_MS)) {
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
    if (cq_n > 0) {
        period = 10;  // fw33: a command waits for its ack (the Feather applies it within a few ms): don't sit a full period
    }
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
        hc_known = false;  // fw35: and its capture counters too
        pf_stale();  // fw87: nor is its profile current
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
    static bool first_status = true;
    if (first_status) {  // fw33: tuning already set = the Feather kept running and only the QT Py restarted (OTA, crash)
        first_status = false;
        feather_ran = st[2] & 1;
        // audit 11-01: the numbers go on from the Feather's last ack (byte 55), so after a QT Py-only restart the first command
        // can't carry the number it took last (it would skip it as a repeat). Nothing has gone out yet: renumber the queue.
        uint8_t s = st[55], olds[CMDQ];
        for (uint8_t k = 0; k < cq_n; k++) {
            Cmd& c = cmdq[(cq_head + k) % CMDQ];
            s = (s == 255) ? 1 : s + 1;
            olds[k] = c.seq;
            c.seq = s;
            c.f[1] = s;
            c.f[c.n - 1] = crc8(c.f, c.n - 1);
        }
        for (uint8_t e = 0; e < EDIT_LOG; e++) {  // the edits that went with them follow (edit_dropped)
            for (uint8_t k = 0; k < cq_n; k++) {
                if (edit_seq[e] == (0x100 | olds[k])) {
                    edit_seq[e] = 0x100 | cmdq[(cq_head + k) % CMDQ].seq;
                    break;
                }
            }
        }
        next_seq = (s == 255) ? 1 : s + 1;
    }
    if (chords_supported() && (!ch_ok || (ch[1] != st[124]) || (events_supported() && ((st[117] >> 4) != ev_nib)))) {
        read_chords();
    }
    fz_check((st[56] >= 48) && (st[2] & 16));  // fw77
    if (st[56] >= 55) {
        pf_check();  // fw82; fw83: layout 2 only (Feather v51)
    } else {
        pf_stale();  // fw87: an older Feather has no profile: the last one isn't current
    }
    // a restarted Feather has lost its tuning flag: re-baseline the counter, give back the settings
    if (feather_init_done && !(st[2] & 1)) {
        feather_init_done = false;
        toggles_known = false;
        ch_ok = false;
        hc_known = false;
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
    q_fresh = 0;
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
    // fw89 (Feather v58): sleep time (s) and competitive mode, at 236-237 of the recent-actions block (evb 44-45)
    bool x = (st[56] >= 58);
    o += ",\"xsup\":" + String(x ? "true" : "false");
    o += ",\"sleep\":" + ((x && ev_ok && (known & 32) && (evb[44] >= 3) && (evb[44] <= 60)) ? String(evb[44] * 10) : String("null"));
    o += ",\"comp\":" + String((x && ev_ok && (known & 64) && (evb[45] <= 1)) ? (evb[45] ? "true" : "false") : "null");
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
// fw88: the byte after the % is 1 while the cable charges it (11-05: 1 at 65-95 % with the voltage rising; 0 at 100 % on 10-09)
static uint8_t bat_info_seen = 0;
static void bat_set(const uint8_t* q, bool known) {  // q: the reply (report ID first)
    bat_pct = q[6];
    bat_chg = q[7];
    bat_mv = (uint16_t) (q[8] << 8 | q[9]);
    bat_at = millis();
    bat_age_known = known;
}
static bool bat_reply_ok(const uint8_t* q, uint8_t len) {
    return (len >= 10) && (q[0] == 8) && (q[1] == 0x04) && (q[6] <= 100);
}
// each good status from the Feather: a new reply to the refresh's status query (info) or to a probe (cmd 04), whichever came
static void bat_note(const uint8_t* b, bool first) {
    if (!st_ext) {
        return;
    }
    if (first) {  // after a restart: the replies already there; an info reply is used, of unknown age
        bat_info_seen = b[65];
        bat_probe_seen = b[91];
        if ((b[66] > 0) && bat_reply_ok(b + 67, b[66])) {
            bat_set(b + 67, false);
        }
        return;
    }
    if (b[65] != bat_info_seen) {
        bat_info_seen = b[65];
        if ((b[66] > 0) && bat_reply_ok(b + 67, b[66])) {
            bat_set(b + 67, true);
        }
    }
    if (b[91] != bat_probe_seen) {
        bat_probe_seen = b[91];
        if (((b[92] & 3) == 2) && (b[93] <= 24) && bat_reply_ok(b + 94, b[93])) {
            bat_set(b + 94, true);
        }
    }
}
static String battery_json() {
    if (!st_ok || !st_ext || (bat_pct == 0xFF)) {
        return "null";
    }
    return "{\"pct\":" + String(bat_pct) + ",\"mv\":" + String(bat_mv) + ",\"chg\":" + String(bat_chg) + ",\"age_s\":" + String(bat_age_known ? (long) ((millis() - bat_at) / 1000) : -1L) + "}";
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
    if (!from_current_page()) {
        return;
    }
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
    } else if ((k == "sleep") || (k == "comp")) {  // fw89: Feather v58
        if (st[56] < 58) {
            return ok_json(false, "needs Feather firmware v58");
        }
        long b;
        if ((k == "sleep") ? (!arg_int("v", 30, 600, &b) || (b % 10)) : !arg_int("v", 0, 1, &b)) {
            return ok_json(false, (k == "sleep") ? "sleep time: 30-600 s in 10 s steps" : "bad value");
        }
        d[0] = (k == "sleep") ? 5 : 6;
        d[1] = (k == "sleep") ? b / 10 : b;
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
    if (!from_current_page()) {
        return;
    }
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
    nvs_put("chords", c, 1 + n * 5);
    log_edit(EK_CHORDS, String(n) + (n == 1 ? " rule" : " rules"));
    ok_json(true);
}

static void osd_push(const String& s) {
    memmove(osdq + 1, osdq, sizeof(OsdItem) * (OSD_KEEP - 1));
    osdq[0].seq = ++osd_seq;
    osdq[0].ms = millis();
    strncpy(osdq[0].text, s.c_str(), sizeof(osdq[0].text) - 1);
    osdq[0].text[sizeof(osdq[0].text) - 1] = 0;
    if (osd_n < OSD_KEEP) {
        osd_n++;
    }
}

static String osd_stage_text(int s, const char* host = nullptr) {  // "DPI 10000 (stage 2)", the kanata OSD's style
    uint16_t x = u16(23 + 4 * s), y = u16(25 + 4 * s);
    if (!(st[3] & 128) || !x) {  // fw40: the DPI table isn't read yet (right after power-on): "DPI stage 1 (FLOW)", not "DPI 0"
        return "DPI stage " + String(s + 1) + (host ? String(" (") + host + ")" : String(""));
    }
    return "DPI " + String(x) + ((y && y != x) ? "/" + String(y) : String("")) + " (stage " + String(s + 1) + (host ? String(", ") + host : String("")) + ")";
}

// fw108: a change osd_tick saw waits here until the Feather's recent actions say whether a mouse chord made it (a chord event of
// that setting, from about when it changed); within 2.5 s or it wasn't one (the page, the automatic stage, the idle stage, a wake)
static bool cw_on[3] = { false, false, false };
static int16_t cw_from[3], cw_to[3];
static uint8_t cw_n[3];
static uint32_t cw_ms[3], cw_t0[3];
static void chord_wait(uint8_t g, int from, int to) {
    if (!cw_on[g]) {
        cw_from[g] = from;
        cw_n[g] = 0;
        cw_t0[g] = millis();  // fw111
    }
    cw_to[g] = to;
    cw_n[g]++;
    cw_ms[g] = millis();
    cw_on[g] = true;
}
static void chord_log(uint8_t g, int from, int to, uint8_t presses) {
    static const char G[3] = { 's', 'a', 'p' };
    EditLog& e = edit_log[0];
    int f0 = 0, t0 = 0, n0 = 0;
    char c = 0;
    // quick presses of the same setting make one entry: its first value, the latest, and how many
    if (edit_n && (e.kind == EK_CHORD) && (e.dev == 7) && (e.boot == (uint16_t) b_boots) && (millis() - e.ms < 15000) &&
        (sscanf(e.text, "%c:%d:%d:%d", &c, &f0, &t0, &n0) == 4) && (c == G[g])) {
        snprintf(e.text, sizeof(e.text), "%c:%d:%d:%d", c, f0, to, n0 + presses);
        e.t = now_unix();
        e.ms = millis();
        nvs_put("edits", edit_log, sizeof(EditLog) * edit_n);
        return;
    }
    char t[16];
    snprintf(t, sizeof(t), "%c:%d:%d:%d", G[g], from, to, presses);
    log_edit_as(EK_CHORD, String(t), 7, 0);
}
static void chord_tick() {
    for (uint8_t g = 0; g < 3; g++) {
        if (!cw_on[g]) {
            continue;
        }
        uint32_t el = millis() - cw_ms[g];
        bool chord = false;
        if (ev_ok && (ev_read_at + 500 >= cw_ms[g])) {  // the actions as read since about the change
            uint32_t extra = (millis() - ev_read_at) / 1000;
            for (uint8_t k = 0; (k < evb[1]) && !chord; k++) {
                const uint8_t* q = evb + 2 + 6 * k;
                chord = ev_is_chord(q) && (ev_group(q[4]) == g) && ((uint32_t) (q[0] | q[1] << 8) + extra <= el / 1000 + 2);
            }
        }
        if (chord) {
            cw_on[g] = false;
            uint8_t n = pr_count(g, cw_t0[g]);  // fw111: the presses the Feather listed, not the changes the status showed
            chord_log(g, cw_from[g], cw_to[g], n ? n : cw_n[g]);
        } else if (el > 2500) {
            cw_on[g] = false;
        }
    }
}

static void osd_tick() {
    if (!st_ok) {
        return;
    }
    uint8_t f = st[3];
    // while the automatic stage is being set, its own "host → stage" line says it (once, when the mouse confirmed it)
    bool auto_busy = (auto_want != 0xFF) && ((auto_state == 0) || (auto_state == 1) || (auto_state == 5) || (auto_state == 8));
    if ((f & 8) && (st[4] <= 4)) {
        if ((osd_stage >= 0) && (st[4] != osd_stage) && !auto_busy) {
            osd_push(osd_stage_text(st[4]));
            chord_wait(0, osd_stage, st[4]);  // fw108
        }
        osd_stage = st[4];
    }
    if (f & 16) {
        if ((osd_angle != 0x7FFF) && (s8(5) != osd_angle)) {
            osd_push("angle " + String(s8(5)) + "\xC2\xB0");
            chord_wait(1, osd_angle, s8(5));  // fw108
        }
        osd_angle = s8(5);
    }
    if (f & 32) {
        if ((osd_pos != 0x7FFF) && (s8(6) != osd_pos)) {
            osd_push("position " + String(s8(6)));
            chord_wait(2, osd_pos, s8(6));  // fw108
        }
        osd_pos = s8(6);
    }
}

// GET /api/osd?since=N (fw38): the changes after N, oldest first: {"boot","seq","items":[[seq, age ms, "text"],...]}.
// Not counted as use (a PC polls it while the mouse is connected to it).
static void h_osd() {
    long since = server.arg("since").toInt();
    String j = "{\"boot\":" + String((uint16_t) b_boots) + ",\"seq\":" + String(osd_seq) + ",\"items\":[";
    bool first = true;
    for (int k = osd_n - 1; k >= 0; k--) {
        if ((long) osdq[k].seq > since) {
            j += (first ? "[" : ",[") + String(osdq[k].seq) + "," + String(millis() - osdq[k].ms) + ",\"" + String(osdq[k].text) + "\"]";
            first = false;
        }
    }
    j += "]}";
    server.send(200, "application/json", j);
}

// not counted as use: an open tab alone shouldn't keep the radio on while playing
// fw65: GET /api/nvs: every NVS key (all namespaces: the Wi-Fi and RF calibration keep theirs there too) as
// [namespace, key, type, bytes], with the entries used / free / total and what a save can still take
static void h_nvs() {
    touch();
    String j = "{\"keys\":[";
    nvs_iterator_t it = nullptr;
    esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
    bool first = true;
    while (e == ESP_OK) {
        nvs_entry_info_t in;
        nvs_entry_info(it, &in);
        size_t len = 0;
        if ((in.type == NVS_TYPE_BLOB) || (in.type == NVS_TYPE_STR)) {
            nvs_handle_t h;
            if (nvs_open(in.namespace_name, NVS_READONLY, &h) == ESP_OK) {
                if (in.type == NVS_TYPE_BLOB) {
                    nvs_get_blob(h, in.key, NULL, &len);
                } else {
                    nvs_get_str(h, in.key, NULL, &len);
                }
                nvs_close(h);
            }
        }
        j += String(first ? "" : ",") + "[\"" + in.namespace_name + "\",\"" + in.key + "\"," + String((int) in.type) + "," + String((unsigned) len) + "]";
        first = false;
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_stats_t ns;
    j += "]";
    if (nvs_get_stats(NULL, &ns) == ESP_OK) {
        j += ",\"used\":" + String(ns.used_entries) + ",\"free\":" + String(ns.free_entries) + ",\"total\":" + String(ns.total_entries);
    }
    server.send(200, "application/json", j + ",\"room\":" + String(nvs_room()) + ",\"fail\":" + String(d_nvs_fail) + ",\"fail_key\":\"" + d_nvs_fail_key + "\"}");
}

static void h_status() {
    bat_polled_at = millis() | 1;  // fw88
    clock_sync();
    String j;
    j.reserve(6144);  // fw33: the body was ~3-3.6 KB (was 2200: dozens of reallocs per poll); fw47: ~5 KB with 24 cm/360 results and a curve
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
        j += "],\"src\":\"" + dg_src + "\",\"src_t\":" + String(dg_src_t);  // fw39
        j += ",\"sup\":" + String(dg_supported() ? "true" : "false") + ",\"on\":" + String(sup && (evb[50] & 1) ? "true" : "false") +
             ",\"got\":" + String(sup && (evb[50] & 2) ? "true" : "false") + ",\"want\":" + String(dg_wanted() ? "true" : "false") +
             ",\"sends\":" + String(dg_sends) + ",\"temp\":" + String(dgt_on ? "true" : "false") + ",\"applied\":" + String(dg_applied() ? "true" : "false") + "}";  // fw42
    }
    {  // fw46: the speed curve, its hosts, and the Feather's copy (sup = Feather v30+, on = applying now)
        j += ",\"cv\":{\"n\":" + String(cv.n) + ",\"tau\":" + String(cv.tau) + ",\"dpi\":" + String(cv.dpi) + ",\"sp\":[";
        for (uint8_t k = 0; k < cv.n; k++) {
            j += (k ? "," : "") + String(cv.sp[k]);
        }
        j += "],\"g\":[";
        for (uint8_t k = 0; k < cv.n; k++) {
            j += (k ? "," : "") + String(cv.g[k]);
        }
        j += "],\"hosts\":[";
        char h[12];
        for (uint8_t k = 0; k < cv.hn; k++) {
            snprintf(h, sizeof(h), "%08lx", (unsigned long) cv.sig[k]);
            j += (k ? ",\"" : "\"") + String(h) + "\"";
        }
        bool sup = cv_supported() && ev_ok;
        j += "],\"src\":\"" + cv_src + "\",\"sup\":" + String(cv_supported() ? "true" : "false") + ",\"on\":" + String(sup && (evb[60] & 1) ? "true" : "false") +
             ",\"got\":" + String(sup && (evb[60] & 2) ? "true" : "false") + ",\"want\":" + String(cv_wanted() ? "true" : "false") +
             ",\"applied\":" + String(cv_applied() ? "true" : "false") + ",\"sends\":" + String(cv_sends) + ",\"temp\":" + String(cvt_off ? "true" : "false") + ",\"id\":" + String(lib_off ? 0 : cv_id) + ",\"cand\":" + String(lib_off ? 0 : cv_cand) + ",\"tcand\":" + String(cvt_cand ? "true" : "false") + ",\"try\":" + String(cvt_try ? "true" : "false") + ",\"ab\":" + String((cvt_ab_at && (millis() - cvt_ab_at < 12000)) ? "true" : "false") +
             ",\"name\":\"" + String(cv_id ? cvlib[cv_id - 1].name : "") + "\",\"cand_name\":\"" + String(cv_cand ? cvlib[cv_cand - 1].name : "") + "\"" + (lib_off ? ",\"liboff\":true" : "");  // audit 11-01: liboff = the library didn't load; fw48: temp = a BallSheet curve-off run; fw49: ids, names, tcand = its candidate block
        String dc = dc_json(cv_id);  // fw67: the running curve's slow-down gain, and the Feather's copy (dcsup = Feather v39+)
        bool dsup = dc_supported() && ev_ok;
        j += ",\"dc\":" + (dc.length() ? dc : String("null")) + ",\"dcmin\":" + String(dc_min10 / 10.0f, 1) + ",\"dcsup\":" + String(dc_supported() ? "true" : "false") + ",\"dcmax\":" + String(st_ok ? DC_FEATHER_N(st[56]) : DC_N) + ",\"dcon\":" + String(dsup && (evb[60] & 4) ? "true" : "false") +
             ",\"dcapplied\":" + String(dsup && dc_applied() ? "true" : "false") + ",\"dcsends\":" + String(dc_sends) + "}";
    }
    j += String(",\"tdpi\":{\"on\":") + (tdpi_active ? "true" : "false") + ",\"dpi\":" + String(tdpi_val) + ",\"restoring\":" + (tdpi_restore ? "true" : "false") + ",\"bad\":" + String(tdpi_bad) + "}";  // fw28: the calibration's temporary DPI
    {  // fw43: the measured turn (Feather v27: status 246-251, read with the 192-255 block when a turn starts or ends)
        bool sup = turn_supported() && ev_ok;
        int32_t sent = sup ? (int32_t) ((uint32_t) evb[56] | (uint32_t) evb[57] << 8 | (uint32_t) evb[58] << 16 | (uint32_t) evb[59] << 24) : 0;
        j += ",\"turn\":{\"sup\":" + String(sup ? "true" : "false") + ",\"state\":" + String(sup ? evb[54] : 0) + ",\"run\":" + String(sup ? evb[55] : 0) +
             ",\"sent\":" + String((long) sent) + ",\"count\":" + String(sup && (st[56] >= 28) ? "true" : "false") + "}";
        j += ",\"c360\":[";
        for (uint8_t k = 0; k < c360_n; k++) {
            j += String(k ? "," : "") + "[" + String(c360[k].t) + "," + String(c360[k].cm100) + "," + String(c360[k].sp10) + "," + String(c360[k].dpi & 0xFFFF) + "," + String(c360[k].dpi >> 24) + "," + String((c360[k].dpi >> 16) & 0xFF) + "]";  // fw48: [.., dpi, context]; fw49: + curve id
        }
        j += "]";
    }
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
    j += "],\"ed_old\":" + String(edit_old_n);
    if (drst.t || drst_ms) {  // fw112
        j += ",\"drst\":{\"t\":" + String(drst.t) + ",\"on\":" + String(drst.on ? "true" : "false") + ",\"ago\":" + String(drst_ms ? (int32_t) ((millis() - drst_ms) / 1000) : -1) + ",\"fup\":" + String(feather_up_s()) + "}";
    }
    if (usb_ok && st_ok && (st[56] >= 70)) {  // fw105
        const uint8_t uf = (millis() - usb_at < 5000) ? usb_flags : 0;  // fw106: not a state that's stopped being read
        j += String(",\"usb\":{\"resets\":") + String(usb_seen) + ",\"off\":" + String((uf & 1) ? "true" : "false") + ",\"gaveup\":" + String((uf & 2) ? "true" : "false") +
             ",\"stuck\":" + String((uf & 4) ? "true" : "false") + ",\"worst\":" + String(usb_worst) + ",\"mounts\":" + String(usb_mounts) + "}";
    }
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
    j += ",\"qt_fw\":" + String(QT_FW) + ",\"page\":{\"fs\":" + String(page_fs ? "true" : "false") + ",\"t\":" + String(page_fs_t) + ",\"len\":" + String(page_fs_len) + "}";  // fw41
    j += ",\"diag\":{\"polls\":" + String(d_polls) + ",\"ok\":" + String(d_ok) + ",\"short\":" + String(d_short) +
         ",\"bad\":" + String(d_bad) + ",\"i2c_resets\":" + String(d_i2c_resets) + ",\"last_n\":" + String(d_last_n) + ",\"b0\":" + String(d_b0) + ",\"b1\":" + String(d_b1) +
         ",\"rc_fail\":" + String(d_rc_fail) + ",\"rc_ms\":" + String(d_rc_ms) + ",\"rc256\":" + String(d_rc256) + ",\"rc512\":" + String(d_rc512) + ",\"up_s\":" + String(millis() / 1000);
    {  // fw62: the NVS partition's entries (32 bytes each): used, free, total; and saves that failed
        nvs_stats_t ns;
        if (nvs_get_stats(NULL, &ns) == ESP_OK) {
            j += ",\"nvs\":[" + String(ns.used_entries) + "," + String(ns.free_entries) + "," + String(ns.total_entries) + "]";
        }
        j += ",\"nvs_fail\":" + String(d_nvs_fail) + ",\"nvs_fail_key\":\"" + d_nvs_fail_key + "\",\"lib\":\"" + String(lib_fs_on() ? "file" : lib_off ? "off" : "nvs") + "\",\"lib_writes\":" + String(lib_writes) + ",\"lib_fails\":" + String(lib_fails) + ",\"lib_bad\":" + String(lib_bad) + "}";  // fw65: + its key; fw71: where the curve library is
    }
    j += fz_json();  // fw77
    j += pf_json();  // fw82
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
    j += ",\"ui\":" + String(ui_folds) + ",\"ui_tab\":" + String(ui_tab);
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

// fw103: one record in front of a newest-first file, which keeps EDIT_OLD: a whole new <path>.tmp first, then it replaces the file
// (the pattern of cvhist_save: a power cut can't empty it; old_count finishes a replace it cut off)
static void old_push(const char* path, const void* rec, size_t size, uint8_t* count) {
    if (!fs_ok) {
        return;
    }
    String tmp = String(path) + ".tmp";
    File w = FFat.open(tmp, "w");
    if (!w) {
        return;
    }
    bool ok = w.write((const uint8_t*) rec, size) == size;
    uint8_t kept = 1;
    File r = FFat.exists(path) ? FFat.open(path, "r") : File();
    if (r) {
        static uint8_t buf[256];
        size_t left = std::min((size_t) r.size(), size * (EDIT_OLD - 1));
        left -= left % size;
        kept += left / size;
        while (ok && left) {
            size_t got = r.read(buf, std::min(left, sizeof(buf)));
            ok = got && (w.write(buf, got) == got);
            left -= got;
        }
        r.close();
    }
    w.close();
    if (!ok) {
        FFat.remove(tmp);
        return;
    }
    FFat.remove(path);
    FFat.rename(tmp, path);
    *count = kept;
}

static uint8_t old_count(const char* path, size_t size) {
    String tmp = String(path) + ".tmp";
    if (!FFat.exists(path) && FFat.exists(tmp)) {
        FFat.rename(tmp, path);
    }
    File f = FFat.exists(path) ? FFat.open(path, "r") : File();
    if (!f) {
        return 0;
    }
    size_t n = f.size() / size;
    f.close();
    return (n > EDIT_OLD) ? EDIT_OLD : (uint8_t) n;
}

// fw107: the history files wait for the next pass of the loop, one write a pass (oq_tick), so the request that caused them has
// been answered: each file rewrite takes about 0.5 s, and a curve Replace made five. Only history can be lost to a power cut in
// those moments, not the curve
static EditLog oq_e[4];
static uint8_t oq_en = 0;
static void oq_edit_one() {  // the oldest waiting first: the file is newest first
    old_push(EDIT_OLD_BIN, &oq_e[0], sizeof(EditLog), &edit_old_n);
    oq_en--;
    memmove(oq_e, oq_e + 1, sizeof(EditLog) * oq_en);
}

// fw102: automatic stage changes (an ATEN switch) hold at most EDIT_AUTO_KEEP places, so they can't push the page's edits out.
// Drops all but the newest keep of them; everything else keeps its order and moves up
#define EDIT_AUTO_KEEP 4
#define EDIT_CHORD_KEEP 6  // fw108: the same for mouse-chord changes
static void edit_cap_kind(uint8_t kind, uint8_t keep) {
    uint8_t seen = 0, w = 0;
    for (uint8_t r = 0; r < edit_n; r++) {
        if ((edit_log[r].kind == kind) && (edit_log[r].dev == 7) && (seen++ >= keep)) {
            continue;
        }
        if (w != r) {
            edit_log[w] = edit_log[r];
            edit_seq[w] = edit_seq[r];
        }
        w++;
    }
    for (uint8_t k = w; k < edit_n; k++) {
        memset(&edit_log[k], 0, sizeof(edit_log[k]));
        edit_seq[k] = 0;
    }
    edit_n = w;
}
// fw22: record an edit the Feather accepted into its queue, newest first, and save the log
static void log_edit_as(uint8_t kind, const String& text, uint8_t dev, uint32_t ip) {
    if ((kind == EK_AUTO_STAGE) && (dev == 7)) {
        edit_cap_kind(EK_AUTO_STAGE, EDIT_AUTO_KEEP - 1);
    }
    if ((kind == EK_CHORD) && (dev == 7)) {
        edit_cap_kind(EK_CHORD, EDIT_CHORD_KEEP - 1);
    }
    const EditLog& last = edit_log[EDIT_LOG - 1];
    if ((edit_n == EDIT_LOG) && !(((last.kind == EK_AUTO_STAGE) || (last.kind == EK_CHORD)) && (last.dev == 7))) {  // fw108: chords stay out of the older history too
        if (oq_en == (sizeof(oq_e) / sizeof(oq_e[0]))) {  // fw103; fw107: later, unless more than a pass's worth came at once
            oq_edit_one();
        }
        oq_e[oq_en++] = edit_log[EDIT_LOG - 1];
    }
    memmove(edit_log + 1, edit_log, sizeof(EditLog) * (EDIT_LOG - 1));
    memmove(edit_seq + 1, edit_seq, sizeof(edit_seq[0]) * (EDIT_LOG - 1));
    edit_seq[0] = (dev == 7) ? 0 : q_fresh;
    q_fresh = 0;
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
    nvs_put("edits", edit_log, sizeof(EditLog) * edit_n);
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

// v2 (fw34): FNV-1a over the first HSIG_REQS distinct requests (bus resets skipped, SET_ADDRESS's address zeroed, a
// request equal to an earlier one skipped); v1 (fw24-33, for moving saved hosts over): the first 12 as sent
static uint32_t hcap_sig_n(const HostCap& c, uint8_t want, bool distinct) {
    uint8_t n = c.blk[3] <= HCAP_ENTRIES ? c.blk[3] : HCAP_ENTRIES, k = 0;
    uint8_t seen[HSIG_V1_REQS][8];
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; (i < n) && (k < want); i++) {
        const uint8_t* e = c.blk + 16 + 8 * i;
        if ((e[0] == 0xFF) && (e[1] == 0xFF)) {
            continue;
        }
        uint8_t r[8];
        memcpy(r, e, 8);
        if ((r[0] == 0x00) && (r[1] == 0x05)) {
            r[2] = r[3] = 0;
        }
        bool dup = false;
        for (uint8_t j = 0; distinct && (j < k) && !dup; j++) {
            dup = !memcmp(seen[j], r, 8);
        }
        if (dup) {
            continue;
        }
        memcpy(seen[k], r, 8);
        for (uint8_t b = 0; b < 8; b++) {
            h = (h ^ r[b]) * 16777619u;
        }
        k++;
    }
    return (k < want) ? 0 : (h ? h : 1);
}

static uint32_t hcap_sig(const HostCap& c) {
    return hcap_sig_n(c, HSIG_REQS, true);
}

// fw34: a host saved under a v1 signature gets the v2 one of a kept capture that had it (0 = none kept)
static uint32_t sig_v2_of(uint32_t v1) {
    for (uint8_t k = 0; k < hcap_n; k++) {
        if (hcap_sig_n(hcap[k], HSIG_V1_REQS, false) == v1) {
            uint32_t s = hcap_sig(hcap[k]);
            if (s) {
                return s;
            }
        }
    }
    return 0;
}

static void sig_migrate() {
    for (uint8_t k = 0; k < hprof_n; k++) {
        uint32_t s = sig_v2_of(hprof[k].sig);
        if (s) {
            hprof[k].sig = s;
        }
    }
    for (uint8_t a = 0; a < hprof_n; a++) {  // two old signatures of one host: keep the one with a stage, else a name
        for (uint8_t b = a + 1; b < hprof_n;) {
            if (hprof[b].sig != hprof[a].sig) {
                b++;
                continue;
            }
            if ((!hprof[a].stage && hprof[b].stage) || (!hprof[a].stage && !hprof[a].name[0] && hprof[b].name[0])) {
                hprof[a] = hprof[b];
            }
            hprof[b] = hprof[--hprof_n];
        }
    }
    if (hprof_n) {
        nvs_put("hprof", hprof, sizeof(HostProf) * hprof_n);
    }
    for (uint8_t k = 0; k < dg.n; k++) {
        uint32_t s = sig_v2_of(dg.sig[k]);
        if (s) {
            dg.sig[k] = s;
        }
    }
    for (uint8_t a = 0; a < dg.n; a++) {
        for (uint8_t b = a + 1; b < dg.n;) {
            if (dg.sig[b] == dg.sig[a]) {
                dg.sig[b] = dg.sig[--dg.n];
            } else {
                b++;
            }
        }
    }
    if (dg.n) {
        nvs_put("dgain", &dg, sizeof(dg));
    }
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

// fw33 (audit 10-21): the host comes from the first capture with a signature, or from a later one this power-on whose
// signature differs (a UEFI enumeration, then the OS's). fw35 (review 10-21): the mouse's own stage register (0x0004,
// a raw read: [stage, 0x55 - stage]) is read FIRST and the stage written only when it differs, then read again to
// confirm. No answer = asleep: only the read is repeated (every 3 s, every 15 s after a minute), for the whole power-on;
// writes to a sleeping mouse are lost anyway, and each one added a Feather event and a status re-read. A DPI chord seen
// in the Feather's recent actions since the host was taken = the user picked a stage: left alone (state 7).
// States: 0 read the mouse, 8 reading, 1 written (waiting for the Feather), 5 checking, 2 confirmed, 3 gave up (it
// answered with another stage 6 times), 4 no stage for this host, 6 kept (QT Py-only restart), 7 changed by hand.
static bool auto_probe() {  // the Feather has one probe slot: not while the DPI restore uses it
    if (tdpi_restore || (cq_n > 0)) {
        return false;
    }
    uint8_t r[16] = { 0x08, 0, 0x00, 0x04, 2 };
    uint8_t sum = 8;
    for (int i = 0; i < 15; i++) {
        sum += r[i];
    }
    r[15] = 0x55 - sum;
    auto_probe_n = st[91];
    if (!queue_cmd(C_RAW, r, 16)) {
        return false;
    }
    auto_at = millis();
    atl(5);
    return true;
}

static int auto_answer() {  // the mouse's stage from the read, -1 none yet
    bool mine = (st[91] != auto_probe_n) && ((st[92] & 3) == 2) && (st[93] >= 8) && (st[97] == 0x00) && (st[98] == 0x04);
    return mine ? st[100] : -1;
}

static bool stage_chord_since(uint32_t since) {  // a DPI chord on the Feather since then (its recent actions)
    if (!ev_ok) {
        return false;
    }
    uint32_t span = (millis() - since) / 1000 + 1, read_ago = (millis() - ev_read_at) / 1000;
    for (uint8_t k = 0; (k < evb[1]) && (k < 8); k++) {
        const uint8_t* q = evb + 2 + 6 * k;  // newest first: age (s, LE), source, buttons, action, value
        if ((uint32_t) (q[0] | q[1] << 8) + read_ago > span) {
            break;
        }
        if ((q[2] < 6) && (q[4] >= 1) && (q[4] <= 7)) {  // a chord (sources 0-5), GA_DPI_NEXT/PREV/1-5
            return true;
        }
    }
    return false;
}

static void auto_tick() {
    if (hcap_n && (hcap[0].boot == (uint16_t) b_boots) && (hcap[0].blk[1] != auto_cap)) {
        uint32_t sig = hcap_sig(hcap[0]);
        if (sig) {
            auto_cap = hcap[0].blk[1];
            if (sig != auto_sig) {
                bool first = !auto_sig;
                auto_sig = sig;
                HostProf* p = hprof_at(hprof_find(sig));
                auto_want = (p && p->stage >= 1 && p->stage <= 5) ? p->stage - 1 : 0xFF;
                // fw34: kept only if the last boot had already set (or kept) this host's stage
                bool was_set = (auto_prev.magic == AUTO_RTC_MAGIC) && (auto_prev.boots + 1 == b_boots) && (auto_prev.sig == sig) &&
                               ((auto_prev.state == 2) || (auto_prev.state == 6));
                auto_state = (auto_want == 0xFF) ? 4 : (first && feather_ran && was_set) ? 6 : 0;
                auto_tries = 0;
                auto_lost = 0;
                auto_at = 0;
                auto_from = 0xFF;
                auto_since = millis();
                atl(2, auto_state);
            }
        }
    }
    static bool seen_link = false, seen_warg = false;
    if (!seen_link && link_up && st_ok) {
        seen_link = true;
        atl(3);
    }
    if (!seen_warg && st_ok && (st[3] & 1)) {
        seen_warg = true;
        atl(4);
    }
    auto_rtc.magic = AUTO_RTC_MAGIC;  // fw34: for a QT Py-only restart
    auto_rtc.boots = b_boots;
    auto_rtc.sig = auto_sig;
    auto_rtc.state = auto_state;
    bool busy = (auto_state == 0) || (auto_state == 1) || (auto_state == 5) || (auto_state == 8);
    if ((auto_want == 0xFF) || !busy || !link_up || !st_ok || !(st[3] & 1)) {
        return;  // nothing to set, done, or the Warg isn't there yet
    }
    if (stage_chord_since(auto_since)) {
        auto_state = 7;  // the user picked a stage by chord meanwhile
        return;
    }
    uint32_t wait = (auto_lost >= 20) ? 15000 : 3000;  // unanswered: every 3 s, every 15 s after a minute
    if (auto_state == 0) {
        if (((auto_at == 0) || (millis() - auto_at >= wait)) && auto_probe()) {  // fw58: the first read at once (auto_at 0 made it wait until 3 s after power-on)
            auto_state = 8;
        }
        return;
    }
    if ((auto_state == 8) || (auto_state == 5)) {
        int v = auto_answer();
        if (v < 0) {
            if (millis() - auto_at > 3000) {  // no answer (asleep?): read again later (state 0), write nothing
                atl(7);
                auto_lost++;
                auto_state = 0;
            }
            return;
        }
        atl(6, v);
        if (auto_from == 0xFF) {
            auto_from = (uint8_t) v;  // the mouse's stage before any write
        }
        if (v == auto_want) {
            auto_state = 2;
            atl(10);
            if (auto_from != auto_want) {  // fw33: only a real change goes into the log (it holds 20)
                HostProf* p = hprof_at(hprof_find(auto_sig));
                log_edit_as(EK_AUTO_STAGE, String(p ? p->name : "") + "=" + String(auto_want), 7, 0);
                osd_push(osd_stage_text(auto_want, p && p->name[0] ? p->name : "this host"));  // fw38: "DPI 400 (stage 1, FLOW)"
                osd_stage = auto_want;  // shown once, as the host's
            }
            return;
        }
        if (auto_tries >= 6) {
            auto_state = 3;  // it keeps answering with another stage
            return;
        }
        uint8_t b = auto_want;
        if (queue_cmd(C_SET_STAGE, &b, 1)) {
            atl(8, b);
            auto_tries++;
            auto_at = millis();
            auto_state = 1;
        }
        return;
    }
    // 1: written; once the Feather took it, read the mouse back
    if (st[4] == auto_want) {
        if (auto_probe()) {
            atl(9);
            auto_state = 5;
        }
    } else if ((cq_n == 0) && (millis() - auto_at >= 1500)) {
        auto_state = 0;  // the write never showed: read and decide again
        auto_at = 0;
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
         String(auto_state) + ",\"tries\":" + String(auto_tries) + ",\"tl\":[";  // fw57: [ms, code, value]
    for (uint8_t k = 0; k < auto_tl_n; k++) {
        j += (k ? ",[" : "[") + String(auto_tl[k].ms) + "," + String(auto_tl[k].code) + "," + String(auto_tl[k].v) + "]";
    }
    j += "]},\"profiles\":[";
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
    if (!from_current_page()) {
        return;
    }
    uint32_t sig = strtoul(server.arg("sig").c_str(), nullptr, 16);
    long stage;
    if (!sig || !arg_int("stage", 0, 5, &stage)) {
        return ok_json(false, "need sig and stage 0-5");
    }
    String name = server.arg("name");
    name.trim();
    HostProf* p = hprof_at(hprof_find(sig));
    uint8_t was = p ? p->stage : 0;  // fw35: a rename alone mustn't re-apply the stage (review 10-21)
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
    if (hprof_n) {
        nvs_put("hprof", hprof, sizeof(HostProf) * hprof_n);
    } else {
        prefs.remove("hprof");  // fw35: putBytes of 0 bytes writes nothing, so the last forgotten host came back
    }
    log_edit(EK_HOST_STAGE, String(p && (stage || name.length()) ? p->name : "") + "=" + String(stage));
    if ((sig == auto_sig) && ((uint8_t) stage != was)) {  // the host on now, its stage changed: apply it this power-on too
        auto_want = (stage >= 1) ? stage - 1 : 0xFF;
        auto_state = (auto_want == 0xFF) ? 4 : 0;
        auto_tries = 0;
        auto_lost = 0;
        auto_at = 0;
        auto_from = 0xFF;
        auto_since = millis();
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

static void cvmeta_save() {
    if (lib_off) {
        return;  // audit 11-01
    }
    if (lib_fs_on()) {  // fw71: the file
        lib_write();
        return;
    }
    nvs_put("cvmeta", cvmeta, sizeof(cvmeta));
}

static void cvmeta_touch(uint8_t id) {  // fw61: used or changed now
    if (!id || (id > CVLIB_N)) {
        return;
    }
    uint32_t n = now_unix();
    if (n) {
        cvmeta[id - 1].used = n;
        cvmeta_save();
    }
}

static const uint16_t* dg_eff() {  // fw42: the table the Feather should have: the temporary one during an A/B test
    if (dgt_on) {
        return dgt_g;
    }
    if (cvt_cand && cv_cand && cvdg[cv_cand - 1].has) {
        return cvdg[cv_cand - 1].g;  // fw60: the candidate's arm runs the candidate's own gains
    }
    return dg.g;
}

// fw60: the gains in use belong to the running curve: stored with it (each save, and before another curve takes over)
static void cvdg_stamp(uint8_t id) {
    CurveGains* s = cvdg_slot(id);  // fw62: id 0 = no library curve runs: its own slot
    if (!s) {
        return;
    }
    CurveGains& c = *s;
    c.has = 1;
    memcpy(c.g, dg.g, sizeof(c.g));
    c.src_t = dg_src_t;
    memset(c.src, 0, sizeof(c.src));
    strncpy(c.src, dg_src.c_str(), sizeof(c.src) - 1);
    if (id) {
        cvdg_save();
    } else {
        nvs_put("cvdg0", &cvdg0, sizeof(cvdg0));
    }
}

// fw60: a curve becomes the running one: its own gains (if it has them) become the gains in use
static void cvdg_take(uint8_t id) {
    const CurveGains* s = cvdg_slot(id);  // fw62: id 0 = the gains kept while no library curve ran
    if (!s || !s->has) {
        return;
    }
    const CurveGains& c = *s;
    if (memcmp(dg.g, c.g, sizeof(dg.g))) {
        log_edit(EK_DIRGAIN, String("with ") + (id ? String(cvlib[id - 1].name) : String("no curve")));
    }
    memcpy(dg.g, c.g, sizeof(dg.g));
    dg_src = String(c.src);
    dg_src_t = c.src_t;
    nvs_put("dgain", &dg, sizeof(dg));
    prefs.putString("dgsrc", dg_src);
    prefs.putULong("dgsrct", dg_src_t);
    dg_sent_at = 0;
    dg_force = true;
}

static bool dg_eff_want() {
    return dgt_on || dg_wanted();
}

static bool dg_flat() {
    const uint16_t* g = dg_eff();
    for (int k = 0; k < 8; k++) {
        if (g[k] != 1000) {
            return false;
        }
    }
    return true;
}

static uint8_t dg_crc() {  // as the Feather computes it: crc8 of the 16 gain bytes, little-endian
    const uint16_t* g = dg_eff();
    uint8_t raw[16];
    for (int k = 0; k < 8; k++) {
        raw[2 * k] = g[k] & 0xFF;
        raw[2 * k + 1] = g[k] >> 8;
    }
    return crc8(raw, 16);
}

static bool dg_applied() {  // fw42: the Feather holds the table it should (status: BallSheet waits for it between A/B blocks)
    if (!ev_ok || !(evb[50] & 2)) {
        return false;
    }
    bool expect_on = dg_eff_want() && !dg_flat();
    return (evb[51] == dg_crc()) && (((evb[50] & 1) != 0) == expect_on);
}

// the Feather's copy is in status 242-243, which come with the 192-255 block: read only when its change nibble moves,
// so after sending, read it once more ourselves
static void dg_tick() {
    if (!link_up || !dg_supported() || (cq_n > 0)) {
        return;
    }
    if (!auto_sig && (millis() < 6000)) {
        return;  // fw33: the host isn't known yet at power-on: wait for it rather than send "off", then "on"
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
    if (dgt_on && (millis() - dgt_keep_at > 12000)) {
        dgt_on = false;  // fw42: BallSheet stopped checking in: back to the saved table
        dg_sent_at = 0;
        dg_force = true;
    }
    bool want = dg_eff_want();
    if ((!dg_force && dg_applied()) || (millis() - dg_sent_at < 1500)) {
        return;
    }
    const uint16_t* g = dg_eff();
    uint8_t d[17];
    d[0] = want ? 1 : 0;
    for (int k = 0; k < 8; k++) {
        d[1 + 2 * k] = g[k] & 0xFF;
        d[2 + 2 * k] = g[k] >> 8;
    }
    if (queue_cmd(C_SET_DIRGAIN, d, 17)) {
        dg_force = false;
        dg_sent_at = millis();
        dg_reread = true;
        dg_sends++;
    }
}

// POST /api/dgtemp?g=g0,...,g7 | keep=1 | restore=1 (fw42): BallSheet's A/B accuracy test, see dgt_on
static void h_dgtemp() {
    touch();
    if (server.hasArg("restore")) {
        cvt_keep_at = millis() | 1;  // fw81: an arm switch counts as BallSheet checking in (h_cvtry waits 12 s)
        if (dgt_on) {
            dgt_on = false;
            dg_sent_at = 0;
            dg_force = true;
        }
        return ok_json(true);
    }
    if (server.hasArg("keep")) {
        dgt_keep_at = millis();
        return ok_json(dgt_on);
    }
    if (!from_current_page()) {
        return;
    }
    if (!dg_supported()) {
        return ok_json(false, "the Feather has no direction gains (v24+)");
    }
    long v[8];
    if (!csv8(server.arg("g"), 500, 2000, v)) {
        return ok_json(false, "need 8 gains, 500-2000 per mille");
    }
    for (int k = 0; k < 8; k++) {
        dgt_g[k] = v[k];
    }
    cvt_stop();  // fw87: a BallSheet test starting ends the editor's live edit, as /api/cvtemp does
    dgt_on = true;
    dgt_keep_at = millis();
    dg_sent_at = 0;  // send now
    dg_force = true;
    ok_json(true);
}

// POST /api/dirgain?g=g0,...,g7 (per mille, 500-2000)&hosts=sig,sig (8 hex each; empty = off everywhere)
static void h_dirgain() {
    touch();
    if (!from_current_page()) {
        return;
    }
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
    // fw39: a host toggle sends the same gains without a source: keep the one they came from
    bool same = !memcmp(dg.g, g, sizeof(g));
    if (server.hasArg("src") || !same) {
        String src = server.arg("src");
        String clean;
        for (unsigned i = 0; (i < src.length()) && (clean.length() < 120); i++) {
            char c = src[i];
            if (isalnum((unsigned char) c) || (c == ':') || (c == ',') || (c == ';') || (c == '-')) {
                clean += c;
            }
        }
        dg_src = clean;
        dg_src_t = now_unix();
        prefs.putString("dgsrc", dg_src);
        prefs.putULong("dgsrct", dg_src_t);
    }
    memcpy(dg.g, g, sizeof(g));
    dg.n = n;
    memset(dg.sig, 0, sizeof(dg.sig));
    memcpy(dg.sig, sig, sizeof(uint32_t) * n);
    nvs_put("dgain", &dg, sizeof(dg));
    log_edit(EK_DIRGAIN, String(n) + (n == 1 ? " host" : " hosts"));
    cvdg_stamp(cv_id);  // fw60
    cvmeta_touch(cv_id);  // fw62: its gains changed (stamping alone, as on a switch, isn't using it)
    dg_sent_at = 0;  // send now
    dg_force = true;
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
    long sp = server.arg("sp").toInt();  // fw36: BallSheet's cursor speed %
    c.speed = (sp < 0) ? 0 : (sp > 255) ? 255 : sp;
    c.src = (server.arg("src").toInt() == 1) ? 1 : 0;  // fw64
    nvs_put("calib3", calib, sizeof(CalibRec) * calib_n);
    ok_json(true);
}

// POST /api/calibcid?run=<its Unix time>&cid=<curve id 1-31> (fw56): record which curve a saved run used (flags bits
// 3-7, as BallSheet sends them since 10-24), for runs saved before that
static void h_calibcid() {
    touch();
    if (!from_current_page()) {
        return;
    }
    uint32_t run = strtoul(server.arg("run").c_str(), nullptr, 10);
    long cid = server.arg("cid").toInt();
    if ((cid < 0) || (cid > 31)) {
        return ok_json(false, "cid 0-31");
    }
    for (uint8_t k = 0; k < calib_n; k++) {
        if (calib[k].t == run) {
            calib[k].flags = (calib[k].flags & 7) | (uint8_t) (cid << 3);
            nvs_put("calib3", calib, sizeof(CalibRec) * calib_n);
            return ok_json(true);
        }
    }
    ok_json(false, "no such run");
}

// POST /api/calibdel?run=<its Unix time> (fw39): remove a saved run
static void h_calibdel() {
    touch();
    if (!from_current_page()) {
        return;
    }
    long run = server.arg("run").toInt();
    for (uint8_t k = 0; k < calib_n; k++) {
        if ((long) calib[k].t == run) {
            memmove(calib + k, calib + k + 1, sizeof(CalibRec) * (calib_n - k - 1));
            calib_n--;
            if (calib_n) {
                nvs_put("calib3", calib, sizeof(CalibRec) * calib_n);
            } else {
                prefs.remove("calib3");  // putBytes of 0 bytes writes nothing
            }
            return ok_json(true);
        }
    }
    ok_json(false, "no saved run at that time");
}

// ---- fw43: the cm/360 card. POST /api/turn?n=<counts, signed>&r=<counts a second>&hand=<counts> starts the
// Feather's measured turn (v27: it moves the view by itself, steadily); ?stop=1 stops it. Not an edit: no setting
// changes, so nothing is logged.
static bool turn_supported() {
    return st_ok && st_ext && (st[56] >= 27);
}

static void put32le(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = v >> 24;
}

static void h_turn() {
    touch();
    if (!from_current_page()) {
        return;
    }
    if (!turn_supported()) {
        return ok_json(false, "needs Feather v27");
    }
    if (server.hasArg("stop")) {
        return ok_json(queue_cmd(C_TURN, nullptr, 0), "the Feather's queue is full");
    }
    if (server.hasArg("count")) {  // fw44 (Feather v28): count the real X going to the host until ?stop=1
        if (st[56] < 28) {
            return ok_json(false, "needs Feather v28");
        }
        uint8_t one = (server.arg("count") == "2") ? 2 : 1;  // fw55: 2 = after the speed curve (Feather v34)
        if ((one == 2) && (st[56] < 34)) {
            return ok_json(false, "needs Feather v34");
        }
        return ok_json(queue_cmd(C_TURN, &one, 1), "the Feather's queue is full");
    }
    long n = server.arg("n").toInt(), r = server.arg("r").toInt(), hand = server.arg("hand").toInt();
    if ((n == 0) || (labs(n) > 20000000L) || (r < 10) || (r > 20000000L) || (hand < 0)) {
        return ok_json(false, "need n (counts, not 0), r (counts a second) and hand (counts)");
    }
    uint8_t d[12];
    put32le(d, (uint32_t) n);
    put32le(d + 4, (uint32_t) r);
    put32le(d + 8, (uint32_t) hand);
    ok_json(queue_cmd(C_TURN, d, 12), "the Feather's queue is full");
}

// ---- fw46: the speed curve
static bool cv_supported() {
    return st_ok && st_ext && (st[56] >= 30);
}

static bool cv_wanted() {  // on for the host connected now
    if (cvt_off) {
        return false;  // fw48: BallSheet's curve-off run
    }
    for (uint8_t k = 0; auto_sig && (cv.n >= 2) && (k < cv.hn); k++) {
        if (cv.sig[k] == auto_sig) {
            return true;
        }
    }
    return false;
}

// fw47: the Feather's fallback DPI (only while it can't read the mouse's): the connected host's own stage DPI (FLOW's
// stage is 800, the XIM's 40000), else the one saved with the curve
static uint16_t cv_fallback_dpi() {
    for (uint8_t k = 0; auto_sig && st_ok && (st[3] & 128) && (k < hprof_n); k++) {
        if ((hprof[k].sig == auto_sig) && (hprof[k].stage >= 1) && (hprof[k].stage <= 5)) {
            uint16_t v = u16(23 + 4 * (hprof[k].stage - 1));
            return (v >= 50) ? v : cv.dpi;
        }
    }
    return cv.dpi;
}

static uint8_t cv_payload(uint8_t* d) {  // what C_SET_CURVE carries (no points = off)
    uint16_t fb = cv_fallback_dpi();
    if (cvt_try) {  // fw81: the editor's table, on the same hosts
        d[0] = cv_wanted() ? 1 : 0;
        d[1] = try_tau;
        d[2] = fb & 0xFF;
        d[3] = fb >> 8;
        for (uint8_t k = 0; k < try_n; k++) {
            d[4 + 4 * k] = try_sp[k] & 0xFF;
            d[5 + 4 * k] = try_sp[k] >> 8;
            d[6 + 4 * k] = try_g[k] & 0xFF;
            d[7 + 4 * k] = try_g[k] >> 8;
        }
        return 4 + 4 * try_n;
    }
    if (cvt_cand && cv_cand && cvlib[cv_cand - 1].used) {  // fw49: the candidate, on the same hosts
        const CurveLib& c = cvlib[cv_cand - 1];
        d[0] = cv_wanted() ? 1 : 0;
        d[1] = c.tau;
        d[2] = fb & 0xFF;
        d[3] = fb >> 8;
        for (uint8_t k = 0; k < c.n; k++) {
            d[4 + 4 * k] = c.sp[k] & 0xFF;
            d[5 + 4 * k] = c.sp[k] >> 8;
            d[6 + 4 * k] = c.g[k] & 0xFF;
            d[7 + 4 * k] = c.g[k] >> 8;
        }
        return 4 + 4 * c.n;
    }
    d[0] = cv_wanted() ? 1 : 0;
    d[1] = cv.tau;
    d[2] = fb & 0xFF;
    d[3] = fb >> 8;
    for (uint8_t k = 0; k < cv.n; k++) {
        d[4 + 4 * k] = cv.sp[k] & 0xFF;
        d[5 + 4 * k] = cv.sp[k] >> 8;
        d[6 + 4 * k] = cv.g[k] & 0xFF;
        d[7 + 4 * k] = cv.g[k] >> 8;
    }
    return 4 + 4 * cv.n;
}

static bool cv_applied() {  // the Feather holds the curve it should (status 252 applying/got, 253 crc after the "on" byte)
    if (!ev_ok || !(evb[60] & 2)) {
        return false;
    }
    uint8_t d[4 + 4 * CV_N];
    uint8_t len = cv_payload(d);
    return (evb[61] == crc8(d + 1, len - 1)) && (((evb[60] & 1) != 0) == (d[0] != 0));
}

// ---- fw67: the slow-down gain (Feather v39): the running curve's, or the candidate's in BallSheet's candidate block
static bool dc_supported() {
    return cv_supported() && (st[56] >= 39);
}

static uint8_t dc_slot() {
    return (cvt_cand && cv_cand && cvlib[cv_cand - 1].used) ? cv_cand : cv_id;
}

// what C_SET_DECEL carries (at most 1 + 4 * DC_N bytes): [on, n x (share %, gain uint16)]; none = [0]
// fw76: a Feather v47+ gets [on | 2, n x (share 0.1 % uint16, gain uint16)]; an older one whole %: each share rounded,
// a point whose share isn't above the one kept before it left out
static uint8_t dc_payload(uint8_t* d) {
    uint8_t id = dc_slot();
    const CurveDecel* cp = cvt_try ? &try_dc : id ? &cvdc[id - 1] : nullptr;  // fw81: the editor's while it tries
    if (!cp || !cp->n) {
        d[0] = 0;
        return 1;
    }
    const CurveDecel& c = *cp;
    if (st_ok && (c.n > DC_FEATHER_N(st[56]))) {
        d[0] = 0;  // fw98 (review): more points than this Feather takes (v68 for 7-10): sent as none, so it isn't refused and resent forever (status cv.dcmax: the page says why)
        return 1;
    }
    uint8_t n = (c.n > DC_N) ? DC_N : c.n;  // fw76: never past the caller's buffer
    d[0] = cv_wanted() ? 1 : 0;
    if (st[56] >= 47) {
        d[0] |= 2;
        for (uint8_t k = 0; k < n; k++) {
            d[1 + 4 * k] = c.p10[k] & 0xFF;
            d[2 + 4 * k] = c.p10[k] >> 8;
            d[3 + 4 * k] = c.g[k] & 0xFF;
            d[4 + 4 * k] = c.g[k] >> 8;
        }
        uint8_t len = 1 + 4 * n;
        if (st[56] >= 73) {  // fw109: the flick threshold, last
            d[0] |= 4;
            d[len++] = dc_min10 & 0xFF;
            d[len++] = dc_min10 >> 8;
        }
        return len;
    }
    uint8_t len = 1, last = 0;
    for (uint8_t k = 0; k < n; k++) {
        uint8_t p = (uint8_t) std::max(1, std::min(99, (c.p10[k] + 5) / 10));
        if (p <= last) {
            continue;
        }
        last = p;
        d[len++] = p;
        d[len++] = c.g[k] & 0xFF;
        d[len++] = c.g[k] >> 8;
    }
    return len;
}

static bool dc_applied() {  // status 252 bit 2 applying, bit 3 got one; 254 crc after the "on" byte (0 for none)
    if (!ev_ok || !(evb[60] & 8)) {
        return false;
    }
    uint8_t d[3 + 4 * DC_N];  // fw109: + the flick threshold
    uint8_t len = dc_payload(d);
    return (evb[62] == ((len > 1) ? crc8(d + 1, len - 1) : 0)) && (((evb[60] & 4) != 0) == ((d[0] & 1) != 0));  // fw76: bit 0 (bit 1 = the format)
}

// as dg_tick: 252-253 come with the 192-255 block, so after sending, read it once more ourselves
static void cv_tick() {
    if ((cvt_off || cvt_cand) && (millis() - cvt_keep_at > 12000)) {
        cvt_off = false;
        cvt_cand = false;  // fw48: BallSheet stopped checking in (before any other check: it must end even with the Feather away)
        cv_force = true;
        cv_sent_at = 0;
        dg_force = true;  // fw60
        dg_sent_at = 0;
    }
    if (cvt_try && (millis() - cvt_seen > CVT_LEASE_MS)) {
        cvt_stop();  // fw87: the live edit's page stopped checking in (gone without pagehide)
    }
    if (!link_up || !cv_supported() || (cq_n > 0)) {
        return;
    }
    if (!auto_sig && (millis() < 6000)) {
        return;  // the host isn't known yet at power-on
    }
    if (cv_reread) {
        if (millis() - cv_sent_at >= 300) {
            cv_reread = false;
            read_chords();
        }
        return;
    }
    if (!ev_ok || (millis() - cv_sent_at < 1500)) {
        return;
    }
    uint8_t d[4 + 4 * CV_N];
    bool sent = false;
    if (cv_force || !cv_applied()) {
        uint8_t len = cv_payload(d);
        if (!queue_cmd(C_SET_CURVE, d, len)) {
            return;
        }
        cv_force = false;
        cv_sends++;
        sent = true;
        dc_force = true;  // fw67: the slow-down gain right behind it
    }
    if (dc_supported() && !lib_off && (dc_force || !dc_applied())) {  // audit 11-01: the curve's slow-down gain is in the library
        uint8_t len = dc_payload(d);
        if (queue_cmd(C_SET_DECEL, d, len)) {
            dc_force = false;
            dc_sends++;
            sent = true;
        }
    }
    if (sent) {
        cv_sent_at = millis();
        cv_reread = true;
    }
}

// ---- fw50: the live view (Feather v32): its hand speed every 5 ms and the time spent at each speed, read over the
// link's selected blocks only while the page's speed curve chart asks (GET /api/live in the last 3 s)
#define LV_SEL 0xF5   // a 2-byte write [LV_SEL, block]: the Feather's next read answers that block
#define LV_LEN 80     // Feather v33: 32 samples a read (v32: 48 bytes, 16 samples)
#define LH_LEN 116
#define LV_RING 512   // samples kept here (2.5 s)
#define LV_GAP 0xFFFF // a sample that wasn't read (a break in the trace)
static uint32_t lv_want_at = 0;
static uint32_t lv_read_at = 0, lh_read_at = 0;
static uint16_t lv_ring[LV_RING];
static uint32_t lv_n = 0;  // samples kept so far (the Feather's 16-bit counter, extended here)
static uint16_t lv_raw = 0;
static bool lv_have = false;
static uint8_t lv_blk[LV_LEN];
static uint8_t lh_blk[LH_LEN];
static bool lh_have = false;
static uint32_t lv_reads = 0, lv_fails = 0;
static uint32_t lvf_wr = 0, lvf_short = 0, lvf_crc = 0, lvf_mark = 0, lvf_ms = 0;  // fw52: why live reads failed
static uint8_t lvf_wr_code = 0, lvf_short_n = 0, lvf_bad[LH_LEN], lvf_bad_len = 0, lvf_prev[LH_LEN], lvf_prev_len = 0;

static bool live_supported() {
    return st_ok && st_ext && (st[56] >= 32);
}

// The Feather takes the select write in its lowest-priority interrupt (the mouse's USB work comes first). When that
// interrupt is late, the read arrives before the write was taken and the answer is the status block ("G", seen in
// ~1.7 % of reads, QT Py fw52's capture): so wait a little between the two, and select again once if it still happens.
#define LV_SEL_WAIT_US 600
static uint32_t lvf_lost = 0, lvf_retry_ok = 0;

static uint8_t read_sel_once(uint8_t which, uint8_t* b, uint8_t len) {  // 0 ok, 1 failed, 2 got the status block
    uint32_t t0 = millis();
    Wire1.beginTransmission(LINK_ADDR);
    Wire1.write(LV_SEL);
    Wire1.write(which);
    uint8_t e = Wire1.endTransmission();
    if (e != 0) {
        lvf_wr++;
        lvf_wr_code = e;
        lvf_ms = millis() - t0;
        return 1;
    }
    delayMicroseconds(LV_SEL_WAIT_US);
    uint8_t n = Wire1.requestFrom((uint8_t) LINK_ADDR, len);
    if (n != len) {
        while (Wire1.available()) {
            Wire1.read();
        }
        lvf_short++;
        lvf_short_n = n;
        lvf_ms = millis() - t0;
        return 1;
    }
    for (uint8_t i = 0; i < len; i++) {
        b[i] = Wire1.read();
    }
    if ((b[0] == 'G') && (b[1] == 1)) {
        return 2;
    }
    if (crc8(b, len - 1) != b[len - 1]) {
        lvf_crc++;
        memcpy(lvf_bad, b, len);
        lvf_bad_len = len;
        return 1;
    }
    if ((b[0] != 'L') && (b[0] != 'H') && (b[0] != 'F') && (b[0] != 'P') && (b[0] != 'R')) {  // fw77: + 'F', block 4; fw82: + 'P', block 5; fw95: + 'R', block 8
        lvf_mark++;
        return 1;
    }
    if (which == 1) {
        memcpy(lvf_prev, b, len);  // the last good live block, to compare a bad one with
        lvf_prev_len = len;
    }
    return 0;
}

static bool read_sel(uint8_t which, uint8_t* b, uint8_t len) {
    uint8_t r = read_sel_once(which, b, len);
    if (r == 2) {
        lvf_lost++;
        r = read_sel_once(which, b, len);
        if (r == 0) {
            lvf_retry_ok++;
        } else if (r == 2) {
            lvf_lost++;
        }
    }
    return r == 0;
}
// fw77: what the Feather's last freeze left (Feather v48: block 4, layout in the fork's gw_ota.h). While the Feather says it
// holds one (status byte 2 bit 4; during an update, block 3 byte 2 bit 2) it's read every 5 s. One that differs from the
// last read this boot is a new freeze: kept with the time it was read, here and in NVS ("fz"), and counted.
#define FZ_LEN 32
struct FzRec {
    uint32_t t;      // unix time it was read (0: before the clock was known)
    uint32_t total;  // freezes read since the first one kept
    uint8_t blk[FZ_LEN];
};
static FzRec fz = {};
static bool fz_have = false, fz_flag = false, fz_flag_ota = false, fz_seen_ok = false;
static uint8_t fz_seen[FZ_LEN];
static uint32_t fz_at = 0, fz_ms = 0, fz_reads = 0, fz_fails = 0;
static uint16_t fz_boot = 0;

static void fz_check(bool flag) {
    fz_flag = flag;
    if (!flag || (fz_at && (millis() - fz_at < 5000))) {
        return;
    }
    fz_at = millis() | 1;
    uint8_t b[FZ_LEN];
    if (!read_sel(4, b, FZ_LEN) || (b[0] != 'F') || (b[1] != 1)) {
        fz_fails++;
        return;
    }
    fz_reads++;
    if (!b[3] || (fz_seen_ok && !memcmp(b, fz_seen, FZ_LEN))) {
        return;
    }
    memcpy(fz_seen, b, FZ_LEN);
    fz_seen_ok = true;
    memcpy(fz.blk, b, FZ_LEN);
    fz.t = now_unix();
    fz.total++;
    fz_have = true;
    fz_ms = millis();
    fz_boot = (uint16_t) b_boots;
    nvs_put("fz", &fz, sizeof(fz));
}

// fw82: where the Feather's frames go (Feather v50: block 5, layout in the fork's gw_mouse.h), read once a second;
// fw83: Feather v51's layout 2 (96 bytes: gw_tick's parts, the heap)
#define PF_LEN 112  // fw85: Feather v55's layout 3 (24 sections); 96 for layout 2
static uint8_t pf_blk[PF_LEN];
static bool pf_have = false;
static uint32_t pf_at = 0, pf_reads = 0, pf_fails = 0;
static uint32_t pf_good_at = 0;  // fw87: the last good read (prof age_ms)

static void pf_stale() {  // fw87: no current profile (a failed read, an older Feather, the link down)
    pf_have = false;
}

// fw91 (Feather v59): frame timing over the last second, minute, hour and 24 hours, for all frames or for still and moving
// frames apart. Each Feather second comes as link blocks 6 (still frames) and 7 (moving), layout 4 in the fork's
// gw_mouse.cc, read when block 6's second number moves (checked twice a second); block 5 (all frames) is read with them for
// the status's prof and the link interrupt. Kept in RAM (a QT Py restart starts over): the last 60 seconds as read, and
// 5-minute and 1-hour slots by the QT Py's clock, so time the link was down shows as seconds without data, never as old
// data. Averages are frame-weighted, Worst the largest, Worst frame the parts of the slowest frame seen (its own parts 0-12
// summed); frames skipped from the blocks' uncapped since-start count (a smaller uptime: the Feather restarted, all of it
// counts). fw90 (Feather v55-v58: all frames only, paired with the status's perf bytes) is gone: those get no windows.
#define PW_NS 24
#define PW_MAIN 13  // the Feather's GW_PROF_MAIN: a frame is sections 0-12 (13-19 are inside 7, 20-23 inside 0)
#define PC_LEN 128
#define GP_LEN 48  // fw105: 48 from Feather v70 (layout 3: the USB-A port); fw95: block 8 (Feather v63), the mouse's report pauses; fw96: layout 2 (Feather v66), 44 bytes
#define GP_B 5     // 3-4, 5-9, 10-19, 20-49, 50+ ms
#define PW_FIVE 300000UL
#define PW_HOUR 3600000UL
struct PwCls {  // one kind of frame over one Feather second, as its block holds it
    uint16_t frames, ft10, ftmx;  // remapping average (0.1 us), worst (us)
    uint8_t fc10, fcmx, td4, tdmx4, sof4, sofmx4;  // chord check (0.1 us, us); frame finished, SOF interrupt (4 us)
    uint16_t nodpi;  // fw92 (Feather v60): of the frames, those told apart by raw motion (no DPI known)
    uint8_t s[PW_NS][4];  // per part: average (0.1 us, LE), worst (4 us), in the slowest frame (4 us)
};
struct PwSec {
    uint32_t t;       // millis() when read
    uint32_t missed;  // frames skipped since the second before it was read (fw93, Feather v62+: outside settling)
    uint32_t smissed; // fw93: of them, while the Feather settled (its start, a USB change): no fault, shown apart
    uint32_t irq_us;
    uint16_t irq_n;
    uint16_t gap;     // fw92: Feather seconds since the second read before it (1 = none missed; its missed covers them all)
    bool has5;        // fw92: block 5 read with it (irq_*)
    bool hasg;        // fw95: block 8 read with it (Feather v63: the mouse's report pauses)
    uint16_t gp[GP_B];  // fw95: report pauses since the second before it, by length (GP_B buckets)
    uint16_t gms, glong, gfr;  // fw95: paused ms (3-49 ms ones), the longest 3-49 ms one, frames the check covered
    uint16_t jmax, jn;  // fw97 (Feather v67): the 1 ms USB frame interrupt's worst jitter this second (us), how many 50 us or more
    PwCls c[2];  // still, moving
};
struct PwAggC {
    uint32_t frames, nodpi;
    float ft, fc, td, sof;  // sums of average x frames (us)
    uint16_t ftmx, tdmx, sofmx;  // us
    uint8_t fcmx;
    float s[PW_NS];  // sums of average x frames (0.1 us)
    uint8_t smx[PW_NS], swf[PW_NS];  // 4 us
    uint16_t wtot;  // the slowest frame's parts 0-12 summed (4 us)
};
struct PwAgg {
    uint32_t bin, n, missed, smissed, irq_s;  // bin: the slot's number + 1 (millis / slot length; 0 = empty); n: seconds in it; irq_s: of them with block 5
    float irq_us, irq_n;
    uint32_t gs, gp[GP_B], gms, gfr;  // fw95: seconds with block 8, report pauses by length, paused ms, frames checked
    uint16_t glong;
    uint16_t jmax;  // fw97: the USB frame interrupt's worst jitter (us)
    uint32_t jn;    // fw97: its intervals 50 us or more off
    PwAggC c[2];
};
static PwSec pw_s[60];
static uint8_t pw_sh = 0, pw_sn = 0;
static PwAgg pw_f[12], pw_h[24];  // 5-minute and 1-hour slots
static uint32_t pw_mtot = 0, pw_up = 0, pw_last_t = 0, pw_stot = 0;
static uint32_t pw_gtot[GP_B], pw_gmtot = 0;  // fw95: block 8's since-start counts as last read
static uint32_t pw_gftot = 0, pw_g_up = 0, pw_g_t = 0;  // fw96: its frames checked since the start, the Feather's uptime and millis() then
static bool pw_g_ok = false;
static bool pw_mtot_ok = false;
static int32_t feather_up_s() {  // fw113
    return pw_mtot_ok ? (int32_t) (pw_up + (millis() - pw_last_t) / 1000) : -1;
}
static uint16_t pc_seq = 0;
static bool pc_seq_ok = false;
static uint32_t pc_reads = 0, pc_fails = 0;

static inline uint16_t mx16(uint16_t a, uint16_t b) { return (a > b) ? a : b; }
static inline uint8_t mx8(uint8_t a, uint8_t b) { return (a > b) ? a : b; }

// (void* parameters: the Arduino build puts its function prototypes above these structs)
static void pwc_add_sec(void* ap, const void* cp) {  // one kind's second into a sum
    PwAggC& a = *(PwAggC*) ap;
    const PwCls& c = *(const PwCls*) cp;
    if (!c.frames) {
        return;
    }
    float f = c.frames;
    bool first = !a.frames;
    a.frames += c.frames;
    a.nodpi += c.nodpi;
    a.ft += c.ft10 / 10.0f * f;
    a.fc += c.fc10 / 10.0f * f;
    a.td += c.td4 * 4.0f * f;
    a.sof += c.sof4 * 4.0f * f;
    a.ftmx = mx16(a.ftmx, c.ftmx);
    a.fcmx = mx8(a.fcmx, c.fcmx);
    a.tdmx = mx16(a.tdmx, (uint16_t) (c.tdmx4 * 4));
    a.sofmx = mx16(a.sofmx, (uint16_t) (c.sofmx4 * 4));
    uint16_t wt = 0;
    for (uint8_t k = 0; k < PW_NS; k++) {
        a.s[k] += (uint16_t) (c.s[k][0] | c.s[k][1] << 8) * f;
        a.smx[k] = mx8(a.smx[k], c.s[k][2]);
        if (k < PW_MAIN) {
            wt += c.s[k][3];
        }
    }
    if (first || (wt > a.wtot)) {
        a.wtot = wt;
        for (uint8_t k = 0; k < PW_NS; k++) {
            a.swf[k] = c.s[k][3];
        }
    }
}

static void pwc_merge(void* ap, const void* bp) {
    PwAggC& a = *(PwAggC*) ap;
    const PwAggC& b = *(const PwAggC*) bp;
    if (!b.frames) {
        return;
    }
    bool first = !a.frames;
    a.frames += b.frames;
    a.nodpi += b.nodpi;
    a.ft += b.ft;
    a.fc += b.fc;
    a.td += b.td;
    a.sof += b.sof;
    a.ftmx = mx16(a.ftmx, b.ftmx);
    a.fcmx = mx8(a.fcmx, b.fcmx);
    a.tdmx = mx16(a.tdmx, b.tdmx);
    a.sofmx = mx16(a.sofmx, b.sofmx);
    for (uint8_t k = 0; k < PW_NS; k++) {
        a.s[k] += b.s[k];
        a.smx[k] = mx8(a.smx[k], b.smx[k]);
    }
    if (first || (b.wtot > a.wtot)) {
        a.wtot = b.wtot;
        memcpy(a.swf, b.swf, sizeof(a.swf));
    }
}

static void pw_add_sec(void* ap, const void* sp) {
    PwAgg& a = *(PwAgg*) ap;
    const PwSec& s = *(const PwSec*) sp;
    a.n++;
    a.missed += s.missed;
    a.smissed += s.smissed;
    if (s.has5) {
        a.irq_s++;
        a.irq_us += s.irq_us;
        a.irq_n += s.irq_n;
    }
    if (s.hasg) {  // fw95
        a.gs++;
        for (uint8_t k = 0; k < GP_B; k++) {
            a.gp[k] += s.gp[k];
        }
        a.gms += s.gms;
        a.gfr += s.gfr;
        a.glong = mx16(a.glong, s.glong);
        a.jmax = mx16(a.jmax, s.jmax);  // fw97
        a.jn += s.jn;
    }
    pwc_add_sec(&a.c[0], &s.c[0]);
    pwc_add_sec(&a.c[1], &s.c[1]);
}

static void pw_merge(void* ap, const void* bp) {
    PwAgg& a = *(PwAgg*) ap;
    const PwAgg& b = *(const PwAgg*) bp;
    a.n += b.n;
    a.missed += b.missed;
    a.smissed += b.smissed;
    a.irq_s += b.irq_s;
    a.irq_us += b.irq_us;
    a.irq_n += b.irq_n;
    a.gs += b.gs;  // fw95
    for (uint8_t k = 0; k < GP_B; k++) {
        a.gp[k] += b.gp[k];
    }
    a.gms += b.gms;
    a.gfr += b.gfr;
    a.glong = mx16(a.glong, b.glong);
    a.jmax = mx16(a.jmax, b.jmax);  // fw97
    a.jn += b.jn;
    pwc_merge(&a.c[0], &b.c[0]);
    pwc_merge(&a.c[1], &b.c[1]);
}

static void pw_bin_add(void* ring, uint8_t len, uint32_t bin, const void* sp) {
    PwAgg& a = ((PwAgg*) ring)[bin % len];
    if (a.bin != bin) {
        memset(&a, 0, sizeof(a));  // a slot from an earlier hour (day) or never used
        a.bin = bin;
    }
    pw_add_sec(&a, sp);
}

static bool pc_parse(const uint8_t* q, uint8_t kind, void* cp) {
    PwCls& c = *(PwCls*) cp;
    if ((q[0] != 'P') || (q[1] != 4) || (q[2] != PW_NS) || (q[7] != kind)) {
        return false;
    }
    c.frames = q[3] | q[4] << 8;
    c.ft10 = q[8] | q[9] << 8;
    c.ftmx = q[10] | q[11] << 8;
    c.fc10 = q[12];
    c.fcmx = q[13];
    c.td4 = q[14];
    c.tdmx4 = q[15];
    c.sof4 = q[16];
    c.sofmx4 = q[17];
    c.nodpi = q[122] | q[123] << 8;
    memcpy(c.s, q + 18, sizeof(c.s));
    return true;
}

// a new Feather second: blocks 6 and 7 (b5, block 5 of the same moment, or nullptr; fw95: b8, block 8 of that second, or nullptr)
static bool pw_record(const uint8_t* b6, const uint8_t* b7, const uint8_t* b5, const uint8_t* b8) {
    PwSec& s = pw_s[pw_sh];
    if (!pc_parse(b6, 0, &s.c[0]) || !pc_parse(b7, 1, &s.c[1])) {
        return false;
    }
    uint32_t mt = le32(b6 + 114), up = le32(b6 + 118), now = millis();
    // fw92: a restart is an uptime that went back, or one that moved on less than the QT Py's clock did (a restart in a
    // long gap, its uptime already past the old one)
    uint32_t wall = pw_mtot_ok ? (now - pw_last_t) / 1000 : 0;
    bool restarted = pw_mtot_ok && ((up < pw_up) || (up - pw_up + 5 < wall));
    // fw112: a Feather restart starts its counts over by itself: they're no longer since the reset. After a QT Py restart the
    // first second read checks the stored reset: an uptime below the one then, or (clock set) less time on it than went by
    if (drst.on) {
        uint32_t nu = now_unix();
        bool lost = pw_mtot_ok ? restarted : ((up < drst.up) || (nu && drst.t && (nu > drst.t) && (nu - drst.t > up - drst.up + 120)));
        if (lost) {
            drst.on = 0;
            nvs_put("drst", &drst, sizeof(drst));
        }
    }
    s.missed = !pw_mtot_ok ? 0 : restarted ? mt : (mt >= pw_mtot) ? mt - pw_mtot : mt;
    uint32_t stot = b6[124] | b6[125] << 8 | (uint32_t) b6[126] << 16;  // fw93 (Feather v62): skipped while settling, uint24
    s.smissed = !pw_mtot_ok ? 0 : restarted ? stot : (stot >= pw_stot) ? stot - pw_stot : stot;
    pw_stot = stot;
    uint32_t gap = !pw_mtot_ok ? 1 : restarted ? (up ? up : 1) : (up - pw_up);
    s.gap = (gap > 65535) ? 65535 : (gap ? gap : 1);
    pw_mtot = mt;
    pw_up = up;
    pw_last_t = now;
    pw_mtot_ok = true;
    s.has5 = (b5 != nullptr);
    s.irq_us = b5 ? le32(b5 + 5) : 0;
    s.irq_n = b5 ? (uint16_t) (b5[9] | b5[10] << 8) : 0;
    // fw95: the report pauses from block 8's since-start counts (a restart: all of them count, as missed)
    s.hasg = (b8 != nullptr);
    memset(s.gp, 0, sizeof(s.gp));
    s.gms = s.glong = s.gfr = 0;
    s.jmax = s.jn = 0;
    if (b8) {
        // fw96 (review 11-06): a restart by block 8's own uptime (as blocks 6-7: smaller, or moved on less than the clock),
        // so a restart seen on a second without block 8 doesn't leave the old totals as the base
        uint32_t up8 = le32(b8 + 32), wall8 = pw_g_ok ? (now - pw_g_t) / 1000 : 0;
        bool base = pw_g_ok && (up8 >= pw_g_up) && (up8 - pw_g_up + 5 >= wall8);
        for (uint8_t k = 0; k < GP_B; k++) {
            uint32_t c = le32(b8 + 4 + 4 * k), d = !pw_g_ok ? 0 : (!base || (c < pw_gtot[k])) ? c : c - pw_gtot[k];
            s.gp[k] = (d > 65535) ? 65535 : d;
            pw_gtot[k] = c;
        }
        uint32_t m = le32(b8 + 24), dm = !pw_g_ok ? 0 : (!base || (m < pw_gmtot)) ? m : m - pw_gmtot;
        s.gms = (dm > 65535) ? 65535 : dm;
        pw_gmtot = m;
        s.glong = b8[28] | b8[29] << 8;
        if (st[56] >= 67) {  // fw97: the USB frame interrupt's jitter this second
            s.jmax = b8[30] | b8[31] << 8;
            s.jn = b8[40] | b8[41] << 8;
        }
        uint32_t fr = le32(b8 + 36), dfr = !pw_g_ok ? 0 : (!base || (fr < pw_gftot)) ? fr : fr - pw_gftot;  // fw96: since the start, as the counts
        s.gfr = (dfr > 65535) ? 65535 : dfr;
        pw_gftot = fr;
        pw_g_up = up8;
        pw_g_t = now;
        pw_g_ok = true;
    }
    s.t = now;
    pw_sh = (pw_sh + 1) % 60;
    if (pw_sn < 60) {
        pw_sn++;
    }
    pw_bin_add(pw_f, 12, now / PW_FIVE + 1, &s);
    pw_bin_add(pw_h, 24, now / PW_HOUR + 1, &s);
    return true;
}

static void pf_check() {
    if (fo_busy() || (st[56] < 55)) {  // fw85: layout 3 only (Feather v55)
        return;
    }
    bool split = (st[56] >= 59);  // fw91
    if (pf_at && (millis() - pf_at < (split ? 500 : 1000))) {
        return;
    }
    pf_at = millis() | 1;
    uint8_t b[PF_LEN];
    if (split) {
        // fw91: twice a second, block 6 alone; when its second number moved, block 7 (the same number: one retry if a new
        // second came between the two reads) and block 5
        static uint8_t b6[PC_LEN], b7[PC_LEN];
        if (!read_sel(6, b6, PC_LEN) || (b6[0] != 'P') || (b6[1] != 4)) {
            pc_fails++;
            return;
        }
        uint16_t seq = b6[5] | b6[6] << 8;
        if (pc_seq_ok && (seq == pc_seq)) {
            return;  // the same second as last time
        }
        bool ok = false;
        for (uint8_t tries = 0; (tries < 2) && !ok; tries++) {
            if (tries && (!read_sel(6, b6, PC_LEN) || (b6[0] != 'P') || (b6[1] != 4))) {
                break;
            }
            ok = read_sel(7, b7, PC_LEN) && ((b7[5] | b7[6] << 8) == (b6[5] | b6[6] << 8));
        }
        if (!ok) {
            pc_fails++;
            return;
        }
        bool have5 = read_sel(5, b, PF_LEN) && (b[0] == 'P') && (b[1] == 3) && (b[2] <= 24);
        // fw95 (Feather v63): block 8, the mouse's report pauses, of the same second (else that second has none: its counts
        // land in the next one read, they're since the Feather started)
        static uint8_t b8[GP_LEN];
        const uint8_t g_len = (st[56] >= 70) ? GP_LEN : 44, g_lay = (st[56] >= 70) ? 3 : 2;  // fw105
        bool have8 = (st[56] >= 66) && read_sel(8, b8, g_len) && (b8[0] == 'R') && (b8[1] == g_lay) && ((b8[2] | b8[3] << 8) == (b6[5] | b6[6] << 8));
        if (have8 && (g_lay == 3)) {
            usb_note(b8);
        }
        if (pw_record(b6, b7, have5 ? b : nullptr, have8 ? b8 : nullptr)) {
            pc_seq = b6[5] | b6[6] << 8;
            pc_seq_ok = true;
            pc_reads++;
        } else {
            pc_fails++;
        }
        if (!have5) {
            pf_fails++;
            pf_have = false;
            return;
        }
    } else if (!read_sel(5, b, PF_LEN) || (b[0] != 'P') || (b[1] != 3) || (b[2] > 24)) {
        pf_fails++;
        pf_have = false;  // fw87: the last good one isn't this second's
        return;
    }
    memcpy(pf_blk, b, PF_LEN);
    pf_have = true;
    pf_good_at = millis();
    pf_reads++;
}

// prof: frames that second, the link interrupt's us and count that second, and per section [average us, worst us,
// its us in the second's worst frame]
static String pf_json() {
    String j = ",\"prof\":{\"reads\":" + String(pf_reads) + ",\"fails\":" + String(pf_fails);
    if (pf_reads) {
        j += ",\"age_ms\":" + String(millis() - pf_good_at);  // fw87: since the last good read (none yet: left out)
    }
    if (pf_have && !fo_busy()) {  // fw87: not while a Feather update runs (nothing reads the block then)
        static const char* const NAME[] = { "wait from tick", "idle stage + chords", "direction gains", "turn", "speed curve", "turn count",
                                            "tap/sticky/layers", "registers + gw_tick", "macro queue", "mappings", "macros, accumulate",
                                            "reports", "out reports", "tick: wake, events, chords, DPI", "tick: mouse settings",
                                            "tick: idle stage", "tick: link frame + update", "tick: status (every 20 ms)", "tick: status host part + hand-off",
                                            "tick: live/hist blocks + diag", "loop: USB host task", "loop: report parsing", "loop: G-Wolves hook",
                                            "loop: wait for the pass before", "?" };  // fw84: Feather v52's gw_tick parts; fw85: v55's loop pass
        const uint8_t* b = pf_blk;
        j += ",\"frames\":" + String(b[3] | b[4] << 8) + ",\"irq_us\":" + String(le32(b + 5)) + ",\"irq_n\":" + String(b[9] | b[10] << 8) +
             ",\"heap_used\":" + String((uint32_t) (b[11] | b[12] << 8) * 16) + ",\"heap_arena\":" + String((uint32_t) (b[13] | b[14] << 8) * 16) + ",\"sec\":[";
        for (uint8_t k = 0; k < b[2]; k++) {
            const uint8_t* p = b + 15 + 4 * k;
            j += String(k ? "," : "") + "[\"" + String(NAME[(k < 24) ? k : 24]) + "\"," + String((p[0] | p[1] << 8) / 10.0f, 1) + "," + String(p[2] * 4) + "," + String(p[3] * 4) + "]";
        }
        j += "]";
    }
    return j + "}";
}

// fw91: GET /api/perfwin?w=s|m|h|d&k=a|i|m   frame timing over the last second, minute, hour or 24 hours, for all frames
// (k=a), still ones (i) or moving ones (m), shaped as /api/status's perf and prof. n = seconds with data, win_s = the seconds
// the window's slots reach back (h: the 5-minute slot running now and 11 before it, d: the hour and 23 before), frames =
// that kind's frames, frames_all = all of them; perf.missed = frames skipped in the window (all frames: a skipped frame
// has no kind). {"sup":false} = the Feather answers and is older than v59; fw92: down = no answer from it now (what was
// kept is still served); nodpi = of that kind's frames, those told apart by raw motion (no DPI known); a second read
// after a gap longer than the second (minute) window doesn't bring its skipped frames into it
static void h_perfwin() {
    bool down = !link_up || !st_ok;
    if (!down && (st[56] < 59)) {
        server.send(200, "application/json", "{\"sup\":false}");
        return;
    }
    String w = server.arg("w"), k = server.arg("k");
    uint32_t now = millis(), win = 0;
    PwAgg a;
    memset(&a, 0, sizeof(a));
    if ((w == "s") || (w == "m")) {
        bool sec = (w == "s");
        win = sec ? 1 : 60;
        for (uint8_t i = 0; i < (sec ? (pw_sn ? 1 : 0) : pw_sn); i++) {
            const PwSec& x = pw_s[(pw_sh + 59 - i) % 60];
            if (now - x.t < (sec ? 3000UL : 60000UL)) {
                pw_add_sec(&a, &x);
                if (x.gap > win) {
                    a.missed -= x.missed;  // fw92: those happened over the gap, before this window
                    a.smissed -= x.smissed;
                    if (x.hasg) {  // fw95: the same for the report pauses
                        for (uint8_t k = 0; k < GP_B; k++) {
                            a.gp[k] -= x.gp[k];
                        }
                        a.gms -= x.gms;
                        a.gfr -= x.gfr;
                    }
                }
            }
        }
    } else if ((w == "h") || (w == "d")) {
        bool h = (w == "h");
        PwAgg* r = h ? pw_f : pw_h;
        uint8_t len = h ? 12 : 24;
        uint32_t L = h ? PW_FIVE : PW_HOUR, cur = now / L + 1;
        for (uint8_t i = 0; i < len; i++) {
            if (r[i].bin && (r[i].bin <= cur) && (cur - r[i].bin < len)) {
                pw_merge(&a, &r[i]);
            }
        }
        win = ((len - 1) * L + now % L) / 1000;
    } else {
        return ok_json(false, "w=s, m, h or d");
    }
    PwAggC c;
    memset(&c, 0, sizeof(c));
    if (k == "i") {
        c = a.c[0];
    } else if (k == "m") {
        c = a.c[1];
    } else {
        pwc_merge(&c, &a.c[0]);
        pwc_merge(&c, &a.c[1]);
    }
    String j;
    j.reserve(2800);
    j = "{\"sup\":true,\"down\":" + String(down ? "true" : "false") + ",\"nodpi\":" + String(c.nodpi) + ",\"n\":" + String(a.n) + ",\"win_s\":" + String(win) + ",\"frames\":" + String(c.frames) +
        ",\"frames_all\":" + String(a.c[0].frames + a.c[1].frames) + ",\"missed\":" + String(a.missed) + ",\"settle_missed\":" + String(a.smissed) + ",\"settle_total\":" + String(pw_stot);
    // fw95 (Feather v63): the mouse's report pauses in the window, whatever k (they happen mid-movement): s = seconds with
    // block 8, n = by length (3-4, 5-9, 10-19, 20-49, 50+ ms), ms = paused ms of the 3-49 ms ones, long = the longest of
    // those, frames = the frames the check covered (moving, DPI known, not settling), tot = all since the Feather started
    j += ",\"gaps\":{\"sup\":" + String((st_ok && (st[56] >= 66)) ? "true" : "false") + ",\"s\":" + String(a.gs) + ",\"n\":[";
    for (uint8_t k = 0; k < GP_B; k++) {
        j += String(k ? "," : "") + String(a.gp[k]);
    }
    j += "],\"ms\":" + String(a.gms) + ",\"long\":" + String(a.glong) + ",\"frames\":" + String(a.gfr) + ",\"jit_max\":" + String(a.jmax) + ",\"jit_n50\":" + String(a.jn) + ",\"tot\":[";
    for (uint8_t k = 0; k < GP_B; k++) {
        j += String(k ? "," : "") + String(pw_gtot[k]);
    }
    j += "]}";
    if (!c.frames) {
        server.send(200, "application/json", j + ",\"perf\":null,\"prof\":null}");
        return;
    }
    static const char* const NAME[] = { "wait from tick", "idle stage + chords", "direction gains", "turn", "speed curve", "turn count",
                                        "tap/sticky/layers", "registers + gw_tick", "macro queue", "mappings", "macros, accumulate",
                                        "reports", "out reports", "tick: wake, events, chords, DPI", "tick: mouse settings",
                                        "tick: idle stage", "tick: link frame + update", "tick: status (every 20 ms)", "tick: status host part + hand-off",
                                        "tick: live/hist blocks + diag", "loop: USB host task", "loop: report parsing", "loop: G-Wolves hook",
                                        "loop: wait for the pass before" };
    float f = c.frames, n = a.irq_s ? a.irq_s : 1;  // fw92: the link interrupt over the seconds that have it
    j += ",\"perf\":{\"avg_us\":" + String(c.ft / f, 1) + ",\"max_us\":" + String(c.ftmx) + ",\"chords_avg_us\":" + String(c.fc / f, 1) +
         ",\"chords_max_us\":" + String(c.fcmx) + ",\"missed\":" + String(a.missed) + ",\"missed_total\":" + String(pw_mtot) +
         ",\"tick_avg_us\":" + String(c.td / f, 0) + ",\"tick_max_us\":" + String(c.tdmx) + ",\"usb_avg_us\":" + String(c.sof / f, 0) +
         ",\"usb_max_us\":" + String(c.sofmx) + "}";
    j += ",\"prof\":{\"age_ms\":0,\"frames\":" + String(c.frames) + ",\"irq_us\":" + String(a.irq_us / n, 0) + ",\"irq_n\":" + String(a.irq_n / n, 0);
    if (pf_have) {
        j += ",\"heap_used\":" + String((uint32_t) (pf_blk[11] | pf_blk[12] << 8) * 16) + ",\"heap_arena\":" + String((uint32_t) (pf_blk[13] | pf_blk[14] << 8) * 16);
    }
    j += ",\"sec\":[";
    for (uint8_t i = 0; i < PW_NS; i++) {
        j += String(i ? "," : "") + "[\"" + NAME[i] + "\"," + String(c.s[i] / f / 10.0f, 1) + "," + String(c.smx[i] * 4) + "," + String(c.swf[i] * 4) + "]";
    }
    j += "]}}";
    server.send(200, "application/json", j);
}
static String hex32(uint32_t v) {
    char s[11];
    snprintf(s, sizeof(s), "0x%08lx", (unsigned long) v);
    return String(s);
}

static String fz_json() {
    String j = ",\"fz\":{\"flag\":" + String(fz_flag ? "true" : "false") + ",\"reads\":" + String(fz_reads) + ",\"fails\":" + String(fz_fails) +
               ",\"total\":" + String(fz.total);
    if (fz_have) {
        static const char* const STEP[] = { "start", "USB host task (the mouse)", "tick (mapping, gw_tick)", "USB device task (the XIM)",
                                            "reports out", "config save", "link frame", "update: erase", "update: page", "update: record",
                                            "update: CRC", "update: restart", "chord table save" };
        const uint8_t* b = fz.blk;
        bool here = fz_ms && (fz_boot == (uint16_t) b_boots);
        j += ",\"last\":{\"t\":" + String(fz.t) + ",\"ago_s\":" + String(here ? (long) ((millis() - fz_ms) / 1000) : -1L) +
             ",\"since_power_on\":" + String(b[2]) + ",\"step\":" + String(b[4]) + ",\"step_name\":\"" + String((b[4] < 13) ? STEP[b[4]] : "?") +
             "\",\"cmd\":" + String(b[5]) + ",\"ota\":" + String(b[6]) + ",\"off\":" + String(le32(b + 10)) + ",\"caught\":\"" +
             String((b[7] == 1) ? "still" : (b[7] == 2) ? "fault" : "none") + "\",\"exc\":" + String(b[8]) + ",\"times\":" + String(b[9]) +
             ",\"pc\":\"" + hex32(le32(b + 14)) + "\",\"lr\":\"" + hex32(le32(b + 18)) + "\",\"pc2\":\"" + hex32(le32(b + 22)) + "\"}";
    }
    return j + "}";
}

static void lv_push(uint16_t v) {
    lv_ring[lv_n % LV_RING] = v;
    lv_n++;
}

static void live_tick() {
    bool want = lv_want_at && (millis() - lv_want_at < 3000);
    if (!want || !link_up || !live_supported()) {
        if (lv_have) {
            lv_have = false;  // the next start is a break in the trace
            lv_push(LV_GAP);
        }
        return;
    }
    if (cq_n > 0) {
        return;  // a command waits for its ack in the status
    }
    if (millis() - lv_read_at >= 50) {
        lv_read_at = millis();
        uint8_t b[LV_LEN];
        uint8_t len = (st[56] >= 33) ? 80 : 48, held = (len - 16) / 2;  // samples a block holds: 32 (v33) or 16
        if (read_sel(1, b, len) && (b[0] == 'L')) {
            lv_reads++;
            uint16_t raw = b[2] | b[3] << 8;
            uint16_t d = lv_have ? (uint16_t) (raw - lv_raw) : held;
            if (d > 128) {
                lv_push(LV_GAP);  // a long break, or the Feather restarted
                d = held;
            }
            for (uint16_t k = d; k > 0; k--) {  // oldest first; the block holds the newest ones
                lv_push((k <= held) ? (uint16_t) (b[8 + 2 * k] | b[9 + 2 * k] << 8) : LV_GAP);
            }
            lv_raw = raw;
            lv_have = true;
            memcpy(lv_blk, b, LV_LEN);
        } else {
            lv_fails++;
        }
    }
    if (millis() - lh_read_at >= 500) {
        lh_read_at = millis();
        uint8_t b[LH_LEN];
        if (read_sel(2, b, LH_LEN) && (b[0] == 'H') && (b[1] == 26)) {
            memcpy(lh_blk, b, LH_LEN);
            lh_have = true;
        }
    }
}

static uint32_t le32(const uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

// GET /api/live?since=N (fw50; fw51 reads Feather v33's 32-sample blocks every 50 ms): the samples after N (else the last second), the speed and gain now, the histogram;
// POST /api/live?reset=1: the histogram starts over
static void h_live() {
    touch();
    if (server.method() == HTTP_POST) {
        uint8_t z = 0;
        return ok_json((server.arg("reset") == "1") && live_supported() && queue_cmd(C_CURVE_HIST_RESET, &z, 0));
    }
    lv_want_at = millis() | 1;
    String j = "{\"sup\":" + String(live_supported() ? "true" : "false") + ",\"fw\":" + String(st_ok ? st[56] : 0);
    uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), nullptr, 10) : 0;
    uint32_t from = (since && (since <= lv_n) && (lv_n - since < LV_RING)) ? since : ((lv_n > 200) ? lv_n - 200 : 0);
    j += ",\"n\":" + String(lv_n) + ",\"from\":" + String(from) + ",\"every\":5,\"reads\":" + String(lv_reads) + ",\"fails\":" + String(lv_fails) + ",\"why\":{\"wr\":" + String(lvf_wr) + ",\"wr_code\":" + String(lvf_wr_code) + ",\"short\":" + String(lvf_short) + ",\"short_n\":" + String(lvf_short_n) + ",\"crc\":" + String(lvf_crc) + ",\"mark\":" + String(lvf_mark) + ",\"ms\":" + String(lvf_ms) + ",\"lost\":" + String(lvf_lost) + ",\"retry_ok\":" + String(lvf_retry_ok) + ",\"bad\":\"" + hex_bytes(lvf_bad, lvf_bad_len) + "\",\"prev\":\"" + hex_bytes(lvf_prev, lvf_prev_len) + "\"}";
    if (lv_have) {
        const uint8_t* b = lv_blk;
        j += ",\"on\":" + String((b[1] & 1) ? "true" : "false") + ",\"mv\":" + String((b[1] & 4) ? "true" : "false") +
             ",\"dpi\":" + String(b[4] | b[5] << 8) + ",\"g\":" + String(b[6] | b[7] << 8) + ",\"v\":" + String(b[8] | b[9] << 8);
        if (st_ok && (st[56] >= 39)) {  // fw67: the slow-down gain applying, and its gain now (per mille, part of g)
            j += ",\"dcon\":" + String((b[1] & 8) ? "true" : "false") + ",\"dcg\":" + String(b[74] | b[75] << 8);
        }
    }
    j += ",\"s\":[";
    for (uint32_t k = from; k < lv_n; k++) {
        uint16_t v = lv_ring[k % LV_RING];
        if (k > from) {
            j += ',';
        }
        j += (v == LV_GAP) ? String("-1") : String(v);
    }
    j += "]";
    if (lh_have) {
        j += ",\"h\":{\"r\":" + String(lh_blk[2]) + ",\"ms\":" + String(le32(lh_blk + 4)) + ",\"b\":[";
        for (uint8_t k = 0; k < 26; k++) {
            j += (k ? "," : "") + String(le32(lh_blk + 8 + 4 * k));
        }
        j += "]}";
    }
    server.send(200, "application/json", j + "}");
}

// POST /api/cvtemp?off=1 | keep=1 | restore=1 (fw48): BallSheet's curve-off runs, see cvt_off
static void h_cvtemp() {
    touch();
    const bool was_try = cvt_stop();  // fw81: any BallSheet curve call (its running-curve arm's restore and keep-alives too) ends the editor's live edit
    if (server.arg("ab") == "1") {
        cvt_ab_at = millis() | 1;
    }
    if (server.hasArg("restore")) {
        cvt_keep_at = millis() | 1;  // fw81: an arm switch counts as BallSheet checking in (h_cvtry waits 12 s)
        if (cvt_off || cvt_cand) {
            cvt_off = false;
            cvt_cand = false;
            cv_force = true;
            cv_sent_at = 0;
            dg_force = true;  // fw60: the running curve's gains back
            dg_sent_at = 0;
        }
        return ok_json(true);
    }
    if (server.hasArg("keep")) {
        cvt_keep_at = millis();
        const String want = server.arg("arm");  // fw63: the arm BallSheet's A/B or run expects: cand, off, or on (the active curve)
        if (want.length()) {
            if (was_try) {  // fw87: the editor's table ran instead of this arm since the last keep-alive: BallSheet drops the block
                return ok_json(false, "that arm isn't held any more (the curve editor's live edit ran in it)");
            }
            const bool held = (want == "cand") ? cvt_cand : (want == "off") ? (cvt_off && !cvt_cand) : !(cvt_off || cvt_cand);
            return held ? ok_json(true) : ok_json(false, "that arm isn't held any more (12 s without a keep-alive, or a restart)");
        }
        return ok_json(cvt_off || cvt_cand || (server.arg("ab") == "1"));  // fw59: an A/B's keep-alive in the active curve's arm is fine too
    }
    if (!from_current_page()) {
        return;
    }
    if (server.hasArg("cand")) {  // fw49: the candidate curve instead of the active one
        if (!cv_cand || !cvlib[cv_cand - 1].used) {
            return ok_json(false, lib_off ? LIB_OFF_WHY : "no candidate curve set (Speed curve card)");  // audit 11-01
        }
        cvt_cand = true;
        cvt_off = false;
        cvt_keep_at = millis();
        cv_force = true;
        cv_sent_at = 0;
        dg_force = true;  // fw60: the candidate's own gains
        dg_sent_at = 0;
        return ok_json(true);
    }
    cvt_cand = false;
    cvt_off = true;
    cvt_keep_at = millis();
    cv_force = true;
    cv_sent_at = 0;  // send now
    dg_force = true;  // fw62: from the candidate's arm its gains go back too
    dg_sent_at = 0;
    ok_json(true);
}

// POST /api/curve?sp=s0,...&g=g0,...&tau=ms&dpi=n&hosts=sig,sig&src=text: the speed curve (2-10 points, speeds in
// 0.1 cm/s rising, gains 250-4000 per mille); sp empty = remove it. hosts empty = off everywhere.
static bool csv_n(const String& s, long lo, long hi, long* out, uint8_t* n) {
    *n = 0;
    int at = 0;
    while (at < (int) s.length()) {
        int comma = s.indexOf(',', at);
        long v = ((comma < 0) ? s.substring(at) : s.substring(at, comma)).toInt();
        if ((v < lo) || (v > hi) || (*n >= CV_N)) {
            return false;
        }
        out[(*n)++] = v;
        if (comma < 0) {
            break;
        }
        at = comma + 1;
    }
    return true;
}

static String cvlib_clean(const String& s) {
    String o;
    for (unsigned i = 0; (i < s.length()) && (o.length() < 47); i++) {
        char c = s[i];
        if ((c >= 32) && (c < 127) && (c != '"') && (c != '\\')) {  // any printable ASCII but the two that would break the status JSON
            o += c;
        }
    }
    return o;
}

// audit 11-01: cvlib_save in two halves, so a curve switch can do the slow file rewrite first and then write the running
// table (NVS "curve") and its id back to back
static bool cvlib_lib_save() {
    if (lib_off) {
        return false;
    }
    return lib_fs_on() ? lib_write() : slots_save("cvl", cvlib, cvlib_nv, sizeof(CurveLib), cvlib_used, "cvlib", &cvlib_blob);  // fw71: the file
}
static bool cvlib_ids_save() {
    bool ok = true;
    if (lib_off) {
        return false;  // the saved ids stay (they name curves in the file that didn't load)
    }
    if ((prefs.getUChar("cvid", 0) != cv_id) && !prefs.putUChar("cvid", cv_id)) {
        ok = false;
        d_nvs_fail++;
        d_nvs_fail_key = "cvid";
    }
    if ((prefs.getUChar("cvcand", 0) != cv_cand) && !prefs.putUChar("cvcand", cv_cand)) {
        ok = false;
        d_nvs_fail++;
        d_nvs_fail_key = "cvcand";
    }
    return ok;
}
static bool cvlib_save() {  // fw65: false = something wasn't saved (it's gone at the next power-off)
    bool ok = cvlib_lib_save();
    return cvlib_ids_save() && ok;
}

static uint16_t cv_src_s10() {  // the "S=" in the active curve's source, x10
    int at = cv_src.indexOf(";S=");
    return (at < 0) ? 0 : (uint16_t) lround(cv_src.substring(at + 3).toFloat() * 10);
}

// audit 11-01: cvlib_add would take this table (a curve with the same points and slow-down gain, or a free slot)
static bool cvlib_fits(uint8_t n, const uint16_t* sp, const uint16_t* g, uint8_t dn, const uint16_t* dp, const uint16_t* dg) {
    for (int k = 0; k < CVLIB_N; k++) {
        const CurveLib& c = cvlib[k];
        const CurveDecel& d = cvdc[k];
        bool same_dc = (d.n == dn) && (!dn || (!memcmp(d.p10, dp, 2 * dn) && !memcmp(d.g, dg, 2 * dn)));
        if (!c.used || ((c.n == n) && !memcmp(c.sp, sp, 2 * n) && !memcmp(c.g, g, 2 * n) && same_dc)) {
            return true;
        }
    }
    return false;
}

// the library entry with this table (same points), registered if new; 0 = the library is full
// fw67: dn points of slow-down gain (dp shares, fw76: 0.1 %; dg gains; dn 0 = none) are part of what makes a curve the same
static uint8_t cvlib_add(const String& name, uint8_t n, uint8_t tau, uint16_t s10, const uint16_t* sp, const uint16_t* g, uint8_t dn, const uint16_t* dp, const uint16_t* dg) {
    int fr = -1;
    for (int k = 0; k < CVLIB_N; k++) {
        CurveLib& c = cvlib[k];
        const CurveDecel& d = cvdc[k];
        bool same_dc = (d.n == dn) && (!dn || (!memcmp(d.p10, dp, 2 * dn) && !memcmp(d.g, dg, 2 * dn)));
        if (c.used && (c.n == n) && !memcmp(c.sp, sp, 2 * n) && !memcmp(c.g, g, 2 * n) && same_dc) {
            if (name.length()) {
                memset(c.name, 0, sizeof(c.name));
                strncpy(c.name, name.c_str(), sizeof(c.name) - 1);
            }
            if (s10) {
                c.s10 = s10;
            }
            c.tau = tau;
            cvmeta_touch(k + 1);
            return k + 1;
        }
        if (!c.used && (fr < 0)) {
            fr = k;
        }
    }
    if (fr < 0) {
        return 0;
    }
    CurveLib& c = cvlib[fr];
    memset(&c, 0, sizeof(c));
    c.used = 1;
    String nm = name.length() ? name : String("curve ") + String(fr + 1);
    strncpy(c.name, nm.c_str(), sizeof(c.name) - 1);
    c.n = n;
    c.tau = tau;
    c.s10 = s10;
    memcpy(c.sp, sp, 2 * n);
    memcpy(c.g, g, 2 * n);
    CurveDecel& d = cvdc[fr];  // fw67
    memset(&d, 0, sizeof(d));
    d.n = dn;
    if (dn) {
        memcpy(d.p10, dp, 2 * dn);
        memcpy(d.g, dg, 2 * dn);
    }
    cvdc_save();
    uint8_t last = 0;  // fw61: added now, at the end of the custom order
    for (int k = 0; k < CVLIB_N; k++) {
        if (cvlib[k].used && (k != fr) && (cvmeta[k].ord > last)) {
            last = cvmeta[k].ord;
        }
    }
    if (last >= 200) {  // fw62: renumber 1..n in the same order before it can wrap
        last = 0;
        for (uint8_t want = 1; want; want++) {
            for (int k = 0; k < CVLIB_N; k++) {
                if (cvlib[k].used && (k != fr) && (cvmeta[k].ord == want)) {
                    cvmeta[k].ord = ++last;
                }
            }
            if (want == 255) {
                break;
            }
        }
    }
    cvmeta[fr].added = cvmeta[fr].used = now_unix();
    cvmeta[fr].ord = last + 1;
    cvmeta_save();
    return fr + 1;
}

static String dc_share(uint16_t p10) {  // fw76: a share in 0.1 % as JSON's %: 200 -> 20, 25 -> 2.5
    String s = String(p10 / 10);
    if (p10 % 10) {
        s += ".";
        s += String(p10 % 10);
    }
    return s;
}

static String dc_json(uint8_t id) {  // fw67: a library curve's slow-down gain as [[share %, gain per mille], ...]; "" = none
    if (!id || (id > CVLIB_N) || !cvdc[id - 1].n) {
        return String();
    }
    const CurveDecel& d = cvdc[id - 1];
    String j = "[";
    for (uint8_t k = 0; (k < d.n) && (k < DC_N); k++) {
        j += (k ? ",[" : "[") + dc_share(d.p10[k]) + "," + String(d.g[k]) + "]";  // fw76: shares with one decimal when they have one
    }
    return j + "]";
}

static String cvlib_json_one(uint8_t id) {
    const CurveLib& c = cvlib[id - 1];
    String j = "{\"id\":" + String(id) + ",\"name\":\"" + String(c.name) + "\",\"n\":" + String(c.n) + ",\"tau\":" + String(c.tau) + ",\"s10\":" + String(c.s10) +
               ",\"added\":" + String(cvmeta[id - 1].added) + ",\"used\":" + String(cvmeta[id - 1].used) + ",\"ord\":" + String(cvmeta[id - 1].ord) + ",\"sp\":[";  // fw61
    for (uint8_t k = 0; k < c.n; k++) {
        j += (k ? "," : "") + String(c.sp[k]);
    }
    j += "],\"g\":[";
    for (uint8_t k = 0; k < c.n; k++) {
        j += (k ? "," : "") + String(c.g[k]);
    }
    j += "]";
    if (cvdg[id - 1].has) {  // fw60: its own direction gains
        j += ",\"dg\":[";
        for (uint8_t k = 0; k < 8; k++) {
            j += (k ? "," : "") + String(cvdg[id - 1].g[k]);
        }
        j += "]";
    }
    String dc = dc_json(id);  // fw67
    if (dc.length()) {
        j += ",\"dc\":" + dc;
    }
    return j + "}";
}

// GET /api/curves: the library; POST ?use=id (make it the active curve) | ?cand=id (0 = none) | ?del=id | ?id=&name= (fw49)
// | ?add=1&name=&sp=&g=&tau=&s10=[&setcand=1]: a curve from the page's editor into the library, not running (answers its id)
// | ?id=&s10= (fw54): its linear cm/360 x10
// fw76: a slow-down share from a request, "20" or "2.5" (%, one decimal at most), in 0.1 %; -1 = not written that way
static long dc_p10_arg(String s) {
    s.trim();
    long v = 0;
    int dot = -1;
    for (int i = 0; i < (int) s.length(); i++) {
        char c = s[i];
        if ((c == '.') && (dot < 0) && i) {
            dot = i;
            continue;
        }
        if (!isdigit((unsigned char) c) || (v > 9999)) {
            return -1;
        }
        v = v * 10 + (c - '0');
    }
    if (!s.length() || ((dot >= 0) && (dot != (int) s.length() - 2))) {
        return -1;  // empty, "2." or more than one decimal
    }
    return (dot < 0) ? v * 10 : v;
}

// fw86: what each curve Replace changed, before and after (an edit log entry has 15 characters: "R<id>:<what>"). The last
// EDIT_LOG of them, newest first, in /cvhist.bin (RAM only without the file system); GET /api/curvehist
#define CVH_POINTS 1  // what: the curve's points
#define CVH_DECEL 2   // its slow-down gain
#define CVH_TAU 4     // its smoothing
#define CVH_S10 8     // the XIM linear cm/360 it goes with
struct __attribute__((packed)) CvHist {
    uint32_t t;  // as the edit log entry's (now_unix)
    uint8_t id, what;
    uint8_t n0, n1, tau0, tau1;
    uint16_t s10_0, s10_1;
    uint16_t sp0[CV_N], g0[CV_N], sp1[CV_N], g1[CV_N];
    CurveDecel d0, d1;
};
static_assert(sizeof(CvHist) == 176, "/cvhist.bin's layout");  // fw98: 10-point slow-down gains (fw86-97: 144, read by cvhist_read)
#define CVH_OLD_SIZE 144
#define CVH_HEAD (sizeof(CvHist) - 2 * sizeof(CurveDecel))  // the part before d0, the same in both layouts
static_assert(CVH_HEAD + 2 * DC_V2_SIZE == CVH_OLD_SIZE, "fw86-97's record: the head, then two 6-point slow-down gains");
static CvHist cvhist[EDIT_LOG];
static CvHist oq_c;  // fw107: the record on its way to /cvhist_old.bin (oq_tick)
static bool oq_con = false, cvh_dirty = false;
static uint8_t cvhist_n = 0;

static bool cvhist_read(const char* path) {  // fw87: true if the file holds a whole log (cvhist_n left as it was if not)
    if (!FFat.exists(path)) {
        return false;
    }
    File f = FFat.open(path, "r");
    if (!f) {
        return false;
    }
    uint8_t n = 0;
    bool ok = (f.read(&n, 1) == 1) && (n <= EDIT_LOG);
    size_t rest = ok ? f.size() - 1 : 0;
    if (ok && n && (rest == (size_t) CVH_OLD_SIZE * n) && (rest != sizeof(CvHist) * n)) {
        // fw98: fw86-97's records (6-point slow-down gains): each read and widened, the 4 new points empty
        static uint8_t o[CVH_OLD_SIZE];
        for (uint8_t k = 0; ok && (k < n); k++) {
            ok = f.read(o, CVH_OLD_SIZE) == CVH_OLD_SIZE;
            if (ok) {
                memcpy(&cvhist[k], o, CVH_HEAD);
                cvdc_from_old(&cvhist[k].d0, o + CVH_HEAD);
                cvdc_from_old(&cvhist[k].d1, o + CVH_HEAD + DC_V2_SIZE);
            }
        }
    } else {
        ok = ok && (f.read((uint8_t*) cvhist, sizeof(CvHist) * n) == sizeof(CvHist) * n);
    }
    f.close();
    if (ok) {
        cvhist_n = n;
    }
    return ok;
}

static void cvhist_load() {
    if (!fs_ok) {
        return;
    }
    if (!cvhist_read("/cvhist.bin")) {
        cvhist_read("/cvhist.tmp");  // fw87: a save cut off between its remove and rename left the new log here
    }
}

// fw87: a whole new /cvhist.tmp first, then it replaces /cvhist.bin (a power cut mid-write no longer empties the log)
static void cvhist_save() {
    if (!fs_ok) {
        return;
    }
    File f = FFat.open("/cvhist.tmp", "w");
    if (!f) {
        return;
    }
    const size_t len = sizeof(CvHist) * cvhist_n;
    bool ok = (f.write(&cvhist_n, 1) == 1) && (f.write((const uint8_t*) cvhist, len) == len);
    f.close();
    if (!ok) {
        FFat.remove("/cvhist.tmp");  // a full disk: the old log stays
        return;
    }
    FFat.remove("/cvhist.bin");
    FFat.rename("/cvhist.tmp", "/cvhist.bin");
}

static void oq_tick() {  // fw107
    if (oq_con) {
        oq_con = false;
        old_push(CVH_OLD_BIN, &oq_c, sizeof(CvHist), &cvh_old_n);
    } else if (cvh_dirty) {
        cvh_dirty = false;
        cvhist_save();
    } else if (oq_en) {
        oq_edit_one();
    }
}

// curve id's old version (cvh_was, cvh_dwas: set before the replace; no struct parameters, as the sketch's generated
// prototypes come before the structs) against its new one in the library; returns what changed (CVH_*)
static CurveLib cvh_was;
static CurveDecel cvh_dwas;
static uint8_t cvhist_add(uint8_t id) {
    const CurveLib& o = cvh_was;
    const CurveDecel& od = cvh_dwas;
    const CurveLib& c = cvlib[id - 1];
    const CurveDecel& d = cvdc[id - 1];
    uint8_t what = 0;
    if ((o.n != c.n) || memcmp(o.sp, c.sp, 2 * c.n) || memcmp(o.g, c.g, 2 * c.n)) {
        what |= CVH_POINTS;
    }
    if ((od.n != d.n) || memcmp(od.p10, d.p10, 2 * d.n) || memcmp(od.g, d.g, 2 * d.n)) {
        what |= CVH_DECEL;
    }
    what |= (o.tau != c.tau) ? CVH_TAU : 0;
    what |= (o.s10 != c.s10) ? CVH_S10 : 0;
    if (cvhist_n == EDIT_LOG) {  // fw103; fw107: written by oq_tick
        if (oq_con) {
            old_push(CVH_OLD_BIN, &oq_c, sizeof(CvHist), &cvh_old_n);
        }
        oq_c = cvhist[EDIT_LOG - 1];
        oq_con = true;
    }
    memmove(cvhist + 1, cvhist, sizeof(CvHist) * (EDIT_LOG - 1));
    CvHist& h = cvhist[0];
    memset(&h, 0, sizeof(h));
    h.t = now_unix();
    h.id = id;
    h.what = what;
    h.n0 = o.n;
    h.n1 = c.n;
    h.tau0 = o.tau;
    h.tau1 = c.tau;
    h.s10_0 = o.s10;
    h.s10_1 = c.s10;
    memcpy(h.sp0, o.sp, sizeof(h.sp0));
    memcpy(h.g0, o.g, sizeof(h.g0));
    memcpy(h.sp1, c.sp, sizeof(h.sp1));
    memcpy(h.g1, c.g, sizeof(h.g1));
    h.d0 = od;
    h.d1 = d;
    if (cvhist_n < EDIT_LOG) {
        cvhist_n++;
    }
    cvh_dirty = true;  // fw107: oq_tick
    return what;
}

static String cvhist_pts(const uint16_t* x, const uint16_t* y, uint8_t n) {
    String j = "[";
    for (uint8_t k = 0; k < n; k++) {
        j += String(k ? ",[" : "[") + String(x[k]) + "," + String(y[k]) + "]";
    }
    return j + "]";
}

static String cvhist_json(const void* rec) {  // a void pointer: the generated prototypes come before the struct
    const CvHist& h = *(const CvHist*) rec;
    uint8_t n0 = (h.n0 > CV_N) ? CV_N : h.n0, n1 = (h.n1 > CV_N) ? CV_N : h.n1, e0 = (h.d0.n > DC_N) ? DC_N : h.d0.n, e1 = (h.d1.n > DC_N) ? DC_N : h.d1.n;
    return String("{\"t\":") + String(h.t) + ",\"id\":" + String(h.id) + ",\"what\":" + String(h.what) + ",\"tau\":[" + String(h.tau0) + "," +
           String(h.tau1) + "],\"s10\":[" + String(h.s10_0) + "," + String(h.s10_1) + "],\"c0\":" + cvhist_pts(h.sp0, h.g0, n0) + ",\"c1\":" +
           cvhist_pts(h.sp1, h.g1, n1) + ",\"d0\":" + cvhist_pts(h.d0.p10, h.d0.g, e0) + ",\"d1\":" + cvhist_pts(h.d1.p10, h.d1.g, e1) + "}";
}

static void h_curvehist() {
    if (server.hasArg("old")) {  // fw103: from the file, a record at a time (80 of them are about 32 KB as text)
        server.setContentLength(CONTENT_LENGTH_UNKNOWN);
        server.send(200, "application/json", "");
        server.sendContent("[");
        File f = (fs_ok && FFat.exists(CVH_OLD_BIN)) ? FFat.open(CVH_OLD_BIN, "r") : File();
        if (f) {
            static CvHist h;
            bool first = true;
            while (f.read((uint8_t*) &h, sizeof(h)) == sizeof(h)) {
                server.sendContent(String(first ? "" : ",") + cvhist_json(&h));
                first = false;
            }
            f.close();
        }
        server.sendContent("]");
        server.sendContent("");
        return;
    }
    String j = "[";
    for (uint8_t i = 0; i < cvhist_n; i++) {
        j += String(i ? "," : "") + cvhist_json(&cvhist[i]);
    }
    server.send(200, "application/json", j + "]");
}

static void h_editsold() {  // fw103: as the status's "edits", from the file
    String j = "[";
    File f = (fs_ok && FFat.exists(EDIT_OLD_BIN)) ? FFat.open(EDIT_OLD_BIN, "r") : File();
    if (f) {
        EditLog e;
        bool first = true;
        while (f.read((uint8_t*) &e, sizeof(e)) == sizeof(e)) {
            e.text[sizeof(e.text) - 1] = 0;
            j += String(first ? "[" : ",[") + String(e.t) + ",-1," + String(e.kind) + "," + String(e.dev) + ",\"" + IPAddress(e.ip).toString() + "\",\"" + String(e.text) + "\"]";
            first = false;
        }
        f.close();
    }
    server.send(200, "application/json", j + "]");
}

// fw105: what block 8 says of the USB-A port; a restart the Feather did by itself is logged here (the button's when it's pressed)
static void usb_note(const uint8_t* b) {
    uint8_t r = b[42];
    if ((usb_seen >= 0) && (r > usb_seen)) {
        uint8_t n = r - usb_seen;
        if (usb_btn && (millis() - usb_btn_at < 10000)) {  // fw106: logged here, so only when it really happened
            log_edit_as(EK_USB_RESET, "button", usb_btn_dev, usb_btn_ip);
            n--;
        }
        usb_btn = 0;
        while (n--) {
            log_edit_as(EK_USB_RESET, "auto", 7, 0);
        }
    }
    usb_seen = r;
    usb_flags = b[43];
    usb_worst = b[44] | b[45] << 8;
    usb_mounts = b[46];
    usb_at = millis();
    usb_ok = true;
}

static String dcmin_txt(uint16_t v) {  // fw110: 20 or 17.5
    return (v % 10) ? String(v / 10.0f, 1) : String(v / 10);
}

// fw109: POST /api/dcmin?v=<cm/s>: the slow-down gain's flick threshold, 0-100 (0 = every stroke), for all curves
// fw110: logged as "dcmin=<new>/<old>"; changes from the same sender within 60 s of it update that entry (the number box's
// arrows send one per click), and one that comes back to where it started removes it
static void h_dcmin() {
    touch();
    if (!from_current_page()) {
        return;
    }
    float v = server.arg("v").toFloat();
    if (!server.hasArg("v") || !(v >= 0) || (v > 100)) {
        return ok_json(false, "a speed from 0 to 100 cm/s");
    }
    uint16_t x = (uint16_t) lroundf(v * 10);
    if (x != dc_min10) {
        uint16_t from = dc_min10;
        dc_min10 = x;
        prefs.putUShort("dcmin", x);
        dc_force = true;
        cv_sent_at = 0;
        EditLog& e = edit_log[0];
        uint32_t ip = (uint32_t) server.client().remoteIP();
        const char* sl = strchr(e.text, '/');
        if (edit_n && (e.kind == EK_OPT) && (e.boot == (uint16_t) b_boots) && (e.ip == ip) && (millis() - e.ms < 60000) && !strncmp(e.text, "dcmin=", 6) && sl) {
            uint16_t f0 = (uint16_t) lroundf(atof(sl + 1) * 10);
            if (f0 == x) {  // back where it started: as if nothing changed
                edit_n--;
                memmove(edit_log, edit_log + 1, sizeof(EditLog) * edit_n);
                memmove(edit_seq, edit_seq + 1, sizeof(edit_seq[0]) * edit_n);
                memset(&edit_log[edit_n], 0, sizeof(EditLog));
                edit_seq[edit_n] = 0;
            } else {
                snprintf(e.text, sizeof(e.text), "dcmin=%s/%s", dcmin_txt(x).c_str(), dcmin_txt(f0).c_str());
                e.t = now_unix();
                e.ms = millis();
            }
            nvs_put("edits", edit_log, sizeof(EditLog) * edit_n);
        } else {
            log_edit(EK_OPT, "dcmin=" + dcmin_txt(x) + "/" + dcmin_txt(from));
        }
    }
    ok_json(true);
}

// POST /api/usbreset: the Feather's USB-A port off and on (Feather v70), as replugging what's in it
static void h_usbreset() {
    touch();
    if (!from_current_page()) {
        return;
    }
    if (!st_ok || !link_up || (st[56] < 70)) {
        return ok_json(false, "this needs Feather firmware v70");
    }
    bool ok = queue_cmd(C_USB_RESET, nullptr, 0);
    if (ok) {
        usb_btn = 1;
        usb_btn_at = millis();
        usb_btn_dev = ua_device();
        usb_btn_ip = (uint32_t) server.client().remoteIP();
    }
    ok_json(ok);
}

// fw112: POST /api/diagreset: the frame-timing numbers start over. The QT Py's minute / hour / 24 h windows are emptied, and
// Feather v74+ is told to start its since-start counts over (frames skipped, report pauses, USB-A restarts and slowest send).
// The last-read totals stay as the base: the Feather's next, smaller counts are taken as all new (as after its restart).
// {"ok":true,"feather":false} = only the windows (an older Feather, or no answer from it)
static void h_diagreset() {
    touch();
    if (!from_current_page()) {
        return;
    }
    memset(pw_s, 0, sizeof(pw_s));
    pw_sh = pw_sn = 0;
    memset(pw_f, 0, sizeof(pw_f));
    memset(pw_h, 0, sizeof(pw_h));
    bool fe = st_ok && link_up && (st[56] >= 74) && queue_cmd(C_DIAG_RESET, nullptr, 0);
    drst.t = now_unix();
    drst.up = pw_mtot_ok ? pw_up : 0;
    drst.on = fe ? 1 : 0;
    drst_ms = millis() | 1;
    nvs_put("drst", &drst, sizeof(drst));
    server.send(200, "application/json", String("{\"ok\":true,\"feather\":") + (fe ? "true" : "false") + "}");
}

// fw104: a star and a short note on a saved curve version: the save's time and curve id are the key (an entry of the curve
// history, its After column). A file of at most CVN_MAX, as many as the history holds; both empty removes the entry
#define CVN_MAX 100
#define CVN_BIN "/cvnotes.bin"
struct __attribute__((packed)) CvNote {
    uint32_t t;
    uint8_t id, star;
    char note[42];
};
static_assert(sizeof(CvNote) == 48, "/cvnotes.bin's layout");

static void h_cvnotes() {
    String j = "[";
    File f = (fs_ok && FFat.exists(CVN_BIN)) ? FFat.open(CVN_BIN, "r") : File();
    if (f) {
        CvNote c;
        bool first = true;
        while (f.read((uint8_t*) &c, sizeof(c)) == sizeof(c)) {
            c.note[sizeof(c.note) - 1] = 0;
            j += String(first ? "[" : ",[") + String(c.t) + "," + String(c.id) + "," + String(c.star) + ",\"" + String(c.note) + "\"]";
            first = false;
        }
        f.close();
    }
    server.send(200, "application/json", j + "]");
}

// POST ?at=<the save's time>&id=&star=0|1&note= ("t" is the page's clock, as on every POST)
static void h_cvnote() {
    touch();
    if (!from_current_page()) {
        return;
    }
    uint32_t at = strtoul(server.arg("at").c_str(), nullptr, 10);
    long id = server.arg("id").toInt();
    if (!fs_ok || !at || (id < 1) || (id > CVLIB_N)) {
        return ok_json(false, "no such curve save");
    }
    uint8_t star = server.arg("star").toInt() ? 1 : 0;
    String note = cvlib_clean(server.arg("note")).substring(0, sizeof(CvNote::note) - 1);
    CvNote* a = (CvNote*) malloc(sizeof(CvNote) * CVN_MAX);
    if (!a) {
        return ok_json(false, "out of memory");
    }
    size_t n = 0;
    File f = FFat.exists(CVN_BIN) ? FFat.open(CVN_BIN, "r") : File();
    if (f) {
        n = f.read((uint8_t*) a, sizeof(CvNote) * CVN_MAX) / sizeof(CvNote);
        f.close();
    }
    size_t k = 0;
    while ((k < n) && !((a[k].t == at) && (a[k].id == id))) {
        k++;
    }
    if (!star && !note.length()) {
        if (k < n) {
            memmove(a + k, a + k + 1, sizeof(CvNote) * (n - k - 1));
            n--;
        }
    } else {
        if (k == n) {
            if (n == CVN_MAX) {  // full: the oldest save's goes
                k = 0;
                for (size_t i = 1; i < n; i++) {
                    if (a[i].t < a[k].t) {
                        k = i;
                    }
                }
            } else {
                n++;
            }
        }
        memset(&a[k], 0, sizeof(CvNote));
        a[k].t = at;
        a[k].id = (uint8_t) id;
        a[k].star = star;
        strncpy(a[k].note, note.c_str(), sizeof(a[k].note) - 1);
    }
    File w = FFat.open(CVN_BIN ".tmp", "w");
    bool ok = w && (w.write((const uint8_t*) a, sizeof(CvNote) * n) == sizeof(CvNote) * n);
    if (w) {
        w.close();
    }
    free(a);
    if (!ok) {
        FFat.remove(CVN_BIN ".tmp");
        return ok_json(false, "couldn't save the note");
    }
    FFat.remove(CVN_BIN);
    FFat.rename(CVN_BIN ".tmp", CVN_BIN);
    ok_json(true);
}

// fw67: a curve's table from a request (sp, g, tau, s10, dc), as add=1 and replace=1 take it; nullptr = fine, else why not
static const char* cv_req_table(uint8_t* n, uint16_t* s16, uint16_t* g16, uint8_t* tau, uint16_t* s10, uint8_t* dn, uint16_t* dp, uint16_t* dgv) {
    long sp[CV_N], g[CV_N];
    uint8_t ng = 0;
    *n = 0;
    if (!csv_n(server.arg("sp"), 1, 65535, sp, n) || !csv_n(server.arg("g"), 250, 4000, g, &ng) || (*n != ng) || (*n < 2)) {
        return "need 2-10 speeds (0.1 cm/s) and as many gains (250-4000 per mille)";
    }
    for (uint8_t k = 0; k < *n; k++) {
        if (k && (sp[k] <= sp[k - 1])) {
            return "speeds must rise";
        }
        s16[k] = sp[k];
        g16[k] = g[k];
    }
    long t = server.hasArg("tau") ? server.arg("tau").toInt() : 8, s = server.arg("s10").toInt();
    *tau = (uint8_t) std::max(1L, std::min(200L, t));
    *s10 = (uint16_t) std::max(0L, std::min(65535L, s));
    *dn = 0;  // &dc=share:gain,... (shares % 1-99 in 0.1 % steps, rising, e.g. 2.5:525; gains per mille), none = no slow-down gain
    String ds = server.arg("dc");
    for (int at = 0; at < (int) ds.length();) {
        int comma = ds.indexOf(',', at);
        String one = (comma < 0) ? ds.substring(at) : ds.substring(at, comma);
        int colon = one.indexOf(':');
        long p = (colon > 0) ? dc_p10_arg(one.substring(0, colon)) : -1, gv = (colon > 0) ? one.substring(colon + 1).toInt() : 0;  // fw76: p in 0.1 %
        if ((*dn >= (st_ok ? DC_FEATHER_N(st[56]) : DC_OLD_N)) || (p < 10) || (p > 990) || (*dn && (p <= dp[*dn - 1])) || (gv < 250) || (gv > 4000)) {
            return ((*dn >= DC_OLD_N) && (*dn < DC_N) && (!st_ok || (st[56] < 68))) ? (st_ok ? "slow-down gain: more than 6 points needs Feather firmware v68" : "slow-down gain: more than 6 points needs the Feather answering (firmware v68): try again in a moment") : "slow-down gain: 1-10 points share:gain, shares 1-99 % in 0.1 % steps and rising, gains 250-4000 per mille";
        }
        dp[*dn] = (uint16_t) p;
        dgv[(*dn)++] = (uint16_t) gv;
        if (comma < 0) {
            break;
        }
        at = comma + 1;
    }
    return nullptr;
}
// POST /api/cvtry (fw81): the curve editor's live edit. The table as add=1 takes it (sp, g, tau, dc) = run it now (again
// with each change); keep=1 = is it still live (ok false once a BallSheet run or a restart ended it: the page sends it again); end=1 = the running curve back
// fw87: each carries &tok= (the page load's token, cvt_tok): keep from another page = "another editor" (that page waits for
// its own user's next change, whose try takes over), end from another page changes nothing
static void h_cvtry() {
    touch();
    String tok = server.arg("tok");
    if (tok.length() >= sizeof(cvt_tok)) {
        tok = tok.substring(0, sizeof(cvt_tok) - 1);
    }
    const bool mine = cvt_try && (tok == cvt_tok);
    if (server.hasArg("keep")) {
        if (!cvt_try) {
            return ok_json(false, "not live");
        }
        if (!mine) {
            return ok_json(false, "another editor");
        }
        cvt_seen = millis();  // fw87: the lease goes on
        return ok_json(true);
    }
    if (server.hasArg("end")) {
        if (mine) {
            cvt_stop();
        }
        return ok_json(true);
    }
    if (!from_current_page()) {
        return;
    }
    if (cvt_off || cvt_cand || (cvt_keep_at && (millis() - cvt_keep_at < 12000))) {  // a BallSheet keep-alive in the last 12 s: a run is on, whatever its arm
        return ok_json(false, "BallSheet is running a curve test: it goes live after that");
    }
    if (dgt_on || (tdpi_active && !tdpi_restore)) {  // fw87: its gains or DPI test too (they end a live edit as they start)
        return ok_json(false, "BallSheet is running a test: it goes live after that");
    }
    if (!cv_supported()) {
        return ok_json(false, "the Feather isn't connected, or its firmware is too old for a curve");
    }
    uint16_t s16[CV_N] = {}, g16[CV_N] = {}, dgv[DC_N] = {}, dp[DC_N] = {}, s10 = 0;
    uint8_t n = 0, tau = 8, dn = 0;
    const char* why = cv_req_table(&n, s16, g16, &tau, &s10, &dn, dp, dgv);
    if (why) {
        return ok_json(false, why);
    }
    if (lib_off && dn) {  // the Feather's slow-down gain is never sent while the library is off (cvdc didn't load): a try's couldn't be undone
        return ok_json(false, "the slow-down gain can't run live while the curve library isn't loaded (the table alone can)");
    }
    try_n = n;
    try_tau = tau;
    memcpy(try_sp, s16, sizeof(try_sp));
    memcpy(try_g, g16, sizeof(try_g));
    memset(&try_dc, 0, sizeof(try_dc));
    try_dc.n = dn;
    memcpy(try_dc.p10, dp, 2 * dn);
    memcpy(try_dc.g, dgv, 2 * dn);
    strlcpy(cvt_tok, tok.c_str(), sizeof(cvt_tok));  // fw87: the newest editor's page owns it (a live one from another page is taken over)
    cvt_seen = millis();
    cvt_try = true;
    cv_force = true;
    dc_force = true;
    cv_sent_at = 0;
    String j = String("{\"ok\":true,\"want\":") + (cv_wanted() ? "true" : "false") + "}";  // want false: the curve is off on this host, so the try is too
    server.send(200, "application/json", j);
}
static void h_curves() {
    touch();
    if (server.method() == HTTP_GET) {
        String j = "{\"active\":" + String(cv_id) + ",\"cand\":" + String(cv_cand) + (lib_off ? ",\"off\":true" : "") + (lib_dmg ? ",\"dmg\":true" : "") + ",\"curves\":[";  // audit 11-01: off = didn't load, dmg = its files are damaged
        bool first = true;
        for (uint8_t k = 1; k <= CVLIB_N; k++) {
            if (cvlib[k - 1].used) {
                j += (first ? "" : ",") + cvlib_json_one(k);
                first = false;
            }
        }
        server.send(200, "application/json", j + "]}");
        return;
    }
    if (!from_current_page()) {
        return;
    }
    if (server.hasArg("fresh")) {  // audit 11-01: damaged library files set aside (name.bad), a new library with the running curve
        if (!lib_dmg) {
            return ok_json(false, "the curve library isn't damaged");
        }
        const char* lib_files[] = { "/curves.bin", "/curves.tmp", "/curves.bak" };
        for (const char* p : lib_files) {
            if (FFat.exists(p)) {
                String bad = String(p) + ".bad";
                FFat.remove(bad.c_str());
                FFat.rename(p, bad.c_str());
                lib_bad++;
            }
        }
        lib_off = false;
        lib_dmg = false;
        lib_fs = true;
        cv_id = 0;
        cv_cand = 0;
        if (cv.n >= 2) {
            cv_id = cvlib_add(String(""), cv.n, cv.tau, cv_src_s10(), cv.sp, cv.g, 0, nullptr, nullptr);
        }
        cvdg_stamp(cv_id);
        lib_fs = lib_write();
        if (!lib_fs) {
            lib_off = true;
            lib_dmg = true;
            return ok_json(false, "couldn't write the new library file");
        }
        cvlib_ids_save();
        return ok_json(true, "a new curve library, with the running curve");
    }
    if (lib_off) {
        return ok_json(false, LIB_OFF_WHY);  // audit 11-01
    }
    if (server.hasArg("add")) {
        uint16_t s16[CV_N] = {}, g16[CV_N] = {}, dgv[DC_N] = {}, dp[DC_N] = {}, s10 = 0;  // fw76: dp in 0.1 %
        uint8_t n = 0, tau = 8, dn = 0;
        const char* why = cv_req_table(&n, s16, g16, &tau, &s10, &dn, dp, dgv);
        if (why) {
            return ok_json(false, why);
        }
        for (int k = 0; k < CVLIB_N; k++) {  // 10-31 audit: "Save as copy" of an unchanged curve renamed the original (cvlib_add's match)
            const CurveDecel& o = cvdc[k];
            if (cvlib[k].used && (cvlib[k].n == n) && !memcmp(cvlib[k].sp, s16, 2 * n) && !memcmp(cvlib[k].g, g16, 2 * n) && (o.n == dn) &&
                (!dn || (!memcmp(o.p10, dp, 2 * dn) && !memcmp(o.g, dgv, 2 * dn)))) {
                return ok_json(false, (String("that's the same as ") + String(cvlib[k].name) + ": change something first").c_str());  // no quotes: ok_json doesn't escape
            }
        }
        if (!lib_fs_on() && (nvs_room() < NVS_KEEP + 5 + (dn ? 3 : 0))) {  // fw71: only while the library is in NVS; fw65: a curve takes 5 entries (fw67: 3 more with a slow-down gain); refused rather than lost at the next power-off
            return ok_json(false, "the QT Py's settings storage is full: delete a curve first");
        }
        uint8_t nid = cvlib_add(cvlib_clean(server.arg("name")), n, tau, s10, s16, g16, dn, dp, dgv);
        if (!nid) {
            return ok_json(false, "the library is full (16): delete a curve first");
        }
        if ((server.arg("setcand") == "1") && (nid != cv_id)) {
            cv_cand = nid;
        }
        if (!cvlib_save()) {
            return ok_json(false, "added, but the QT Py couldn't save it: it's gone at the next power-off");
        }
        server.send(200, "application/json", "{\"ok\":true,\"id\":" + String(nid) + "}");
        return;
    }
    if (server.hasArg("order")) {  // fw61: ?order=id,id,...: the page's custom order (drag and drop), first = 1
        String s = server.arg("order");
        int at = 0;
        uint8_t pos = 0;
        while ((at < (int) s.length()) && (pos < 250)) {
            int comma = s.indexOf(',', at);
            long v = ((comma < 0) ? s.substring(at) : s.substring(at, comma)).toInt();
            if ((v >= 1) && (v <= CVLIB_N) && cvlib[v - 1].used) {
                cvmeta[v - 1].ord = ++pos;
            }
            if (comma < 0) {
                break;
            }
            at = comma + 1;
        }
        cvmeta_save();
        return ok_json(true);
    }
    long id = server.arg(server.hasArg("use") ? "use" : server.hasArg("cand") ? "cand" : server.hasArg("del") ? "del" : "id").toInt();
    bool ok = (id >= 1) && (id <= CVLIB_N) && cvlib[id - 1].used;
    if (server.hasArg("cand") && (id == 0)) {
        cv_cand = 0;
        cvlib_save();
        return ok_json(true);
    }
    if (!ok) {
        return ok_json(false, "no such curve");
    }
    CurveLib& c = cvlib[id - 1];
    if (server.hasArg("use")) {
        cv.n = c.n;
        cv.tau = c.tau;
        memset(cv.sp, 0, sizeof(cv.sp));
        memset(cv.g, 0, sizeof(cv.g));
        memcpy(cv.sp, c.sp, 2 * c.n);
        memcpy(cv.g, c.g, 2 * c.n);
        cvdg_stamp(cv_id);  // fw60
        cv_id = id;
        cvmeta_touch(cv_id);  // fw61
        if (cv_cand == id) {
            cv_cand = 0;
        }
        cvlib_lib_save();  // audit 11-01: the slow /curves.bin rewrites first; the running table and its id then back to back
        cvdg_take(cv_id);
        cv_src = String("lib:") + String(c.name) + (c.s10 ? ";S=" + String(c.s10 / 10.0f, 1) : String(""));
        prefs.putString("cvsrc", cv_src);
        nvs_put("curve", &cv, sizeof(cv));
        cvlib_ids_save();
        log_edit(EK_CURVE, String("U") + String(id) + " " + String(c.name));  // fw86: the id first, so the page names it in full
        cv_sent_at = 0;
        cv_force = true;
        return ok_json(true);
    }
    if (server.hasArg("cand")) {
        cv_cand = (id == cv_id) ? 0 : id;
        cvlib_save();
        return ok_json(true);
    }
    if (server.hasArg("del")) {
        if (id == cv_id) {
            return ok_json(false, "that's the active curve: use another one first");
        }
        memset(&c, 0, sizeof(c));
        memset(&cvdg[id - 1], 0, sizeof(CurveGains));  // fw60
        cvdg_save();
        memset(&cvdc[id - 1], 0, sizeof(CurveDecel));  // fw67
        cvdc_save();
        memset(&cvmeta[id - 1], 0, sizeof(CurveMeta));  // fw61
        cvmeta_save();
        if (cv_cand == id) {
            cv_cand = 0;
        }
        cvlib_save();
        return ok_json(true);
    }
    if (server.hasArg("replace")) {  // fw67: ?id=&replace=1 and the table as add=1 takes it: written over that curve (same id, name, place)
        uint16_t s16[CV_N] = {}, g16[CV_N] = {}, dgv[DC_N] = {}, dp[DC_N] = {}, s10 = 0;  // fw76: dp in 0.1 %
        uint8_t n = 0, tau = 8, dn = 0;
        const char* why = cv_req_table(&n, s16, g16, &tau, &s10, &dn, dp, dgv);
        if (why) {
            return ok_json(false, why);
        }
        for (int k = 0; (k < CVLIB_N) && !server.hasArg("restore"); k++) {  // fw106: going back to an old version may match another curve
            const CurveDecel& o = cvdc[k];
            if ((k != id - 1) && cvlib[k].used && (cvlib[k].n == n) && !memcmp(cvlib[k].sp, s16, 2 * n) && !memcmp(cvlib[k].g, g16, 2 * n) && (o.n == dn) &&
                (!dn || (!memcmp(o.p10, dp, 2 * dn) && !memcmp(o.g, dgv, 2 * dn)))) {
                return ok_json(false, "that's the same as another saved curve");
            }
        }
        if (!lib_fs_on() && (nvs_room() < NVS_KEEP + 5 + ((dn && !cvdc[id - 1].n) ? 3 : 0))) {  // fw71: in NVS only; the curve's blob is written anew before the old goes; a new slow-down gain adds its own
            return ok_json(false, "the QT Py's settings storage is full: delete a curve first");
        }
        cvh_was = c;  // fw86: for the change history (cvhist_add)
        cvh_dwas = cvdc[id - 1];
        c.n = n;
        c.tau = tau;
        if (s10 || server.hasArg("s10z")) {  // fw106: s10z: really none (a restore), else 0 leaves it as it is
            c.s10 = s10;
        }
        memset(c.sp, 0, sizeof(c.sp));
        memset(c.g, 0, sizeof(c.g));
        memcpy(c.sp, s16, 2 * n);
        memcpy(c.g, g16, 2 * n);
        CurveDecel& d = cvdc[id - 1];
        memset(&d, 0, sizeof(d));
        d.n = dn;
        memcpy(d.p10, dp, 2 * dn);
        memcpy(d.g, dgv, 2 * dn);
        if (id == cv_id) {  // the running one: the Feather gets it now
            cv.n = n;
            cv.tau = tau;
            memset(cv.sp, 0, sizeof(cv.sp));
            memset(cv.g, 0, sizeof(cv.g));
            memcpy(cv.sp, s16, 2 * n);
            memcpy(cv.g, g16, 2 * n);
            nvs_put("curve", &cv, sizeof(cv));
            cv_src = String("lib:") + String(c.name) + (c.s10 ? ";S=" + String(c.s10 / 10.0f, 1) : String(""));
            prefs.putString("cvsrc", cv_src);
        }
        if ((id == cv_id) || (id == cv_cand)) {
            cv_force = true;
            cv_sent_at = 0;
        }
        cvmeta_touch(id);
        bool saved = lib_fs_on() ? true : cvdc_save();  // fw107: with the library in its file, cvlib_save writes the slow-down gains too (the same lib_write)
        saved = cvlib_save() && saved;
        uint8_t what = cvhist_add(id);  // fw86: the log says which curve and what changed (the page names it)
        log_edit(EK_CURVE, String("R") + String(id) + ":" + ((what & CVH_POINTS) ? "c" : "") + ((what & CVH_DECEL) ? "d" : "") + ((what & CVH_TAU) ? "t" : "") +
                               ((what & CVH_S10) ? "l" : ""));
        return saved ? ok_json(true) : ok_json(false, "replaced, but the QT Py couldn't save it: the old version comes back at the next power-off");
    }    if (server.hasArg("s10")) {  // fw54: ?id=&s10=: the XIM's linear cm/360 (x10) the curve goes with
        long v = server.arg("s10").toInt();
        if ((v < 0) || (v > 65535)) {
            return ok_json(false, "linear cm/360 out of range");
        }
        c.s10 = (uint16_t) v;
        cvmeta_touch(id);  // fw61
        if (id == cv_id) {
            cv_src = String("lib:") + String(c.name) + (c.s10 ? ";S=" + String(c.s10 / 10.0f, 1) : String(""));
            prefs.putString("cvsrc", cv_src);
        }
        cvlib_save();
        log_edit(EK_CURVE, String("L") + String(id) + " " + String(c.s10 / 10.0f, 1));  // fw86: the id, so the page names it in full
        return ok_json(true);
    }
    String nm = cvlib_clean(server.arg("name"));
    if (!nm.length()) {
        return ok_json(false, "a name, please");
    }
    memset(c.name, 0, sizeof(c.name));
    strncpy(c.name, nm.c_str(), sizeof(c.name) - 1);
    bool saved = cvlib_save();
    cvmeta_touch(id);  // fw61
    saved ? ok_json(true) : ok_json(false, "renamed, but the QT Py couldn't save it: the old name comes back at the next power-off");
}

static void h_curve() {
    touch();
    if (!from_current_page()) {
        return;
    }
    if (lib_off) {
        return ok_json(false, LIB_OFF_WHY);  // audit 11-01: the running curve's id can't follow a new table
    }
    long sp[CV_N], g[CV_N];
    uint8_t n = 0, ng = 0;
    if (!csv_n(server.arg("sp"), 1, 65535, sp, &n) || !csv_n(server.arg("g"), 250, 4000, g, &ng) || (n != ng) || (n == 1)) {
        return ok_json(false, "need 2-10 speeds (0.1 cm/s) and as many gains (250-4000 per mille)");
    }
    for (uint8_t k = 1; k < n; k++) {
        if (sp[k] <= sp[k - 1]) {
            return ok_json(false, "speeds must rise");
        }
    }
    long tau = server.hasArg("tau") ? server.arg("tau").toInt() : 8, dpi = server.arg("dpi").toInt();
    if ((tau < 1) || (tau > 200) || (dpi < 0) || (dpi > 65535) || (n && (dpi < 50))) {
        return ok_json(false, "tau 1-200 ms, dpi 50-65535 (the Feather's fallback while it can't read the mouse's)");
    }
    uint32_t sig[HPROF_MAX];
    uint8_t hn = 0;
    String h = server.arg("hosts");
    int at = 0;
    while (at < (int) h.length()) {
        int comma = h.indexOf(',', at);
        String one = (comma < 0) ? h.substring(at) : h.substring(at, comma);
        uint32_t v = strtoul(one.c_str(), nullptr, 16);
        if (v && (hn < HPROF_MAX)) {
            sig[hn++] = v;
        }
        if (comma < 0) {
            break;
        }
        at = comma + 1;
    }
    if (n) {  // audit 11-01: a new table the full library can't take is refused as /api/curves?add=1 does, before anything changes
        uint16_t s16[CV_N] = {}, g16[CV_N] = {};
        for (uint8_t k = 0; k < n; k++) {
            s16[k] = sp[k];
            g16[k] = g[k];
        }
        CurveDecel od = {};
        if (cv_id) {
            od = cvdc[cv_id - 1];
        }
        if (!cvlib_fits(n, s16, g16, od.n, od.p10, od.g)) {
            return ok_json(false, "the library is full (16): delete a curve first");
        }
    }
    cv.n = n;
    cv.tau = tau;
    cv.dpi = dpi;
    memset(cv.sp, 0, sizeof(cv.sp));
    memset(cv.g, 0, sizeof(cv.g));
    for (uint8_t k = 0; k < n; k++) {
        cv.sp[k] = sp[k];
        cv.g[k] = g[k];
    }
    cv.hn = hn;
    memset(cv.sig, 0, sizeof(cv.sig));
    memcpy(cv.sig, sig, sizeof(uint32_t) * hn);
    if (server.hasArg("src") || !n) {
        String src = n ? server.arg("src") : String();
        String clean;
        for (unsigned i = 0; (i < src.length()) && (clean.length() < 240); i++) {
            char c = src[i];
            if (isalnum((unsigned char) c) || (c && strchr(":;,.=+/-", c))) {
                clean += c;
            }
        }
        cv_src = clean;
        prefs.putString("cvsrc", cv_src);
    }
    if (n) {
        uint8_t was = cv_id;
        CurveDecel od = {};  // fw67: a new table keeps the running curve's slow-down gain
        if (was) {
            od = cvdc[was - 1];
        }
        cv_id = cvlib_add(cvlib_clean(server.arg("name")), cv.n, cv.tau, cv_src_s10(), cv.sp, cv.g, od.n, od.p10, od.g);  // fw49
        if (cv_id != was) {  // fw60: as Use
            cvdg_stamp(was);
            cvdg_take(cv_id);
        }
        if (cv_cand == cv_id) {
            cv_cand = 0;
        }
    } else {
        cvdg_stamp(cv_id);  // fw62
        cv_id = 0;
        cvdg_take(0);
    }
    cvlib_lib_save();  // audit 11-01: the slow /curves.bin rewrites first; the running table and its id then back to back
    if (n) {
        nvs_put("curve", &cv, sizeof(cv));
    } else {
        prefs.remove("curve");
    }
    cvlib_ids_save();
    log_edit(EK_CURVE, n ? String(hn) + (hn == 1 ? " host" : " hosts") : String("off"));
    cv_sent_at = 0;  // send now
    cv_force = true;
    ok_json(true);
}

// POST /api/c360?cm=<cm/360 x 100>&sp=<cm/s x 10>&dpi=<n> keeps a result (newest first); ?del=<its Unix time> removes one
static void h_c360() {
    touch();
    if (!from_current_page()) {
        return;
    }
    if (server.hasArg("del")) {
        long t = server.arg("del").toInt();
        for (uint8_t k = 0; k < c360_n; k++) {
            if ((long) c360[k].t == t) {
                memmove(c360 + k, c360 + k + 1, sizeof(C360Rec) * (c360_n - k - 1));
                c360_n--;
                if (c360_n) {
                    nvs_put("c360", c360, sizeof(C360Rec) * c360_n);
                } else {
                    prefs.remove("c360");
                }
                return ok_json(true);
            }
        }
        return ok_json(false, "no result at that time");
    }
    long cm = server.arg("cm").toInt(), sp = server.arg("sp").toInt(), dpi = server.arg("dpi").toInt(), ctx = server.arg("ctx").toInt(), cid = server.arg("cid").toInt();  // fw48: ctx 0-255; fw49: cid
    if ((cm < 100) || (cm > 60000) || (sp < 0) || (sp > 65535) || (dpi < 50) || (dpi > 65535) || (cid < 0) || (cid > 255)) {
        return ok_json(false, "need cm (x100), sp (cm/s x10) and dpi");
    }
    if (c360_n == C360_KEEP) {  // fw47: full: drop the oldest result that a newer one at its speed replaced (greyed on the
        uint8_t drop = c360_n - 1;  // page); only when every result is the newest at its speed, the oldest of all
        for (int k = c360_n - 1; k > 0; k--) {
            bool newer = false;
            for (int i = 0; (i < k) && !newer; i++) {
                newer = (c360[i].sp10 == c360[k].sp10) && ((c360[i].dpi >> 16) == (c360[k].dpi >> 16));  // fw48: per context; fw49: and curve
            }
            if (newer) {
                drop = k;
                break;
            }
        }
        memmove(c360 + drop, c360 + drop + 1, sizeof(C360Rec) * (c360_n - drop - 1));
        c360_n--;
    }
    memmove(c360 + 1, c360, sizeof(C360Rec) * c360_n);
    c360_n++;
    c360[0].t = now_unix();
    c360[0].cm100 = (uint16_t) cm;
    c360[0].sp10 = (uint16_t) sp;
    c360[0].dpi = ((uint32_t) dpi & 0xFFFF) | ((uint32_t) (cid & 0xFF) << 16) | ((uint32_t) (ctx & 0xFF) << 24);
    nvs_put("c360", c360, sizeof(C360Rec) * c360_n);
    ok_json(true);
}

// POST /api/calibspeed?run=<its Unix time>&sp=<%> (fw36): the speed of a run saved before runs carried one (only when
// it has none yet)
static void h_calibspeed() {
    touch();
    long run = server.arg("run").toInt(), sp = server.arg("sp").toInt();
    if ((sp < 1) || (sp > 255)) {
        return ok_json(false, "sp 1-255");
    }
    for (uint8_t k = 0; k < calib_n; k++) {
        if ((long) calib[k].t == run) {
            if (calib[k].speed) {
                return ok_json(false, "that run has its speed already");
            }
            calib[k].speed = sp;
            nvs_put("calib3", calib, sizeof(CalibRec) * calib_n);
            return ok_json(true);
        }
    }
    ok_json(false, "no saved run at that time");
}

// GET /api/calib (fw27): the saved summaries, newest first
static void h_calib_get() {
    String j = "{\"runs\":[";
    for (uint8_t k = 0; k < calib_n; k++) {
        const CalibRec& c = calib[k];
        j += (k ? ",{\"t\":" : "{\"t\":") + String(c.t) + ",\"flags\":" + String(c.flags) + ",\"skipped\":" + String(c.skipped) + ",\"speed\":" + String(c.speed) + ",\"src\":" + String(c.src) + ",\"n\":[";
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
        cvt_keep_at = millis() | 1;  // fw81: an arm switch counts as BallSheet checking in (h_cvtry waits 12 s)
        if (tdpi_active && server.hasArg("force")) {  // fw33: stop owing it (the table stays as the mouse has it)
            tdpi_restore = false;
            tdpi_active = false;
            tdpi_phase = 0;
            prefs.remove("tdpi");
            prefs.remove("tdpis");
            log_edit(EK_TEMP_DPI, "dropped");
        } else if (tdpi_active) {
            tdpi_restore = true;
            tdpi_sent_at = 0;
            tdpi_phase = 0;
            tdpi_bad = 0;
        }
        return ok_json(true);
    }
    if (server.hasArg("keep")) {
        tdpi_keep_at = millis();
        return ok_json(tdpi_active && !tdpi_restore);
    }
    long v;
    if (!from_current_page()) {
        return;
    }
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
        tdpi_bad = 0;
        tdpi_stage = st[4];
        prefs.putUChar("tdpis", tdpi_stage);
        for (int i = 0; i < 5; i++) {
            tdpi_orig[2 * i] = u16(23 + 4 * i);
            tdpi_orig[2 * i + 1] = u16(25 + 4 * i);
        }
        nvs_put("tdpi", tdpi_orig, sizeof(tdpi_orig));
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
    cvt_stop();  // fw87: a BallSheet test starting ends the editor's live edit, as /api/cvtemp does
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

// fw88: the battery again once a minute while a page (or BallSheet, or a script) polls: one status query (command 04)
// through the Feather's probe slot. Not while the slot or the link is wanted by something else (the automatic stage's read,
// the DPI restore, a queued command, a probe already waiting), the mouse is away or didn't answer the last read (asleep),
// or a BallSheet test runs (nothing extra on the mouse's link while it measures). Its reply also shows as Diagnostics' probe.
static void bat_tick() {
    uint32_t now = millis();
    if (!link_up || !st_ok || !st_ext || !(st[3] & 1) || (st[2] & 8) || (cq_n > 0) || tdpi_active || ((st[92] & 3) == 1)) {
        return;
    }
    if ((auto_want != 0xFF) && ((auto_state == 0) || (auto_state == 1) || (auto_state == 5) || (auto_state == 8))) {
        return;
    }
    if (dgt_on || cvt_off || cvt_cand || (cvt_keep_at && (now - cvt_keep_at < 12000))) {
        return;
    }
    if (!bat_polled_at || (now - bat_polled_at > 10000) || (now - bat_asked_at < BAT_EVERY_MS) || (bat_age_known && (now - bat_at < BAT_EVERY_MS))) {
        return;
    }
    uint8_t r[16] = { 0x04 };
    uint8_t sum = 8;
    for (int i = 0; i < 15; i++) {
        sum += r[i];
    }
    r[15] = 0x55 - sum;
    if (queue_cmd(C_RAW, r, 16)) {
        bat_asked_at = now;
    }
}

static void tdpi_tick() {
    if (tdpi_active && !tdpi_restore && (millis() - tdpi_keep_at > 12000)) {
        tdpi_restore = true;
        tdpi_sent_at = 0;
        tdpi_phase = 0;
    }
    if (!tdpi_restore || !link_up || !st_ok || !st_ext || !(st[3] & 1) || (cq_n > 0) || (auto_state == 5) || (auto_state == 8)) {
        return;  // (fw33: not while the automatic stage's read uses the Feather's one probe slot)
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
        if (mine && (++tdpi_bad >= 10)) {  // fw33: answered with another value 10 times: the mouse can't store it (clamped?)
            tdpi_restore = false;
            tdpi_active = false;
            tdpi_phase = 0;
            prefs.remove("tdpi");
            prefs.remove("tdpis");
            log_edit_as(EK_TEMP_DPI, "unverified " + String(x), 7, 0);
            return;
        }
        tdpi_phase = 0;  // not there yet: send it again
    }
}

// POST /api/calibdiag (body: text, 4000 chars at most) | GET: the page's last calibration diagnostics, RAM only (fw29)
static void h_calibdiag_post() {
    touch();
    if (server.arg("plain").length() > 4000) {
        return ok_json(false, "diagnostics over 4000 chars");  // fw33: not cut into broken JSON
    }
    calib_diag = server.arg("plain");
    ok_json(true);
}

static void h_calibdiag_get() {
    server.send(200, "text/plain", calib_diag.length() ? calib_diag : String("none"));
}
// fw32: requests that change a setting must come from a current page (fw22+ adds "t", its clock, to every request;
// scripts and BallSheet send it too). An older page, e.g. a stale copy a browser restored, is refused and logged.
static bool from_current_page() {
    if (server.hasArg("t")) {
        return true;
    }
    String what = server.uri().substring(5);  // fw35: with its first argument, e.g. "set angle=-14"
    if (server.args() && (server.argName(0) != "t")) {
        what += " " + server.argName(0) + "=" + server.arg(0);
    }
    log_edit_as(EK_REFUSED, what, ua_device(), (uint32_t) server.client().remoteIP());
    ok_json(false, "this page is out of date: reload it");
    return false;
}

// /api/set?stage=0..4 | angle=-30..30 | pos=-100..101
static void h_set() {
    touch();
    if (!from_current_page()) {
        return;
    }
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
    if (!from_current_page()) {
        return;
    }
    uint8_t p[15];
    if (!parse_colours(server.arg("c"), p)) {
        return ok_json(false, "need 5 colours");
    }
    nvs_put("pal", p, 15);
    bool ok = queue_cmd(C_SET_PALETTE, p, 15);
    if (ok) {
        log_edit(EK_COLOURS, String());
    }
    ok_json(ok);
}

// /api/dpi?v=x1,y1,x2,y2,...  (10 numbers, 50..50000)
static void h_dpi() {
    touch();
    if (!from_current_page()) {
        return;
    }
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
    if (!from_current_page()) {
        return;
    }
    long as, ps, ah, ph, fl, poh, wh, rh;
    if (!(arg_int("as", 1, 10, &as) && arg_int("ps", 1, 25, &ps) && arg_int("ah", -30, 30, &ah) && arg_int("ph", -100, 101, &ph) &&
            arg_int("fl", 100, 5000, &fl) && arg_int("poh", 150, 3000, &poh) && arg_int("wh", 0, 3000, &wh) && arg_int("rh", 200, 3000, &rh))) {
        return ok_json(false, "a value is out of range");
    }
    uint8_t t[12] = { (uint8_t) as, (uint8_t) ps, (uint8_t) (int8_t) ah, (uint8_t) (int8_t) ph,
        (uint8_t) (fl & 0xff), (uint8_t) (fl >> 8), (uint8_t) (poh & 0xff), (uint8_t) (poh >> 8),
        (uint8_t) (wh & 0xff), (uint8_t) (wh >> 8), (uint8_t) (rh & 0xff), (uint8_t) (rh >> 8) };
    nvs_put("tune", t, 12);
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
static void h_ui() {  // fw37: or ?tab=0-3, the open tab
    touch();
    long v;
    if (arg_int("tab", 0, 3, &v)) {
        if (v != ui_tab) {
            ui_tab = (int8_t) v;
            prefs.putUChar("uitab", (uint8_t) v);
        }
        return ok_json(true);
    }
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

// fw71: the library (cvlib, cvdg, cvmeta, cvdc: 16 curves, ~4.3 KB) lives in one file on the QT Py's FFat area,
// /curves.bin: written whole to /curves.tmp, read back and checked (CRC-32), then renamed over it (the one before kept as
// /curves.bak). The 20 KB NVS partition had no room for a 7th curve (10-31); NVS keeps the running curve, its id, the
// candidate's and cvdg0. At boot: /curves.bin, else /curves.bak, else NVS (fw49-70's keys: moved over once, then removed).
// No file area (never formatted): it all stays in NVS as before.
#define LIB_MAGIC 0x56435747u  // "GWCV"
// fw76: LIB_VER 2: cvdc's shares in 0.1 % (uint16). A v1 file (fw71-75: whole %, DC_V1_SIZE bytes a curve) still reads,
// its shares x 10; boot then writes the library back as v2 (lib_write: the v1 file stays /curves.bak until the next write)
// fw98: LIB_VER 3: cvdc with 10 points (DC_N). v2 files (fw76-97: 6 points, DC_V2_SIZE bytes a curve) read too, converted
#define LIB_VER 3
#define LIB_V1_BODY (sizeof(cvlib) + sizeof(cvdg) + sizeof(cvmeta) + CVLIB_N * DC_V1_SIZE)
#define LIB_V2_BODY (sizeof(cvlib) + sizeof(cvdg) + sizeof(cvmeta) + CVLIB_N * DC_V2_SIZE)
struct __attribute__((packed)) LibHead {
    uint32_t magic;
    uint16_t ver, n;
    uint32_t crc;  // CRC-32 of the four tables after it, in this order: cvlib, cvdg, cvmeta, cvdc (as the file holds them)
};
static bool lib_v1 = false;  // fw76: the library loaded at boot came from an older file (fw98: v1 or v2; converted, boot writes it back as LIB_VER)

static uint32_t lib_crc() {
    uint32_t c = fo_crc32((const uint8_t*) cvlib, sizeof(cvlib), 0);
    c = fo_crc32((const uint8_t*) cvdg, sizeof(cvdg), c);
    c = fo_crc32((const uint8_t*) cvmeta, sizeof(cvmeta), c);
    return fo_crc32((const uint8_t*) cvdc, sizeof(cvdc), c);
}

// a library file's tables, checked; into: false = only check it
static bool lib_read(const char* path, bool into) {
    static uint8_t body[sizeof(cvlib) + sizeof(cvdg) + sizeof(cvmeta) + sizeof(cvdc)];
    static_assert((LIB_V1_BODY < sizeof(body)) && (LIB_V2_BODY < sizeof(body)), "v1 and v2 bodies fit the buffer");
    File f = FFat.open(path, "r");
    if (!f) {
        return false;
    }
    LibHead h = {};
    bool head = (f.read((uint8_t*) &h, sizeof(h)) == sizeof(h)) && (h.magic == LIB_MAGIC) && ((h.ver == LIB_VER) || (h.ver == 1) || (h.ver == 2)) && (h.n == CVLIB_N);
    size_t len = (h.ver == 1) ? LIB_V1_BODY : (h.ver == 2) ? LIB_V2_BODY : sizeof(body);  // fw76/fw98: older files' cvdc is shorter
    bool ok = head && (f.read(body, len) == len) && (fo_crc32(body, len, 0) == h.crc);
    f.close();
    if (!ok || !into) {
        return ok;
    }
    const uint8_t* p = body;
    memcpy(cvlib, p, sizeof(cvlib));
    p += sizeof(cvlib);
    memcpy(cvdg, p, sizeof(cvdg));
    p += sizeof(cvdg);
    memcpy(cvmeta, p, sizeof(cvmeta));
    p += sizeof(cvmeta);
    lib_v1 = (h.ver != LIB_VER);
    if (h.ver == 1) {
        for (int k = 0; k < CVLIB_N; k++) {
            cvdc_from_v1(k, p + DC_V1_SIZE * k);  // fw76: whole % -> 0.1 %
        }
    } else if (h.ver == 2) {
        for (int k = 0; k < CVLIB_N; k++) {
            cvdc_from_old(&cvdc[k], p + DC_V2_SIZE * k);  // fw98: 6 points -> 10
        }
    } else {
        memcpy(cvdc, p, sizeof(cvdc));
    }
    return true;
}

static bool lib_write() {
    LibHead h = { LIB_MAGIC, LIB_VER, CVLIB_N, lib_crc() };
    File f = FFat.open("/curves.tmp", "w");
    bool ok = (bool) f;
    if (ok) {
        ok = (f.write((const uint8_t*) &h, sizeof(h)) == sizeof(h)) && (f.write((const uint8_t*) cvlib, sizeof(cvlib)) == sizeof(cvlib)) &&
             (f.write((const uint8_t*) cvdg, sizeof(cvdg)) == sizeof(cvdg)) && (f.write((const uint8_t*) cvmeta, sizeof(cvmeta)) == sizeof(cvmeta)) &&
             (f.write((const uint8_t*) cvdc, sizeof(cvdc)) == sizeof(cvdc));
        f.close();
    }
    ok = ok && lib_read("/curves.tmp", false);  // what's on flash, read back
    if (ok) {
        if (FFat.exists("/curves.bin")) {  // 10-31 audit: the backup goes only when a newer good copy takes its place
            FFat.remove("/curves.bak");
            FFat.rename("/curves.bin", "/curves.bak");
        }
        ok = FFat.rename("/curves.tmp", "/curves.bin");
    }
    lib_writes++;
    if (!ok) {
        lib_fails++;
        d_nvs_fail++;  // counted with the settings saves that failed: the page warns the same way
        d_nvs_fail_key = "curves.bin";
    }
    return ok;
}

static void lib_nvs_clear() {  // the library's NVS keys, once it's in the file
    char key[8];
    for (int k = 1; k <= CVLIB_N; k++) {
        const char* pre[] = { "cvl", "cvg", "cvd" };
        for (const char* p : pre) {
            snprintf(key, sizeof(key), "%s%d", p, k);
            if (prefs.isKey(key)) {
                prefs.remove(key);
            }
        }
    }
    for (const char* k : { "cvmeta", "cvlib", "cvdg" }) {
        if (prefs.isKey(k)) {
            prefs.remove(k);
        }
    }
    memset(cvlib_nv, 0, sizeof(cvlib_nv));  // what NVS holds now (nothing)
    memset(cvdg_nv, 0, sizeof(cvdg_nv));
    memset(cvdc_nv, 0, sizeof(cvdc_nv));
    cvlib_blob = cvdg_blob = false;
}
static bool lib_fs_on() {
    return lib_fs;
}

// ---- fw69: the Feather's firmware over Wi-Fi (Feather v40 and its bootloader gw_boot: firmware/src/gw_ota.h). The page
// uploads a build's _ota.uf2; its blocks are unpacked into /feather.bin (the app's bytes from 0x10008000), with size,
// CRC-32, version and time in /feather.inf. Then, on the page's say-so, it goes over the link 40 bytes a frame. The
// Feather takes only the next expected offset (its main loop stands still ~45 ms while a sector erases, and the frames
// sent meanwhile are lost), so after each burst its update status (selected block 3) says where to go on from. Nothing
// else uses the link meanwhile (loop()). The Feather checks its whole copy (CRC-32) before gw_boot may copy it in.
#define FO_BASE 0x10008000u
#define FO_MAX 0x1F8000u
#define FO_CHUNK_MAX 240  // fw74: data bytes in a frame to a Feather v43+ (block 3 byte 2 bit 1); 40 to older ones
#define FO_BURST 8
#define C_OTA_BEGIN 0x13
#define C_OTA_DATA 0x14
#define C_OTA_END 0x15
#define C_OTA_APPLY 0x16
#define C_OTA_ABORT 0x17
enum : uint8_t { FO_IDLE, FO_BEGIN, FO_SEND, FO_END, FO_APPLY, FO_RESTART, FO_DONE, FO_ERROR };
static const char* const FO_NAMES[] = { "idle", "starting", "sending", "checking", "applying", "restarting", "done", "error" };
static uint8_t fo_st = FO_IDLE;
static String fo_err;
static uint32_t fo_size = 0, fo_crc = 0, fo_t = 0;  // the stored image (/feather.inf)
static uint8_t fo_ver = 0;
static uint32_t fo_sent = 0;  // the Feather's next expected offset, as last read
static uint32_t fo_at = 0, fo_moved_at = 0, fo_t0 = 0, fo_rs_at = 0, fo_secs = 0;
static uint8_t fo_tries = 0, fo_from = 0;
static uint8_t fo_chunk = 40;  // fw74: data bytes a frame for this Feather
static bool fo_begun = false;  // fw74: BEGIN sent at this clock (a v43 Feather erases ahead before it takes data: not sent again meanwhile)
static uint32_t fo_begun_at = 0;
static bool fo_apply_sent = false;  // audit 11-01: an APPLY of this update was taken by the bus (FO_APPLY)
static int8_t fo_boot = -1;  // the running Feather build takes updates: 1 yes, 0 no, -1 not known
static uint32_t fo_boot_at = 0;
static File fo_file;
// fw73: the link's clock while an update runs, tried fastest first. The lines hang on the chips' built-in pull-ups, so
// the faster ones may not hold: errors at a clock drop to the next and start the transfer over (a frame garbled at that
// clock can still pass its crc8; the whole image's CRC-32 would only tell at the end). Back to LINK_HZ when it ends.
#define FO_HZ_N 4
static const uint32_t FO_HZ[FO_HZ_N] = { 400000, 200000, 100000, LINK_HZ };
static uint8_t fo_hz = 0, fo_bad = 0, fo_slower_n = 0;
static String fo_slow_why;  // why the clock last stepped down (/api/feather "why")
static uint16_t fo_rd_n = 0, fo_rd_bad = 0;  // status reads at this clock, and those that came back short or garbled
static File fo_up;  // the upload
static bool fo_up_seen = false;
static String fo_up_err;
static uint8_t fo_ub[512];
static uint16_t fo_ub_n = 0;
static uint32_t fo_up_end = 0;
static uint32_t fo_ub_count = 0, fo_ub_total = 0;  // 10-31 audit: UF2 blocks seen, and the file's own count

static bool fo_busy() {
    return (fo_st != FO_IDLE) && (fo_st != FO_DONE) && (fo_st != FO_ERROR);
}

static uint32_t fo_crc32(const uint8_t* d, size_t n, uint32_t crc) {  // as gw_ota_crc32 (zlib's CRC-32)
    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static bool fo_frame(uint8_t cmd, const uint8_t* p, uint8_t len) {  // one link frame, outside the command queue
    uint8_t f[4 + 4 + FO_CHUNK_MAX];  // fw74: 248-byte update frames
    f[0] = cmd;
    f[1] = next_seq;  // the queue's counter: the Feather skips a frame with the last frame's number
    next_seq = (next_seq == 255) ? 1 : next_seq + 1;
    f[2] = len;
    if (len) {
        memcpy(f + 3, p, len);
    }
    f[3 + len] = crc8(f, 3 + len);
    Wire1.beginTransmission(LINK_ADDR);
    Wire1.write(f, 4 + len);
    return Wire1.endTransmission() == 0;
}

// the Feather's update status (block 3): false = no answer, or a Feather without it (the status block comes back)
static bool fo_read(uint8_t* b) {
    for (int k = 0; k < 2; k++) {
        Wire1.beginTransmission(LINK_ADDR);
        Wire1.write(LV_SEL);
        Wire1.write((uint8_t) 3);
        if (Wire1.endTransmission() != 0) {
            continue;
        }
        delayMicroseconds(LV_SEL_WAIT_US);
        uint8_t n = Wire1.requestFrom((uint8_t) LINK_ADDR, (uint8_t) 24);
        if (n != 24) {
            while (Wire1.available()) {
                Wire1.read();
            }
            continue;
        }
        for (int i = 0; i < 24; i++) {
            b[i] = Wire1.read();
        }
        if ((b[0] == 'U') && (crc8(b, 23) == b[23])) {
            fo_rd_n++;
            fz_flag_ota = (b[22] >= 48) && (b[2] & 4);  // fw77: block 4 holds a freeze (read between frames, loop())
            return true;
        }
        if (b[0] != 'G') {  // fw73: garbled (a pre-v40 Feather's status block isn't the bus's fault)
            fo_rd_n++;
            fo_rd_bad++;
        }
    }
    return false;
}

// fw73: over 10% of the status reads at this clock garbled: a slower clock gets more through
static bool fo_noisy() {
    return (fo_hz + 1 < FO_HZ_N) && (fo_rd_n >= 20) && (fo_rd_bad * 10 > fo_rd_n);
}

static const char* fo_err_text(uint8_t e) {
    switch (e) {
        case 1:
            return "this Feather build has no bootloader for updates: once over USB on FLOW, flash the _gwboot.uf2, then the _ota.uf2";
        case 2:
            return "the Feather refused its size";
        case 3:
            return "the Feather's copy didn't check out (CRC-32): send it again";
        case 4:
            return "the Feather's flash didn't take a page";
        default:
            return "the Feather stopped taking it";
    }
}

static void fo_fail(const String& why) {
    bool staged = (fo_st == FO_END) || (fo_st == FO_APPLY) || (fo_st == FO_RESTART);  // 10-31 audit: it may be staged on the Feather; audit 11-01: + RESTART (an unconfirmed APPLY: the old version came back)
    fo_st = FO_ERROR;
    fo_err = why;
    fo_secs = (millis() - fo_t0) / 1000;
    if (fo_file) {
        fo_file.close();
    }
    Wire1.setClock(LINK_HZ);  // fw73
    if (staged) {
        fo_frame(C_OTA_ABORT, nullptr, 0);  // so no later reset installs an update that ended in an error
    }
}

static void fo_clock(uint8_t k) {  // fw73
    fo_hz = k;
    fo_bad = 0;
    fo_rd_n = fo_rd_bad = 0;
    Wire1.end();  // 10-31 audit: a fresh start of the bus too (a Feather that restarted mid-read can leave it stuck)
    Wire1.begin(LINK_SDA, LINK_SCL, FO_HZ[k]);
    Wire1.setTimeOut(50);
}

// fw73: errors at this clock: the next slower one, and the transfer from the start (at the last one: give up)
static void fo_slower(const String& why) {
    if (fo_hz + 1 >= FO_HZ_N) {
        return fo_fail(why);
    }
    fo_slow_why = why + String(" (at ") + String(FO_HZ[fo_hz] / 1000) + " kHz)";
    fo_clock(fo_hz + 1);
    fo_slower_n++;
    fo_begun = false;
    fo_st = FO_BEGIN;
    fo_sent = 0;
    fo_tries = 0;
    fo_at = 0;
}

static void fo_tick() {
    uint8_t b[24];
    uint32_t now = millis();
    switch (fo_st) {
        case FO_BEGIN: {
            if (now - fo_at < 250) {
                return;
            }
            fo_at = now;
            if (!fo_begun) {
                if (++fo_tries > 30) {  // fw79: 7.5 s, not 2.5 (a Feather its watchdog is restarting answers block 3 stale meanwhile)
                    return fo_slower("the Feather didn't start taking it");  // fw73: at a slower clock first
                }
                uint8_t p[8];
                put32le(p, fo_size);
                put32le(p + 4, fo_crc);
                fo_frame(C_OTA_BEGIN, p, 8);
                fo_begun = true;
                fo_begun_at = now;
                delay(80);  // dropping its old record erases a sector
            }
            if (!fo_read(b)) {
                if (++fo_bad >= 24) {  // fw73; fw78: 6 s, not 0.75 (a Feather its watchdog is restarting answers again after ~5 s)
                    fo_slower("the Feather didn't start taking it");
                }
                return;
            }
            fo_bad = 0;
            bool mine = (le32(b + 8) == fo_size) && (le32(b + 12) == fo_crc);
            if ((b[1] == 1) && (le32(b + 4) == 0) && mine) {  // 10-31 audit: a fresh start, not a stale status
                fo_st = FO_SEND;
                fo_sent = 0;
                fo_moved_at = now;
            } else if ((b[1] == 4) && mine) {  // fw74: a v43 Feather erasing its staging slot ahead (about a second): wait
                if (now - fo_begun_at > 15000) {
                    fo_fail("the Feather didn't finish erasing");
                }
            } else if ((b[1] == 0) && ((b[3] == 1) || (b[3] == 2))) {  // refused (no bootloader, size); an older error is the last try's
                fo_fail(fo_err_text(b[3]));
            } else {
                fo_begun = false;  // fw74: the BEGIN didn't take: again
            }
            return;
        }
        case FO_SEND: {
            uint32_t off = fo_sent;
            uint8_t c[4 + FO_CHUNK_MAX];
            for (int k = 0; (k < FO_BURST) && (off < fo_size); k++) {
                uint8_t n = (fo_size - off < fo_chunk) ? (uint8_t) (fo_size - off) : fo_chunk;
                put32le(c, off);
                if (!fo_file.seek(off) || (fo_file.read(c + 4, n) != n)) {
                    return fo_fail("the QT Py couldn't read its copy of the image");
                }
                fo_frame(C_OTA_DATA, c, 4 + n);
                off += n;
            }
            delay(2);  // the Feather takes a frame every ms
            uint32_t stall = (fo_hz + 1 < FO_HZ_N) ? 1500 : 5000;  // fw73: a fast clock gets less time before the next
            bool got = fo_read(b);
            if (fo_noisy()) {
                return fo_slower("the link garbles at this clock");
            }
            if (!got) {
                if ((++fo_bad >= 3) || (now - fo_moved_at > stall)) {
                    fo_slower("the Feather stopped answering");
                }
                return;
            }
            fo_bad = 0;
            if (b[1] != 1) {
                return fo_slower(fo_err_text(b[3]));
            }
            uint32_t e = le32(b + 4);
            if (e != fo_sent) {
                fo_sent = e;
                fo_moved_at = now;
            } else if (now - fo_moved_at > stall) {
                return fo_slower("the Feather stopped taking it");
            }
            if (fo_sent >= fo_size) {
                fo_st = FO_END;
                fo_at = 0;
                fo_tries = 0;
            }
            return;
        }
        case FO_END:
            if (now - fo_at < 1000) {
                return;
            }
            fo_at = now;
            if (++fo_tries > 6) {
                return fo_fail("the Feather didn't confirm its copy");
            }
            fo_frame(C_OTA_END, nullptr, 0);
            delay(500);  // its CRC-32 over the whole copy
            if (fo_read(b)) {
                if (b[1] == 2) {
                    fo_st = FO_APPLY;
                    fo_at = 0;
                    fo_tries = 0;
                    fo_apply_sent = false;  // audit 11-01
                } else if (b[3]) {
                    fo_slower(fo_err_text(b[3]));  // fw73: e.g. a garbled frame that passed its crc8: all again, slower
                }
            }
            return;
        case FO_APPLY: {
            if (now - fo_at < 500) {
                return;
            }
            fo_at = now;
            if (++fo_tries > 6) {
                return fo_fail("the Feather didn't restart");
            }
            // audit 11-01: the Feather restarts 200 ms after it takes APPLY: read a few times in that window. Applying (3), or
            // no answer at all once an earlier try's APPLY went out, goes on to FO_RESTART, whose version check decides; still
            // READY: again
            bool sent = fo_frame(C_OTA_APPLY, nullptr, 0);
            bool answered = false, applying = false;
            for (int k = 0; (k < 5) && !applying; k++) {
                delay(k ? 30 : 20);
                if (fo_read(b)) {
                    answered = true;
                    applying = (b[1] == 3);
                }
            }
            bool restarted = fo_apply_sent && !answered;  // this try's reads all come before the 200 ms: only an earlier APPLY
            fo_apply_sent = fo_apply_sent || sent;
            if (applying || restarted) {
                Wire1.setClock(LINK_HZ);  // fw73: the new firmware is talked to as always
                fo_st = FO_RESTART;
                fo_rs_at = now;
                fo_at = now;
                if (fo_file) {
                    fo_file.close();
                }
            }
            return;
        }
        case FO_RESTART: {
            // gw_boot copies it in (a few seconds), then the new firmware answers; the first second may still be the old one
            if ((now - fo_at < 500) || (now - fo_rs_at < 1500)) {
                return;
            }
            fo_at = now;
            if (poll_status()) {
                toggles_known = false;  // as link_tick after a Feather that went away: it restarted, so its settings go back
                feather_init_done = false;
                ch_ok = false;
                hc_known = false;
                if (!fo_ver || (st[56] == fo_ver)) {
                    fo_st = FO_DONE;
                    fo_secs = (now - fo_t0) / 1000;
                    fo_boot = -1;
                } else {
                    fo_fail(String("the Feather came back with v") + String(st[56]) + ", not v" + String(fo_ver));
                }
                return;
            }
            static uint32_t reinit_at = 0;  // as link_tick: a Feather that restarted mid-read can leave this master stuck
            if (now - reinit_at >= 3000) {
                reinit_at = now;
                Wire1.end();
                Wire1.begin(LINK_SDA, LINK_SCL, LINK_HZ);
                Wire1.setTimeOut(50);
            }
            if (now - fo_rs_at > 60000) {
                fo_fail("no answer since the restart: on FLOW, flash the _ota.uf2 over USB to recover");
            }
            return;
        }
        default:
            return;
    }
}

static void fo_block(const uint8_t* b) {  // one 512-byte UF2 block of an upload into /feather.tmp
    if (fo_up_err.length()) {
        return;
    }
    uint32_t fl = le32(b + 8), addr = le32(b + 12), n = le32(b + 16);
    if ((le32(b) != 0x0A324655u) || (le32(b + 4) != 0x9E5D5157u) || (le32(b + 508) != 0x0AB16F30u)) {
        fo_up_err = "not a UF2 file";
        return;
    }
    if (!fo_ub_count) {
        fo_ub_total = le32(b + 24);
    }
    if ((le32(b + 20) != fo_ub_count) || (le32(b + 24) != fo_ub_total)) {  // 10-31 audit: a CRC only covers what arrived
        fo_up_err = "its blocks aren't one whole file in order";
        return;
    }
    fo_ub_count++;
    if (fl & 1) {
        return;  // "not main flash"
    }
    if (((fl & 0x2000) && (le32(b + 28) != 0xE48BFF56u)) || (n > 476)) {
        fo_up_err = "not an RP2040 image";
        return;
    }
    if (addr < FO_BASE) {
        fo_up_err = "this is the USB image (it starts where the bootloader goes): upload the _ota.uf2";
        return;
    }
    uint32_t off = addr - FO_BASE;
    if (off + n > FO_MAX) {
        fo_up_err = "too big for the Feather's app slot";
        return;
    }
    if (off < fo_up_end) {
        fo_up_err = "its blocks are out of order";
        return;
    }
    uint8_t pad[64];
    memset(pad, 0xFF, sizeof(pad));
    while (fo_up_end < off) {
        uint32_t k = std::min((uint32_t) sizeof(pad), off - fo_up_end);
        fo_up.write(pad, k);
        fo_up_end += k;
    }
    if (fo_up.write(b + 32, n) != n) {
        fo_up_err = "the QT Py couldn't store it";
        return;
    }
    fo_up_end = off + n;
}

// 10-31 audit: after an upload's last byte: all the blocks the file says it has
static void fo_up_finish() {
    if (!fo_up_err.length() && (fo_ub_count != fo_ub_total)) {
        fo_up_err = String("it's cut short (") + String(fo_ub_count) + " of " + String(fo_ub_total) + " blocks)";
    }
}

static void h_fo_upload() {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        fo_up_seen = true;
        fo_ub_n = 0;
        fo_up_end = 0;
        fo_ub_count = fo_ub_total = 0;
        fo_up_err = String();
        if (fo_busy()) {
            fo_up_err = "an update is running";
            return;
        }
        if (!fs_ok) {
            fs_ok = fs_mount_or_format();
        }
        fo_up = fs_ok ? FFat.open("/feather.tmp", "w") : File();
        if (!fo_up) {
            fo_up_err = "the QT Py's file storage isn't working";
        }
    } else if (u.status == UPLOAD_FILE_WRITE) {
        for (size_t i = 0; (i < u.currentSize) && fo_up; i++) {
            fo_ub[fo_ub_n++] = u.buf[i];
            if (fo_ub_n == sizeof(fo_ub)) {
                fo_block(fo_ub);
                fo_ub_n = 0;
            }
        }
    } else if ((u.status == UPLOAD_FILE_END) || (u.status == UPLOAD_FILE_ABORTED)) {
        if (fo_up) {
            fo_up.close();
        }
        if (!fo_up_err.length() && ((u.status == UPLOAD_FILE_ABORTED) || fo_ub_n)) {
            fo_up_err = (u.status == UPLOAD_FILE_ABORTED) ? "the upload stopped" : "not a UF2 file";
        }
        fo_up_finish();
        if (fo_up_err.length() && fs_ok) {
            FFat.remove("/feather.tmp");
        }
    }
}

static void fo_inf_load() {  // the stored image's facts, if there's one
    fo_size = 0;
    if (!fs_ok || !FFat.exists("/feather.bin") || !FFat.exists("/feather.inf")) {
        return;
    }
    File m = FFat.open("/feather.inf", "r");
    String s = m ? m.readString() : String();
    if (m) {
        m.close();
    }
    unsigned long size = 0, crc = 0, ver = 0, t = 0;
    File bin = FFat.open("/feather.bin", "r");
    size_t bin_size = bin ? bin.size() : 0;
    if (bin) {
        bin.close();
    }
    if ((sscanf(s.c_str(), "%lu %lx %lu %lu", &size, &crc, &ver, &t) == 4) && (size == bin_size)) {  // 10-31 audit: the .inf of this .bin
        fo_size = size;
        fo_crc = crc;
        fo_ver = (uint8_t) ver;
        fo_t = t;
    }
}

// CRC-32 and the version tag ("GWFW:v40;", gw_mouse.cc) of /feather.tmp
static bool fo_scan(uint32_t* crc, uint8_t* ver) {
    File f = FFat.open("/feather.tmp", "r");
    if (!f) {
        return false;
    }
    static uint8_t buf[1024 + 16];
    uint32_t c = 0;
    size_t keep = 0;
    *ver = 0;
    while (true) {
        int n = f.read(buf + keep, 1024);
        if (n <= 0) {
            break;
        }
        c = fo_crc32(buf + keep, n, c);
        size_t tot = keep + n;
        for (size_t i = 0; !*ver && (i + 9 <= tot); i++) {
            if (!memcmp(buf + i, "GWFW:v", 6)) {
                uint16_t v = 0;
                size_t j = i + 6;
                while ((j < tot) && (buf[j] >= '0') && (buf[j] <= '9') && (v < 255)) {
                    v = v * 10 + (buf[j++] - '0');
                }
                if ((j < tot) && (buf[j] == ';')) {
                    *ver = (uint8_t) v;
                }
            }
        }
        keep = (tot >= 15) ? 15 : tot;  // a tag cut by the chunk's end is found in the next
        memmove(buf, buf + tot - keep, keep);
    }
    f.close();
    *crc = c;
    return true;
}

// POST /api/feathergo?start=1 sends the stored image to the Feather; ?abort=1 stops sending. fw70: a route of its own:
// on /api/feather (it takes uploads), a POST without a multipart body reached the handler with the previous request's
// arguments, so start=1 was never seen
static void h_feather_go() {
    touch();
    if (!from_current_page()) {
        return;
    }
    if (radio != Radio::STA) {
        return ok_json(false, "only on the home network");
    }
    if (server.hasArg("abort")) {
        if (fo_busy() && (fo_st != FO_APPLY) && (fo_st != FO_RESTART)) {  // 10-31 audit: once APPLY may have gone, the Feather installs it anyway
            fo_frame(C_OTA_ABORT, nullptr, 0);
            fo_fail("stopped");
            return ok_json(true);
        }
        // audit 11-01: say so when it did nothing
        return ok_json(false, fo_busy() ? "too late to stop: the Feather is already installing it" : "no update is running");
    }
    if (server.hasArg("start")) {
        if (fo_busy()) {
            return ok_json(false, "an update is already running");
        }
        if (!fo_size || !(fo_file = FFat.open("/feather.bin", "r"))) {
            return ok_json(false, "upload the Feather's _ota.uf2 first");
        }
        if (!link_up || !st_ok) {
            fo_file.close();
            return ok_json(false, "the Feather isn't answering");
        }
        if (cq_n > 0) {
            fo_file.close();
            return ok_json(false, "the Feather is still taking settings: try again in a moment");  // 10-31 audit
        }
        uint8_t b[24];
        if ((st[56] < 40) || !fo_read(b) || !(b[2] & 1)) {
            fo_file.close();
            return ok_json(false, "this Feather can't take updates over Wi-Fi yet: once over USB on FLOW, flash the _gwboot.uf2, then the _ota.uf2");
        }
        fo_chunk = (b[2] & 2) ? FO_CHUNK_MAX : 40;  // fw74: a v43+ Feather takes 240 bytes a frame
        fo_begun = false;
        fo_from = st[56];
        fo_slower_n = 0;
        fo_clock(0);  // fw73
        fo_st = FO_BEGIN;
        fo_err = String();
        fo_sent = 0;
        fo_tries = 0;
        fo_at = 0;
        fo_t0 = millis();
        return ok_json(true);
    }
    ok_json(false, "start=1 or abort=1");
}

// GET /api/feather: the stored image and an update's progress. POST: multipart "file" = an _ota.uf2 to store (sending:
// /api/feathergo)
static void h_feather() {
    touch();
    if (server.method() == HTTP_GET) {
        if (!fo_busy() && link_up && st_ok && ((fo_boot < 0) || (millis() - fo_boot_at > 10000))) {
            uint8_t b[24];
            fo_boot = (st[56] < 40) ? 0 : fo_read(b) ? (b[2] & 1) : -1;
            fo_boot_at = millis();
        }
        char crc[12];
        snprintf(crc, sizeof(crc), "%08lx", (unsigned long) fo_crc);
        String j = "{\"state\":\"" + String(FO_NAMES[fo_st]) + "\",\"err\":\"" + fo_err + "\",\"sent\":" + String(fo_sent) + ",\"size\":" + String(fo_size) +
                   ",\"file\":" + (fo_size ? "{\"size\":" + String(fo_size) + ",\"crc\":\"" + String(crc) + "\",\"ver\":" + String(fo_ver) + ",\"t\":" + String(fo_t) + "}" : String("null")) +
                   ",\"boot\":" + String((fo_boot < 0) ? "null" : fo_boot ? "true" : "false") + ",\"fw\":" + String(st_ok ? st[56] : 0) + ",\"from\":" + String(fo_from) +
                   ",\"secs\":" + String(fo_busy() ? (millis() - fo_t0) / 1000 : fo_secs) + ",\"hz\":" + String(FO_HZ[fo_hz]) + ",\"slower\":" + String(fo_slower_n) + ",\"why\":\"" + fo_slow_why + "\",\"chunk\":" + String(fo_chunk) + "}";  // fw73: the clock it ran at, steps down
        server.send(200, "application/json", j);
        return;
    }
    bool up = fo_up_seen;
    fo_up_seen = false;
    if (!from_current_page()) {
        return;
    }
    if (radio != Radio::STA) {
        return ok_json(false, "only on the home network");  // as /api/update and /api/page
    }
    if (!up) {
        return ok_json(false, "nothing was uploaded");
    }
    if (fo_up_err.length()) {
        return ok_json(false, fo_up_err.c_str());
    }
    if (!fo_up_end) {
        return ok_json(false, "no firmware in that file");
    }
    uint32_t crc = 0;
    uint8_t ver = 0;
    if (!fo_scan(&crc, &ver)) {
        return ok_json(false, "the QT Py couldn't read it back");
    }
    FFat.remove("/feather.bin");
    if (!FFat.rename("/feather.tmp", "/feather.bin")) {
        return ok_json(false, "the QT Py couldn't keep it");
    }
    File m = FFat.open("/feather.inf", "w");
    if (m) {
        m.printf("%lu %08lx %u %lu", (unsigned long) fo_up_end, (unsigned long) crc, (unsigned) ver, (unsigned long) now_unix());
        m.close();
    }
    fo_inf_load();
    if (fo_st != FO_IDLE) {
        fo_st = FO_IDLE;  // a new image: the last update's result goes
        fo_err = String();
    }
    server.send(200, "application/json", "{\"ok\":true,\"size\":" + String(fo_size) + ",\"ver\":" + String(fo_ver) + "}");
}
// 10-31 audit: an upload's mount; formatting only while the partition never held the curve library (it would erase it)
static bool fs_mount_or_format() {
    if (FFat.begin(false)) {
        return true;
    }
    return !prefs.getUChar("libfs", 0) && FFat.begin(true);
}

static void page_fs_load() {  // fw41: is there an uploaded page for this firmware?
    page_fs = false;
    if (!fs_ok || !FFat.exists("/page.gz") || !FFat.exists("/page.fw")) {
        return;
    }
    File m = FFat.open("/page.fw", "r");
    String meta = m ? m.readString() : String("");
    if (m) {
        m.close();
    }
    long fw = 0, t = 0, len = 0;
    unsigned long src = 0;
    sscanf(meta.c_str(), "%ld %ld %ld %lx", &fw, &t, &len, &src);
    // audit 11-01: also the built-in page it was uploaded over (page_gz.h's FNV of page.h): a rebuild under the same number
    // with another page brings that page
    if ((fw != QT_FW) || (src != (unsigned long) PAGE_SRC_FNV)) {  // uploaded for another firmware: this firmware's own page is the right one
        FFat.remove("/page.gz");
        FFat.remove("/page.fw");
        return;
    }
    page_fs = true;
    page_fs_t = t;
    page_fs_len = len;
}

static void h_page_upload() {
    HTTPUpload& u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        page_up_len = 0;
        if (!fs_ok) {
            fs_ok = fs_mount_or_format();  // the first upload formats the partition (a few seconds); 10-31 audit: not once curves live there
        }
        page_up = fs_ok ? FFat.open("/page.tmp", "w") : File();
        page_up_ok = (bool) page_up;
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (!page_up || (page_up.write(u.buf, u.currentSize) != u.currentSize)) {
            page_up_ok = false;  // fw47: a short write is a broken page
        }
        page_up_len += u.currentSize;
    } else if ((u.status == UPLOAD_FILE_END) || (u.status == UPLOAD_FILE_ABORTED)) {
        if (page_up) {
            page_up.close();
        }
        if (u.status == UPLOAD_FILE_ABORTED) {
            page_up_ok = false;
            FFat.remove("/page.tmp");  // fw47: a dropped upload mustn't be installed by a later request
        }
    }
}

// POST /api/pagereset: back to the built-in page. audit 11-01: a route of its own (was /api/page?reset=1): /api/page takes
// uploads, so a POST without a multipart body saw the previous request's arguments (as /api/feathergo, fw70)
static void h_page_reset() {
    touch();
    if (radio != Radio::STA) {
        return ok_json(false, "only on the home network");
    }
    if (fs_ok) {
        FFat.remove("/page.gz");
        FFat.remove("/page.fw");
    }
    page_fs = false;
    ok_json(true);
}

// POST /api/page (multipart "file": the gzipped page) (fw41); audit 11-01: never removes the stored page (/api/pagereset does)
static void h_page_done() {
    touch();
    bool up = page_up_ok;
    page_up_ok = false;
    if (radio != Radio::STA) {
        return ok_json(false, "only on the home network");  // fw47: as /api/update (the setup network is open)
    }
    if (!fs_ok) {
        return ok_json(false, "no file system");
    }
    File f = FFat.open("/page.tmp", "r");
    bool good = up && f && (f.size() == page_up_len) && (f.size() > 1024) && (f.read() == 0x1f) && (f.read() == 0x8b);
    if (f) {
        f.close();
    }
    if (!good) {
        FFat.remove("/page.tmp");
        return ok_json(false, "not a gzipped page");
    }
    FFat.remove("/page.gz");
    FFat.rename("/page.tmp", "/page.gz");
    File m = FFat.open("/page.fw", "w");
    if (m) {
        m.print(String(QT_FW) + " " + String(now_unix()) + " " + String(page_up_len) + " " + String((unsigned long) PAGE_SRC_FNV, HEX));  // audit 11-01: + the built-in page's FNV
        m.close();
    }
    page_fs_load();
    fo_inf_load();  // fw69
    ok_json(page_fs, page_fs ? nullptr : "stored, but couldn't read it back");
}

static void h_page() {
    touch();
    queue_cmd(C_REFRESH, nullptr, 0);  // opening the page reads the mouse's current settings
    // fw32: never cached. On 10-21 Firefox restored an unloaded HYDROC tab from its cache: an old page without the
    // fw21 input guard, whose restored slider values went straight to the mouse (angle -14, position 15).
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("Pragma", "no-cache");
    if (page_fs) {  // fw41: the uploaded page (streamFile adds Content-Encoding: gzip for a .gz name)
        File f = FFat.open("/page.gz", "r");
        if (f) {
            server.streamFile(f, "text/html");
            f.close();
            return;
        }
    }
    server.sendHeader("Content-Encoding", "gzip");  // fw35: 32 KB instead of 88 KB
    server.send_P(200, "text/html", (PGM_P) PAGE_GZ, PAGE_GZ_LEN);
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
    ui_tab = prefs.isKey("uitab") ? (int8_t) prefs.getUChar("uitab", 0) : -1;
    if (prefs.isKey("rotsign")) {
        prefs.remove("rotsign");  // fw19's learnt swipe direction; the page knows it now
    }
    b_reason = (uint8_t) esp_reset_reason();
    b_boots = prefs.getULong("boots", 0) + 1;
    prefs.putULong("boots", b_boots);
    edit_n = prefs.getBytes("edits", edit_log, sizeof(edit_log)) / sizeof(EditLog);  // fw22: the page edits so far
    if (prefs.getBytes("drst", &drst, sizeof(drst)) != sizeof(drst)) {  // fw112: the last reset of the frame-timing numbers
        memset(&drst, 0, sizeof(drst));
    }
    edit_cap_kind(EK_AUTO_STAGE, EDIT_AUTO_KEEP);  // fw102: a log from before the cap
    hcap_n = prefs.getBytes("hcaps", hcap, sizeof(hcap)) / sizeof(HostCap);         // fw23: USB host captures so far
    fz_have = prefs.getBytes("fz", &fz, sizeof(fz)) == sizeof(fz);                  // fw77: the last Feather freeze
    if (!fz_have) {
        fz = {};
    } else {  // review 11-02: a Feather still reporting it after only the QT Py restarted is the same freeze
        memcpy(fz_seen, fz.blk, FZ_LEN);
        fz_seen_ok = true;
    }
    hprof_n = prefs.getBytes("hprof", hprof, sizeof(hprof)) / sizeof(HostProf);     // fw24: a stage per host
    calib_n = prefs.getBytes("calib3", calib, sizeof(calib)) / sizeof(CalibRec);  // fw27: calibration summaries (fw36: + speed, fw64: + src)
    if (!calib_n && prefs.isKey("calib2")) {  // fw64: move the fw36-63 records (47 bytes, no src) over once
        static uint8_t old2[CALIB_KEEP * CALIB_V2_SIZE];
        calib_n = prefs.getBytes("calib2", old2, sizeof(old2)) / CALIB_V2_SIZE;
        for (uint8_t k = 0; k < calib_n; k++) {
            memcpy(&calib[k], old2 + CALIB_V2_SIZE * k, CALIB_V2_SIZE);
            calib[k].src = 0;
        }
        if (calib_n) {
            nvs_put("calib3", calib, sizeof(CalibRec) * calib_n);
        }
        prefs.remove("calib2");
    }
    c360_n = prefs.getBytes("c360", c360, sizeof(c360)) / sizeof(C360Rec);       // fw43: cm/360 results
    if (!calib_n && prefs.isKey("calib")) {  // fw36: move the fw27-35 records (46 bytes, no speed) over once
        uint8_t old[CALIB_KEEP * CALIB_V1_SIZE];
        calib_n = prefs.getBytes("calib", old, sizeof(old)) / CALIB_V1_SIZE;
        for (uint8_t k = 0; k < calib_n; k++) {
            memcpy(&calib[k], old + CALIB_V1_SIZE * k, CALIB_V1_SIZE);
            calib[k].speed = 0;
            calib[k].src = 0;
        }
        if (calib_n) {
            nvs_put("calib3", calib, sizeof(CalibRec) * calib_n);
        }
        prefs.remove("calib");
    }
    tdpi_stage = prefs.getUChar("tdpis", 0xFF);                                        // fw29: which stage the test lowered
    if (prefs.getBytes("tdpi", tdpi_orig, sizeof(tdpi_orig)) == sizeof(tdpi_orig)) { // fw28: a test's DPI still owed back
        tdpi_active = true;
        tdpi_restore = true;
    }
    fs_ok = FFat.begin(false);  // fw41: an uploaded page (no format here: an unformatted partition waits for the first upload)
    if (!fs_ok) {
        delay(50);
        fs_ok = FFat.begin(false);  // 10-31 audit: once more before the library falls back to NVS
    }
    page_fs_load();
    fo_inf_load();  // fw69
    cv_src = prefs.getString("cvsrc", "");  // fw46: the speed curve
    // fw71: the library's file; 10-31 audit: /curves.tmp is a whole checked copy too (a power cut between lib_write's renames)
    lib_fs = fs_ok && (lib_read("/curves.bin", true) || lib_read("/curves.tmp", true) || lib_read("/curves.bak", true));
    cvhist_load();  // fw86
    if (fs_ok) {  // fw103
        edit_old_n = old_count(EDIT_OLD_BIN, sizeof(EditLog));
        cvh_old_n = old_count(CVH_OLD_BIN, sizeof(CvHist));
        if (!FFat.exists(CVN_BIN) && FFat.exists(CVN_BIN ".tmp")) {  // fw104: a save cut off between its remove and rename
            FFat.rename(CVN_BIN ".tmp", CVN_BIN);
        }
    }
    if (lib_fs && lib_v1) {  // fw76: converted from a v1 file (whole-% slow-down shares): written back as v2 now; a failure
        lib_write();         // leaves the v1 files as they were (read and converted again at the next boot) and lib_fs on
    }
    lib_off = !lib_fs && prefs.getUChar("libfs", 0);  // audit 11-01: in its file, which didn't load: no NVS fallback, nothing written
    lib_dmg = lib_off && fs_ok;
    if (!lib_fs && !lib_off) {  // fw49-70: the library in NVS
    cvlib_blob = prefs.isKey("cvlib");      // fw65: a curve per key; the fw49-64 blob is split up once (below)
    if (!cvlib_blob) {
        for (int k = 0; k < CVLIB_N; k++) {
            char key[8];
            snprintf(key, sizeof(key), "cvl%d", k + 1);
            if ((prefs.getBytes(key, &cvlib[k], sizeof(CurveLib)) != sizeof(CurveLib)) || !cvlib[k].used) {
                memset(&cvlib[k], 0, sizeof(CurveLib));
            }
        }
        memcpy(cvlib_nv, cvlib, sizeof(cvlib));
    } else if (prefs.getBytes("cvlib", cvlib, sizeof(cvlib)) != sizeof(cvlib)) {  // fw49
        memset(cvlib, 0, sizeof(cvlib));
        const size_t OLD = sizeof(CurveLib) - 32;  // the first fw49 library had 16-byte names: moved over
        static uint8_t old[CVLIB_N * (sizeof(CurveLib) - 32)];
        if (prefs.getBytesLength("cvlib") == sizeof(old) && prefs.getBytes("cvlib", old, sizeof(old)) == sizeof(old)) {
            for (int k = 0; k < CVLIB_N; k++) {
                const uint8_t* o = old + OLD * k;
                cvlib[k].used = o[0];
                memcpy(cvlib[k].name, o + 1, 15);
                memcpy(&cvlib[k].n, o + 17, OLD - 17);
            }
        }
    }
    if (cvlib_blob) {
        for (int k = 0; k < CVLIB_N; k++) {
            if (!cvlib[k].used) {
                memset(&cvlib[k], 0, sizeof(CurveLib));  // so an empty slot matches the (empty) saved copy
            }
        }
        // fw65: each curve under its own key, then the blob goes (a failure leaves it for the next boot); not cvlib_save:
        // cvid and cvcand aren't read yet
        slots_save("cvl", cvlib, cvlib_nv, sizeof(CurveLib), cvlib_used, "cvlib", &cvlib_blob);
    }
    for (int k = 0; k < CVLIB_N; k++) {  // fw67: the slow-down gains (before cvlib_add below: they're part of a curve's match)
        char key[8];
        snprintf(key, sizeof(key), "cvd%d", k + 1);
        // fw76: fw67-75's blob (DC_V1_SIZE bytes, whole %) converted; NVS keeps it until that curve's next save (cvdc_nv
        // below holds the converted copy, so an unchanged curve isn't rewritten), and it converts the same at every boot
        size_t len = prefs.isKey(key) ? prefs.getBytesLength(key) : 0;
        bool got = false;
        if (len == sizeof(CurveDecel)) {
            got = prefs.getBytes(key, &cvdc[k], sizeof(CurveDecel)) == sizeof(CurveDecel);
        } else if (len == DC_V2_SIZE) {  // fw98: fw76-97's 6 points
            uint8_t o[DC_V2_SIZE];
            got = prefs.getBytes(key, o, sizeof(o)) == sizeof(o);
            if (got) {
                cvdc_from_old(&cvdc[k], o);
            }
        } else if (len == DC_V1_SIZE) {
            uint8_t o[DC_V1_SIZE];
            got = prefs.getBytes(key, o, sizeof(o)) == sizeof(o);
            if (got) {
                cvdc_from_v1(k, o);
            }
        }
        if (!got || !cvlib[k].used || (cvdc[k].n > DC_N)) {
            memset(&cvdc[k], 0, sizeof(CurveDecel));
        }
    }
    memcpy(cvdc_nv, cvdc, sizeof(cvdc));
    }  // fw71: !lib_fs
    cv_id = prefs.getUChar("cvid", 0);
    dc_min10 = prefs.getUShort("dcmin", 200);  // fw109
    cv_cand = prefs.getUChar("cvcand", 0);
    if ((cv_id > CVLIB_N) || (cv_id && !lib_off && !cvlib[cv_id - 1].used)) {  // audit 11-01: kept while the library didn't load
        cv_id = 0;
    }
    if ((cv_cand > CVLIB_N) || (cv_cand && !lib_off && !cvlib[cv_cand - 1].used)) {
        cv_cand = 0;
    }
    if (prefs.getBytes("curve", &cv, sizeof(cv)) != sizeof(cv) || (cv.n > CV_N) || (cv.hn > HPROF_MAX) || (cv.tau < 1) || (cv.tau > 200)) {
        cv.n = 0;
        cv.hn = 0;
        cv.tau = 8;
    }
    if (!lib_fs && !lib_off && (prefs.getBytes("cvmeta", cvmeta, sizeof(cvmeta)) != sizeof(cvmeta))) {  // fw61; fw62: before the migration below (it saves cvmeta); fw71: in the file
        memset(cvmeta, 0, sizeof(cvmeta));
        for (int k = 0; k < CVLIB_N; k++) {
            cvmeta[k].ord = k + 1;
        }
        cvmeta_save();
    }
    if (prefs.getBytes("cvdg0", &cvdg0, sizeof(cvdg0)) != sizeof(cvdg0)) {  // fw62
        memset(&cvdg0, 0, sizeof(cvdg0));
    }
    if ((cv.n >= 2) && !cv_id && !lib_off) {  // fw49: the curve saved before named curves becomes the library's first
        cv_id = cvlib_add(String(""), cv.n, cv.tau, cv_src_s10(), cv.sp, cv.g, 0, nullptr, nullptr);
        cvlib_save();
    }
    dg_src = prefs.getString("dgsrc", "");  // fw39: where the gains came from
    dg_src_t = prefs.getULong("dgsrct", 0);
    if (prefs.getBytes("dgain", &dg, sizeof(dg)) != sizeof(dg)) {                     // fw26: direction gains
        for (int k = 0; k < 8; k++) {
            dg.g[k] = 1000;
        }
        dg.n = 0;
    }
    if (!lib_fs && !lib_off) {  // fw71: the curves' gains are in the file otherwise
    cvdg_blob = prefs.isKey("cvdg");  // fw65: per-curve keys; the fw60-64 blob is split up once
    bool dg_any = false;
    if (cvdg_blob && (prefs.getBytes("cvdg", cvdg, sizeof(cvdg)) == sizeof(cvdg))) {
        dg_any = true;
        for (int k = 0; k < CVLIB_N; k++) {
            if (!cvdg[k].has) {
                memset(&cvdg[k], 0, sizeof(CurveGains));  // so an empty slot matches the (empty) saved copy
            }
        }
        cvdg_save();  // each curve's under its own key, then the blob goes (a failure leaves it for the next boot)
    } else {
        memset(cvdg, 0, sizeof(cvdg));
        for (int k = 0; k < CVLIB_N; k++) {
            char key[8];
            snprintf(key, sizeof(key), "cvg%d", k + 1);
            if ((prefs.getBytes(key, &cvdg[k], sizeof(CurveGains)) == sizeof(CurveGains)) && cvdg[k].has) {
                dg_any = true;
            } else {
                memset(&cvdg[k], 0, sizeof(CurveGains));
            }
        }
        memcpy(cvdg_nv, cvdg, sizeof(cvdg));
    }
    if (!dg_any) {  // fw60: per-curve gains; first boot: the running curve's are the ones in use
        cvdg_stamp(cv_id);
    }
    }  // fw71: !lib_fs
    if (fs_ok && !lib_fs && !lib_off) {  // fw71: the library moves to its file once; NVS's copies go only after it read back right
        // 10-31 audit: a library file that's there but didn't read back is set aside (name.bad), never written over
        const char* lib_files[] = { "/curves.bin", "/curves.tmp", "/curves.bak" };
        for (const char* p : lib_files) {
            if (FFat.exists(p)) {
                String bad = String(p) + ".bad";
                FFat.remove(bad.c_str());
                FFat.rename(p, bad.c_str());
                lib_bad++;
            }
        }
        lib_fs = lib_write();
    }
    if (lib_fs && !prefs.getUChar("libfs", 0)) {
        prefs.putUChar("libfs", 1);  // 10-31 audit: the library lives in FFat: an upload never formats it (see fs_mount_or_format)
    }
    if (lib_fs) {
        lib_nvs_clear();
    }
    if (prefs.getUChar("sigv", 1) < 2) {  // fw34: hosts saved under the v1 signature
        sig_migrate();
        prefs.putUChar("sigv", 2);
    }
    auto_prev = auto_rtc;  // fw34: the last boot's automatic stage (garbage after a power cut: the magic and boot say so)
    auto_rtc.magic = 0;
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
    server.on("/api/perfwin", HTTP_GET, h_perfwin);  // fw90
    server.on("/api/osd", HTTP_GET, h_osd);
    server.on("/api/hosts", HTTP_GET, h_hosts);
    server.on("/api/hostprof", HTTP_POST, h_hostprof);
    server.on("/api/dirgain", HTTP_POST, h_dirgain);
    server.on("/api/tempdpi", HTTP_POST, h_tempdpi);
    server.on("/api/calibdiag", HTTP_POST, h_calibdiag_post);
    server.on("/api/calibdiag", HTTP_GET, h_calibdiag_get);
    server.on("/api/calib", HTTP_POST, h_calib_post);
    server.on("/api/calib", HTTP_GET, h_calib_get);
    server.on("/api/calibspeed", HTTP_POST, h_calibspeed);
    server.on("/api/calibdel", HTTP_POST, h_calibdel);
  server.on("/api/calibcid", HTTP_POST, h_calibcid);  // fw56
    server.on("/api/dgtemp", HTTP_POST, h_dgtemp);
    server.on("/api/turn", HTTP_POST, h_turn);  // fw43
    server.on("/api/c360", HTTP_POST, h_c360);  // fw43
    server.on("/api/curve", HTTP_POST, h_curve);  // fw46
    server.on("/api/cvtemp", HTTP_POST, h_cvtemp);  // fw48
    server.on("/api/cvtry", HTTP_POST, h_cvtry);  // fw81
    server.on("/api/curvehist", HTTP_GET, h_curvehist);  // fw86
    server.on("/api/editsold", HTTP_GET, h_editsold);    // fw103
    server.on("/api/cvnotes", HTTP_GET, h_cvnotes);      // fw104
    server.on("/api/cvnote", HTTP_POST, h_cvnote);
    server.on("/api/diagreset", HTTP_POST, h_diagreset);  // fw112
    server.on("/api/usbreset", HTTP_POST, h_usbreset);   // fw105
    server.on("/api/dcmin", HTTP_POST, h_dcmin);         // fw109
  server.on("/api/live", HTTP_GET, h_live);  // fw50
  server.on("/api/live", HTTP_POST, h_live);
    server.on("/api/nvs", HTTP_GET, h_nvs);  // fw65
    server.on("/api/curves", HTTP_GET, h_curves);  // fw49
    server.on("/api/curves", HTTP_POST, h_curves);
    server.on("/api/page", HTTP_POST, h_page_done, h_page_upload);
    server.on("/api/pagereset", HTTP_POST, h_page_reset);  // audit 11-01
    server.on("/api/feather", HTTP_GET, h_feather);  // fw69
    server.on("/api/feather", HTTP_POST, h_feather, h_fo_upload);
    server.on("/api/feathergo", HTTP_POST, h_feather_go);  // fw70
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
    if (fo_busy()) {  // fw69: an update has the link to itself; the page keeps being served
        fo_tick();
        if (fz_flag_ota && fo_busy()) {
            fz_check(true);  // fw77: a Feather that froze mid-update and was restarted by its watchdog
        }
        radio_tick();
        if (radio != Radio::OFF) {
            server.handleClient();
        }
        show_state();
        delay(1);
        return;
    }
    oq_tick();  // fw107
    link_tick();
    radio_tick();
    hcap_tick();
    auto_tick();
    osd_tick();
    chord_tick();  // fw108
    dg_tick();
    cv_tick();
    live_tick();
    tdpi_tick();
    bat_tick();  // fw88
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

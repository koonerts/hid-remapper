// G-Wolves settings page on an Adafruit QT Py ESP32-S3, plugged into the HID Remapper Feather's
// STEMMA QT port. The QT Py is the I2C master; the Feather (gw_link.cc) answers at 0x42 with a
// 64-byte status block and takes command frames (layout in firmware/src/gw_mouse.cc).
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

#include "page.h"

#define LINK_ADDR 0x42
#define LINK_SDA 41  // STEMMA QT (Wire1)
#define LINK_SCL 40
#define LINK_HZ 50000  // only the pins' built-in pull-ups hold the lines up, so keep it slow
#define STATUS_LEN 64
#define QT_FW 6        // this firmware's number, shown in /api/status
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

static WebServer server(80);
static DNSServer dns;
static Preferences prefs;

static uint8_t st[STATUS_LEN];
static bool st_ok = false;
static uint32_t st_at = 0;       // last good status
static bool link_up = false;
static bool toggles_known = false;
static uint8_t last_toggles = 0;
static bool feather_init_done = false;  // tuning/palette pushed since the Feather (re)started
// link diagnostics (/api/status "diag")
static uint32_t d_polls = 0, d_ok = 0, d_short = 0, d_bad = 0;
static uint8_t d_last_n = 0, d_b0 = 0, d_b1 = 0;
static bool last_answered = false;  // the last read returned a full block, valid or not

enum class Radio : uint8_t { OFF, JOINING, STA, AP };
static Radio radio = Radio::OFF;
static uint32_t radio_since = 0;
static uint32_t last_http = 0;
static uint8_t wmode = 0;          // Wi-Fi switches itself off: 0 = 10 min idle, 1 = 1 hour idle, 2 = never
static bool radio_manual = true;   // this radio session was started by hand (button, chord, page)

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
    st_ok = true;
    st_at = millis();
    return true;
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
        WiFi.setSleep(true);
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
        return;
    }
    if (!ok) {
        return;
    }
    // a restarted Feather has lost its tuning flag: re-baseline the counter, give back the settings
    if (feather_init_done && !(st[2] & 1)) {
        feather_init_done = false;
        toggles_known = false;
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

static void touch() {
    last_http = millis();
}

static void ok_json(bool ok, const char* err = nullptr) {
    String s = ok ? "{\"ok\":true}" : String("{\"ok\":false,\"err\":\"") + (err ? err : "error") + "\"}";
    server.send(ok ? 200 : 400, "application/json", s);
}

// not counted as use: an open tab alone shouldn't keep the radio on while playing
static void h_status() {
    String j;
    j.reserve(1200);
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
        j += ",\"colours_read\":" + String((f & 64) ? "true" : "false");
        j += ",\"dpi_known\":" + String((f & 128) ? "true" : "false");
        j += ",\"refreshing\":" + String((st[2] & 4) ? "true" : "false");
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
    j += ",\"qt_fw\":" + String(QT_FW);
    j += ",\"diag\":{\"polls\":" + String(d_polls) + ",\"ok\":" + String(d_ok) + ",\"short\":" + String(d_short) +
         ",\"bad\":" + String(d_bad) + ",\"last_n\":" + String(d_last_n) + ",\"b0\":" + String(d_b0) + ",\"b1\":" + String(d_b1) +
         ",\"up_s\":" + String(millis() / 1000) + "}";
    j += ",\"pending\":" + String(cq_n);
    j += ",\"radio\":{\"mode\":\"";
    j += (radio == Radio::STA) ? "sta" : (radio == Radio::AP) ? "ap" : (radio == Radio::JOINING) ? "joining" : "off";
    j += "\",\"ssid\":\"" + prefs.getString("ssid", "") + "\"";
    j += ",\"ip\":\"" + ((radio == Radio::AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "\"";
    uint32_t lim = idle_limit();
    long left = !lim ? -1 : (millis() - last_http < lim) ? (long) ((lim - (millis() - last_http)) / 1000) : 0;
    j += ",\"keep\":" + String(wmode);
    j += ",\"off_in_s\":" + String(left) + "}}";
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

// /api/set?stage=0..4 | angle=-30..30 | pos=-100..101
static void h_set() {
    touch();
    long v;
    uint8_t b;
    if (arg_int("stage", 0, 4, &v)) {
        b = v;
        return ok_json(queue_cmd(C_SET_STAGE, &b, 1));
    }
    if (arg_int("angle", -30, 30, &v)) {
        b = (uint8_t) (int8_t) v;
        return ok_json(queue_cmd(C_SET_ANGLE, &b, 1));
    }
    if (arg_int("pos", -100, 101, &v)) {
        b = (uint8_t) (int8_t) v;
        return ok_json(queue_cmd(C_SET_POS, &b, 1));
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
    ok_json(queue_cmd(C_SET_PALETTE, p, 15));
}

// /api/dpi?v=x1,y1,x2,y2,...  (10 numbers, 50..50000)
static void h_dpi() {
    touch();
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
    ok_json(queue_cmd(C_SET_DPI, d, 20));
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
    ok_json(queue_cmd(C_SET_TUNING, t, 12));
}

static void h_refresh() {
    touch();
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
    if (wmode > 2) {
        wmode = 0;
    }
    Wire1.begin(LINK_SDA, LINK_SCL, LINK_HZ);
    Wire1.setTimeOut(50);
    server.on("/", HTTP_GET, h_page);
    server.on("/api/status", HTTP_GET, h_status);
    server.on("/api/set", HTTP_POST, h_set);
    server.on("/api/palette", HTTP_POST, h_palette);
    server.on("/api/dpi", HTTP_POST, h_dpi);
    server.on("/api/tune", HTTP_POST, h_tune);
    server.on("/api/refresh", HTTP_POST, h_refresh);
    server.on("/api/flash", HTTP_POST, h_flash);
    server.on("/api/radio_off", HTTP_POST, h_radio_off);
    server.on("/api/wifi", HTTP_POST, h_wifi);
    server.on("/api/wifimode", HTTP_POST, h_wifimode);
    server.on("/api/update", HTTP_POST, h_update_done, h_update_upload);
    server.onNotFound(h_not_found);
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

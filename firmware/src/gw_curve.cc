// v30: a speed curve, the XIM's ballistic curve done here instead. The QT Py sends it (LINK_SET_CURVE) after every
// power-on and when it changes, "off" on hosts it shouldn't apply to; nothing is kept in the Feather's flash.
//
// The speed is the motion's length per time in cm/s (counts through the mouse's DPI, so a DPI stage change needs no
// new table), smoothed exponentially over tau_ms by the frames' own times, and back to rest after 50 ms without motion
// (v31: the frames tick every 1 ms whether the mouse moves or not, so a 50 ms gap between frames never came). Like
// the direction gains, only the size of the motion changes, never its direction, and the fractions left after
// rounding carry over to the next frame.

#include "gw_curve.h"

#include <math.h>
#include <string.h>

#include "gw_link.h"

#define GW_CURVE_GAP_US 50000
#define GW_CURVE_STILL 0.1f  // v36: cm/s below which the hand is still: a resting mouse at 40,000 DPI still sends a count every
                             // few ms (about 6% of resting time), which counted as moving at 0.0 cm/s in the live flag and the histogram

static bool on = false;
static bool got = false;
static uint8_t table_crc = 0;
static uint8_t n_pts = 0;
static float tau = 8.0f;
static uint16_t dpi_fb = 0;
static float pt_sp[GW_CURVE_N], pt_g[GW_CURVE_N];  // cm/s, gain
static float spd = 0;                              // cm/s, smoothed
static uint64_t last_us = 0;
static bool have_t = false;
static uint64_t moved_us = 0;  // v31: the last frame with motion
static bool moved = false;
static uint8_t last_on = 0xFF;  // v31: the last payload's on byte (an identical resend changes nothing)
static float rx = 0, ry = 0;  // carried fractions
// v32: the live view
static uint16_t dpi_now = 0;
static float g_now = 1.0f;
static bool moving = false;
static uint16_t ring[GW_CURVE_RING];
static uint16_t ring_n = 0;
static uint64_t samp_us = 0;
static uint32_t hist[GW_CURVE_BINS];
static uint32_t hist_ms = 0;
static uint8_t hist_resets = 0;

void gw_curve_set(bool want, uint8_t tau_ms, uint16_t dpi_fallback, uint8_t n, const uint16_t* sp, const uint16_t* g) {
    bool ok = (n >= 2) && (n <= GW_CURVE_N) && (tau_ms >= 1) && (tau_ms <= 200);
    for (uint8_t k = 0; ok && (k < n); k++) {
        ok = (k == 0) || (sp[k] > sp[k - 1]);
    }
    n_pts = 0;
    if (ok) {
        for (uint8_t k = 0; k < n; k++) {
            uint16_t v = g[k];
            v = (v < GW_CURVE_GMIN) ? GW_CURVE_GMIN : (v > GW_CURVE_GMAX) ? GW_CURVE_GMAX : v;
            pt_sp[k] = sp[k] / 10.0f;
            pt_g[k] = v / 1000.0f;
        }
        n_pts = n;
        tau = tau_ms;
        dpi_fb = dpi_fallback;
    }
    got = true;
    on = want && ok;
    spd = rx = ry = 0;
    have_t = moved = false;
}

void gw_curve_link(const uint8_t* d, uint8_t len) {
    uint8_t crc = (len > 1) ? gw_crc8(d + 1, len - 1) : 0;
    if (got && (len >= 1) && (crc == table_crc) && (d[0] == last_on)) {
        return;  // v31: the QT Py sent the same again (it resends while it can't confirm): keep the speed and the carry
    }
    last_on = (len >= 1) ? d[0] : 0xFF;
    uint16_t sp[GW_CURVE_N] = {}, g[GW_CURVE_N] = {};
    uint8_t n = (len >= 4) ? (len - 4) / 4 : 0;
    bool ok = (len >= 4) && ((len - 4) % 4 == 0) && (n <= GW_CURVE_N);
    for (uint8_t k = 0; ok && (k < n); k++) {
        sp[k] = d[4 + 4 * k] | d[5 + 4 * k] << 8;
        g[k] = d[6 + 4 * k] | d[7 + 4 * k] << 8;
    }
    gw_curve_set(ok && (d[0] != 0), ok ? d[1] : 0, ok ? (d[2] | d[3] << 8) : 0, ok ? n : 0, sp, g);
    table_crc = crc;
}

static float gain_at(float v) {
    if (v <= pt_sp[0]) {
        return pt_g[0];
    }
    for (uint8_t k = 1; k < n_pts; k++) {
        if (v < pt_sp[k]) {
            return pt_g[k - 1] + (pt_g[k] - pt_g[k - 1]) * (v - pt_sp[k - 1]) / (pt_sp[k] - pt_sp[k - 1]);
        }
    }
    return pt_g[n_pts - 1];
}

uint8_t gw_curve_bin(float v) {
    static float edge[GW_CURVE_BINS - 1];  // edge[k] = where bin k + 1 starts
    if (edge[0] == 0) {
        for (uint8_t k = 0; k < GW_CURVE_BINS - 1; k++) {
            edge[k] = 5.0f * powf(32.0f, k / 24.0f);
        }
    }
    uint8_t b = 0;
    while ((b < GW_CURVE_BINS - 1) && (v >= edge[b])) {
        b++;
    }
    return b;
}

static void sample(uint64_t now_us) {
    if (now_us - samp_us < GW_CURVE_SAMPLE_US) {
        return;
    }
    samp_us = (now_us - samp_us < 2 * GW_CURVE_SAMPLE_US) ? samp_us + GW_CURVE_SAMPLE_US : now_us;
    float v = spd * 10.0f + 0.5f;
    ring[ring_n % GW_CURVE_RING] = (v > 65535.0f) ? 65535 : (uint16_t) v;
    ring_n++;
}

// v32: the speed is tracked with the curve off too (the live view), but only an applying curve changes the motion
void gw_curve_filter(int32_t* x, int32_t* y, uint64_t now_us, uint16_t dpi) {
    if ((x == nullptr) || (y == nullptr)) {
        return;
    }
    if (dpi == 0) {
        dpi = dpi_fb;
    }
    dpi_now = dpi;
    g_now = 1.0f;
    moving = moved && (now_us - moved_us < GW_CURVE_GAP_US) && (spd >= GW_CURVE_STILL);
    if (dpi == 0) {
        spd = 0;  // v31: no DPI known: the speed can't be, so the motion passes unchanged (it froze the last gain)
        have_t = moved = moving = false;
        sample(now_us);
        return;
    }
    int32_t dx = *x, dy = *y;
    uint64_t gap = have_t ? (now_us - last_us) : 0;
    last_us = now_us;
    have_t = true;
    if ((dx == 0) && (dy == 0)) {
        if (gap > 0) {
            spd *= expf(-(gap / 1000.0f) / tau);  // a still frame: the speed decays
        }
        if (on) {
            g_now = gain_at(spd);
        }
        sample(now_us);
        return;
    }
    if (!moved || (now_us - moved_us >= GW_CURVE_GAP_US)) {
        spd = 0;  // the first motion after 50 ms without any starts from rest
    } else if (gap > 0) {
        float dt = gap / 1000.0f;  // ms
        float cm = sqrtf((float) dx * dx + (float) dy * dy) * 2.54f / dpi;
        spd += (cm / dt * 1000.0f - spd) * (1.0f - expf(-dt / tau));
    }
    moved = true;
    moved_us = now_us;
    moving = spd >= GW_CURVE_STILL;
    if (moving) {
        hist[gw_curve_bin(spd)]++;
        hist_ms++;
    }
    sample(now_us);
    if (!on) {
        return;
    }
    float g = gain_at(spd);
    g_now = g;
    // as in gw_dirgain.cc: an axis that didn't move stays 0, floor(x + 0.5) rounding, clamped to a 16-bit report
    if (dx != 0) {
        float fx = dx * g + rx;
        int32_t nx = (int32_t) floorf(fx + 0.5f);
        rx = fx - nx;
        *x = (nx > 32767) ? 32767 : (nx < -32767) ? -32767 : nx;
    }
    if (dy != 0) {
        float fy = dy * g + ry;
        int32_t ny = (int32_t) floorf(fy + 0.5f);
        ry = fy - ny;
        *y = (ny > 32767) ? 32767 : (ny < -32767) ? -32767 : ny;
    }
}

void gw_curve_status(uint8_t* s) {
    s[0] = (on ? 1 : 0) | (got ? 2 : 0);
    s[1] = table_crc;
}

static void put16(uint8_t* p, uint32_t v) {
    v = (v > 65535) ? 65535 : v;
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static void put32(uint8_t* p, uint32_t v) {
    for (int k = 0; k < 4; k++) {
        p[k] = (v >> (8 * k)) & 0xFF;
    }
}

void gw_curve_live(uint8_t* b) {
    memset(b, 0, GW_LINK_LIVE_LEN);
    b[0] = 'L';
    b[1] = (on ? 1 : 0) | (dpi_now ? 2 : 0) | (moving ? 4 : 0);
    b[2] = ring_n & 0xFF;
    b[3] = ring_n >> 8;
    put16(b + 4, dpi_now);
    put16(b + 6, (uint32_t) (g_now * 1000.0f + 0.5f));
    put16(b + 8, (uint32_t) (spd * 10.0f + 0.5f));
    uint8_t n = (ring_n < GW_CURVE_RING) ? ring_n : GW_CURVE_RING;
    for (uint8_t k = 0; k < n; k++) {
        put16(b + 10 + 2 * k, ring[(uint16_t) (ring_n - 1 - k) % GW_CURVE_RING]);
    }
    b[GW_LINK_LIVE_LEN - 1] = gw_crc8(b, GW_LINK_LIVE_LEN - 1);
}

void gw_curve_hist(uint8_t* b) {
    memset(b, 0, GW_LINK_HIST_LEN);
    b[0] = 'H';
    b[1] = GW_CURVE_BINS;
    b[2] = hist_resets;
    put32(b + 4, hist_ms);
    for (uint8_t k = 0; k < GW_CURVE_BINS; k++) {
        put32(b + 8 + 4 * k, hist[k]);
    }
    b[GW_LINK_HIST_LEN - 1] = gw_crc8(b, GW_LINK_HIST_LEN - 1);
}

void gw_curve_hist_reset() {
    memset(hist, 0, sizeof(hist));
    hist_ms = 0;
    hist_resets++;
}

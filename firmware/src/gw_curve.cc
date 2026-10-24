// v30: a speed curve, the XIM's ballistic curve done here instead. The QT Py sends it (LINK_SET_CURVE) after every
// power-on and when it changes, "off" on hosts it shouldn't apply to; nothing is kept in the Feather's flash.
//
// The speed is the motion's length per time in cm/s (counts through the mouse's DPI, so a DPI stage change needs no
// new table), smoothed exponentially over tau_ms by the frames' own times, and back to rest after a 50 ms gap. Like
// the direction gains, only the size of the motion changes, never its direction, and the fractions left after
// rounding carry over to the next frame.

#include "gw_curve.h"

#include <math.h>

#include "gw_link.h"

#define GW_CURVE_GAP_US 50000

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
static float rx = 0, ry = 0;  // carried fractions

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
    have_t = false;
}

void gw_curve_link(const uint8_t* d, uint8_t len) {
    uint16_t sp[GW_CURVE_N] = {}, g[GW_CURVE_N] = {};
    uint8_t n = (len >= 4) ? (len - 4) / 4 : 0;
    bool ok = (len >= 4) && ((len - 4) % 4 == 0) && (n <= GW_CURVE_N);
    for (uint8_t k = 0; ok && (k < n); k++) {
        sp[k] = d[4 + 4 * k] | d[5 + 4 * k] << 8;
        g[k] = d[6 + 4 * k] | d[7 + 4 * k] << 8;
    }
    gw_curve_set(ok && (d[0] != 0), ok ? d[1] : 0, ok ? (d[2] | d[3] << 8) : 0, ok ? n : 0, sp, g);
    table_crc = (len > 1) ? gw_crc8(d + 1, len - 1) : 0;
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

void gw_curve_filter(int32_t* x, int32_t* y, uint64_t now_us, uint16_t dpi) {
    if (!on || (x == nullptr) || (y == nullptr)) {
        return;
    }
    if (dpi == 0) {
        dpi = dpi_fb;
    }
    int32_t dx = *x, dy = *y;
    uint64_t gap = have_t ? (now_us - last_us) : GW_CURVE_GAP_US;
    last_us = now_us;
    have_t = true;
    if (gap >= GW_CURVE_GAP_US) {
        spd = 0;  // after a pause it starts from rest
    } else if ((gap > 0) && (dpi > 0)) {
        float dt = gap / 1000.0f;  // ms
        float cm = sqrtf((float) dx * dx + (float) dy * dy) * 2.54f / dpi;
        spd += (cm / dt * 1000.0f - spd) * (1.0f - expf(-dt / tau));
    }
    if ((dx == 0) && (dy == 0)) {
        return;
    }
    float g = gain_at(spd);
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

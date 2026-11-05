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
#include "pico.h"  // gw v53: __not_in_flash_func

#ifndef __not_in_flash_func
#define __not_in_flash_func(f) f  // gw v53: the host test's stand-in pico.h has none
#endif

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
static uint8_t last_p[4 + 4 * GW_CURVE_N], last_len = 0xFF;  // v31: an identical resend changes nothing; v42: the whole payload (a crc8 match let 1 change in 256 through as "the same")
static int64_t rxq = 0, ryq = 0;  // carried fractions (gw v54 (e): Q16 counts; gw v57: Q32, exact)
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
// v39: the slow-down gain
static bool dc_on = false, dc_got = false;
static uint8_t dc_n = 0, dc_crc = 0;
static uint8_t dc_last_p[1 + 4 * GW_DECEL_N], dc_last_len = 0xFF;  // v42: the last payload, whole (as last_p); v47: room for 4-byte points
static float dc_r[GW_DECEL_N], dc_g[GW_DECEL_N];  // share of the peak (0-1), gain
static float dc_pk = 0, dc_tr = 0;                 // cm/s: the stroke's peak, the slowest since it
static float dc_gn = 1.0f;                         // the slow-down gain now (eased)
// gw v53: the per-frame float work cut to what changes (soft-float on RP2040: every op is a call), all exact: the same
// inputs give the same floats, so only repeated work is skipped. The table segments' widths and rises, as gain_at and
// decel_at took them every frame (the same subtractions, done once per table)
static float pt_ds[GW_CURVE_N], pt_dg[GW_CURVE_N];          // [k] = point k minus point k - 1
static float pt_sl[GW_CURVE_N], dc_sl[GW_DECEL_N + 1];      // gw v54 (c): the segments' slopes (rise / width)
static float dc_dx[GW_DECEL_N + 1], dc_dg[GW_DECEL_N + 1];  // the same, with the implied (1.0, 1.0) as point dc_n
// gw v53: dt (ms), e^(-dt/tau) and 1 - that by the frame gap (frames come 1 ms apart, +-1 us: 4 slots by the low bits);
// a gap of 0 is never looked up, so 0 = empty. Emptied when tau changes.
#define GW_CURVE_EC 4
static uint32_t ec_gap[GW_CURVE_EC];
static float ec_dt[GW_CURVE_EC], ec_e[GW_CURVE_EC], ec_a[GW_CURVE_EC];
static float ec_ka[GW_CURVE_EC];  // gw v54 (a2): a * 1000 / dt, by the gap
// gw v53: the motion's length (cm) by |dx|, |dy| (smaller first) and the DPI: (float) -d is -(float) d, and the sum
// doesn't care which is first, so these give the same float. DPI 0 is never looked up, so 0 = empty.
#define GW_CURVE_CC 8
static uint32_t cc_lo[GW_CURVE_CC], cc_hi[GW_CURVE_CC];
static uint16_t cc_dpi[GW_CURVE_CC];
static float cc_cm[GW_CURVE_CC];

// gw v53: v >= e (v < e, v <= e) for an e that's positive and normal, by the bits: those order as the floats do, and a
// denormal or negative v (or -0) is below e whether or not the soft-float flushes it to 0. A NaN v takes the float
// compare. Never a call (a soft-float compare is one).
static inline __attribute__((always_inline)) int32_t fbits(float f) {
    int32_t b;
    memcpy(&b, &f, sizeof(b));
    return b;
}
static inline __attribute__((always_inline)) bool f_nan(int32_t b) {
    return (b & 0x7FFFFFFF) > 0x7F800000;
}
static inline __attribute__((always_inline)) bool ge_pn(float v, float e) {
    int32_t b = fbits(v);
    return f_nan(b) ? (v >= e) : (b >= fbits(e));
}
static inline __attribute__((always_inline)) bool lt_pn(float v, float e) {
    int32_t b = fbits(v);
    return f_nan(b) ? (v < e) : (b < fbits(e));
}
static inline __attribute__((always_inline)) bool le_pn(float v, float e) {
    int32_t b = fbits(v);
    return f_nan(b) ? (v <= e) : (b <= fbits(e));
}

static float dc_tpk = -1.0f;  // gw v54 (d): the peak decel_spd's speeds are for (-1 = none)
static void __not_in_flash_func(dc_stroke)() {
    dc_pk = dc_tr = 0;
    dc_gn = 1.0f;
    dc_tpk = -1.0f;  // gw v54 (d): a new table or stroke: the peak's speeds are stale
}

void gw_curve_set(bool want, uint8_t tau_ms, uint16_t dpi_fallback, uint8_t n, const uint16_t* sp, const uint16_t* g) {
    bool ok = (n >= 2) && (n <= GW_CURVE_N) && (tau_ms >= 1) && (tau_ms <= 200);
    for (uint8_t k = 0; ok && (k < n); k++) {
        ok = (k == 0) || (sp[k] > sp[k - 1]);
    }
    // gw v57: only a new smoothing (tau), the curve turning on or off, or a refused table starts the motion over. New
    // points alone (the QT Py's live editor sends the curve 250 ms after each edit) keep the speed, the carried
    // fractions and the slow-down stroke, so the next frame takes the new table's gain at the same speed; v30-v56 reset
    // them all: mid-motion the gain dropped to the first point's and ramped back over tau
    bool tau_new = ok && ((float) tau_ms != tau);
    bool fresh = !got || !ok || tau_new || ((want && ok) != on);
    n_pts = 0;
    if (ok) {
        for (uint8_t k = 0; k < n; k++) {
            uint16_t v = g[k];
            v = (v < GW_CURVE_GMIN) ? GW_CURVE_GMIN : (v > GW_CURVE_GMAX) ? GW_CURVE_GMAX : v;
            pt_sp[k] = sp[k] / 10.0f;
            pt_g[k] = v / 1000.0f;
        }
        for (uint8_t k = 1; k < n; k++) {  // gw v53: gain_at's subtractions, once
            pt_ds[k] = pt_sp[k] - pt_sp[k - 1];
            pt_dg[k] = pt_g[k] - pt_g[k - 1];
            pt_sl[k] = pt_dg[k] / pt_ds[k];
        }
        n_pts = n;
        if (tau_new) {
            tau = tau_ms;
            memset(ec_gap, 0, sizeof(ec_gap));  // gw v53: e^(-dt/tau) is by tau too (gw v57: kept while tau is)
        }
        dpi_fb = dpi_fallback;
    }
    got = true;
    on = want && ok;
    if (fresh) {
        spd = 0;
        rxq = ryq = 0;
        have_t = moved = false;
        dc_stroke();
    }
}

void gw_curve_link(const uint8_t* d, uint8_t len) {
    uint8_t crc = (len > 1) ? gw_crc8(d + 1, len - 1) : 0;
    if (got && (len == last_len) && !memcmp(d, last_p, len)) {
        return;  // v31: the QT Py sent the same again (it resends while it can't confirm): keep the speed and the carry
    }
    last_len = (len <= sizeof(last_p)) ? len : 0xFF;
    if (last_len != 0xFF) {
        memcpy(last_p, d, len);
    }
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

void gw_curve_decel_set(bool want, uint8_t n, const uint16_t* p10, const uint16_t* g) {
    bool ok = (n >= 1) && (n <= GW_DECEL_N);
    for (uint8_t k = 0; ok && (k < n); k++) {  // v47: shares in 0.1 % (1.0-99.0 %)
        ok = (p10[k] >= 10) && (p10[k] <= 990) && ((k == 0) || (p10[k] > p10[k - 1]));
    }
    dc_n = 0;
    if (ok) {
        for (uint8_t k = 0; k < n; k++) {
            uint16_t v = g[k];
            v = (v < GW_CURVE_GMIN) ? GW_CURVE_GMIN : (v > GW_CURVE_GMAX) ? GW_CURVE_GMAX : v;
            dc_r[k] = p10[k] / 1000.0f;
            dc_g[k] = v / 1000.0f;
        }
        for (uint8_t k = 1; k <= n; k++) {  // gw v53: decel_at's subtractions, once (point n = the implied 1.0, 1.0)
            dc_dx[k] = ((k < n) ? dc_r[k] : 1.0f) - dc_r[k - 1];
            dc_dg[k] = ((k < n) ? dc_g[k] : 1.0f) - dc_g[k - 1];
            dc_sl[k] = dc_dg[k] / dc_dx[k];
        }
        dc_n = n;
    }
    dc_got = true;
    // gw v57: new points alone (on stays on, or off stays off) keep the stroke's peak, its slowest and the eased gain,
    // so the gain eases into the new table; only the peak's thresholds (decel_spd's, from the old shares) are redone.
    // Turning it on or off starts a new stroke, as before (v39-v56 did on every change: mid-stroke, a step to 1.0)
    if (dc_on == (want && ok)) {
        dc_tpk = -1.0f;
    } else {
        dc_on = want && ok;
        dc_stroke();
    }
}

void gw_curve_decel_link(const uint8_t* d, uint8_t len) {
    uint8_t crc = (len > 1) ? gw_crc8(d + 1, len - 1) : 0;
    if (dc_got && (len == dc_last_len) && !memcmp(d, dc_last_p, len)) {
        return;  // the same again (the QT Py resends while it can't confirm): keep the stroke
    }
    dc_last_len = (len <= sizeof(dc_last_p)) ? len : 0xFF;
    if (dc_last_len != 0xFF) {
        memcpy(dc_last_p, d, len);
    }
    // v47: byte 0 bit 0 = on, bit 1 = 4-byte points (share uint16 in 0.1 %); without it the old 3-byte points (share
    // in whole %, taken as share * 10), so a QT Py from before v47 still works
    uint16_t p10[GW_DECEL_N] = {}, g[GW_DECEL_N] = {};
    uint8_t w = ((len >= 1) && (d[0] & 2)) ? 4 : 3;
    uint8_t n = (len >= 1) ? (len - 1) / w : 0;
    bool ok = (len >= 1) && ((len - 1) % w == 0) && (n <= GW_DECEL_N);
    for (uint8_t k = 0; ok && (k < n); k++) {
        const uint8_t* q = d + 1 + w * k;
        p10[k] = (w == 4) ? (uint16_t) (q[0] | q[1] << 8) : (uint16_t) (q[0] * 10);
        g[k] = q[w - 2] | q[w - 1] << 8;
    }
    gw_curve_decel_set(ok && (d[0] & 1), ok ? n : 0, p10, g);
    dc_crc = crc;
}

// gw v53: this and the rest of the per-frame path run from RAM (firmware/src's code is in flash, through the XIP cache)
// gw v54 (d): the shares' speeds for this stroke's peak (dc_tk[k] = peak * share k, dc_tk[dc_n] = the peak), worked out on
// the first slow-down frame after the peak changed; a segment's slope per cm/s (rise / (width * peak)) on the first
// frame in it. dc_tpk (above dc_stroke) = the peak they are for (-1 = none)
static float dc_tk[GW_DECEL_N + 1], dc_ts[GW_DECEL_N + 1];
static uint8_t dc_tok = 0;
static float __not_in_flash_func(decel_spd)(float v) {  // v = the speed, below the stroke's peak (both 0.1 cm/s or more)
    if (fbits(dc_tpk) != fbits(dc_pk)) {
        for (uint8_t k = 0; k < dc_n; k++) {
            dc_tk[k] = dc_pk * dc_r[k];
        }
        dc_tk[dc_n] = dc_pk;
        dc_tok = 0;
        dc_tpk = dc_pk;
    }
    if (le_pn(v, dc_tk[0])) {  // all positive and normal: by the bits
        return dc_g[0];
    }
    for (uint8_t k = 1; k <= dc_n; k++) {
        if (lt_pn(v, dc_tk[k])) {
            if (!(dc_tok & (1u << k))) {
                dc_ts[k] = dc_dg[k] / (dc_dx[k] * dc_pk);
                dc_tok |= (uint8_t) (1u << k);
            }
            return dc_g[k - 1] + dc_ts[k] * (v - dc_tk[k - 1]);
        }
    }
    return 1.0f;
}

// v39: the stroke's peak and the slowest speed since it. A rise of 1.25x and 2 cm/s over that slowest is speeding up
// again: a new peak, so only slowing down gets the gain. It eases toward its value by the speed's own smoothing (a =
// 1 - e^(-dt/tau), 0 for the first frame from rest), so neither the switch at the peak nor a new rise is a step.
static float __not_in_flash_func(decel_step)(float a) {
    if (lt_pn(spd, GW_CURVE_STILL)) {  // v42: still: the stroke is over (at 40,000 DPI a resting mouse's stray counts never
        dc_stroke();             // leave the 50 ms gap, so slow aim after a rest kept the last flick's peak and its lowest gain)
        return dc_gn;
    }
    if (spd >= dc_pk) {
        dc_pk = dc_tr = spd;
    } else if (spd < dc_tr) {
        dc_tr = spd;
    } else if ((spd > dc_tr * 1.25f) && (spd > dc_tr + 2.0f)) {
        dc_pk = dc_tr = spd;
    }
    // gw v54 (d): spd is the peak (just set) = share 1.0 = gain 1.0, as v53's decel_at(1.0) gives; else below the peak
    float want = (dc_on && lt_pn(spd, dc_pk)) ? decel_spd(spd) : 1.0f;
    dc_gn += (want - dc_gn) * a;
    return dc_gn;
}

static float __not_in_flash_func(gain_at)(float v) {
    if (v <= pt_sp[0]) {  // the first point may be 0 cm/s: a float compare (RP2040's flushes a denormal v to 0)
        return pt_g[0];
    }
    for (uint8_t k = 1; k < n_pts; k++) {
        if (lt_pn(v, pt_sp[k])) {  // gw v53: points after the first are 0.1 cm/s or more, so by the bits
            return pt_g[k - 1] + pt_sl[k] * (v - pt_sp[k - 1]);  // gw v54 (c): the segment's slope from curve_set (v53 divided by its width here)
        }
    }
    return pt_g[n_pts - 1];
}

uint8_t __not_in_flash_func(gw_curve_bin)(float v) {
    // edge[k] = where bin k + 1 starts: 5 * 32^(k/24) cm/s. gw v61: written out (double-precision values rounded to float);
    // computed on the first call they were 25 soft-float powf calls, about 490 us in the first moving frame after each start.
    // Not const: .data (RAM), as the rest of the per-frame path (a const table would be read from flash)
    static float edge[GW_CURVE_BINS - 1] = { 5.0f, 5.7767634f, 6.674199f, 7.7110543f, 8.908987f, 10.293022f, 11.892071f, 13.739536f,
                                                   15.87401f, 18.34008f, 21.189262f, 24.481071f, 28.284271f, 32.67831f, 37.754974f, 43.620308f,
                                                   50.396843f, 58.226128f, 67.27171f, 77.72256f, 89.79697f, 103.74716f, 119.86456f, 138.48586f, 160.0f };
    static_assert(sizeof(edge) / sizeof(edge[0]) == 25, "GW_CURVE_BINS - 1 edges");
    uint8_t b = 0;
    int32_t vb = fbits(v);
    if (f_nan(vb)) {
        while ((b < GW_CURVE_BINS - 1) && (v >= edge[b])) {
            b++;
        }
        return b;
    }
    while ((b < GW_CURVE_BINS - 1) && (vb >= fbits(edge[b]))) {  // gw v53: the edges are 5-139 cm/s: by the bits (ge_pn)
        b++;
    }
    return b;
}

static void __not_in_flash_func(sample)(uint64_t now_us) {
    if (now_us - samp_us < GW_CURVE_SAMPLE_US) {
        return;
    }
    samp_us = (now_us - samp_us < 2 * GW_CURVE_SAMPLE_US) ? samp_us + GW_CURVE_SAMPLE_US : now_us;
    float v = spd * 10.0f + 0.5f;
    ring[ring_n % GW_CURVE_RING] = (v > 65535.0f) ? 65535 : (uint16_t) v;
    ring_n++;
}

// gw v53: dt = gap / 1000 ms and e = e^(-dt/tau) as the frame computed them, from the cache when the gap was seen
static inline __attribute__((always_inline)) uint32_t decay_of(uint64_t gap) {
    uint32_t k = (uint32_t) gap & (GW_CURVE_EC - 1);
    if (((gap >> 32) == 0) && (ec_gap[k] == (uint32_t) gap)) {
        return k;
    }
    k = ((gap >> 32) == 0) ? k : 0;  // a gap past 32 bits (71 minutes) isn't kept: slot 0, marked empty
    ec_dt[k] = gap / 1000.0f;
    ec_e[k] = expf(-ec_dt[k] / tau);
    ec_a[k] = 1.0f - ec_e[k];
    ec_ka[k] = ec_a[k] * (1000.0f / ec_dt[k]);
    ec_gap[k] = ((gap >> 32) == 0) ? (uint32_t) gap : 0;
    return k;
}

// gw v54 (b): 2.54 / dpi, by the DPI
static uint16_t kd_dpi = 0;
static float kd_cm = 0;
static inline __attribute__((always_inline)) float cm_per(uint16_t dpi) {
    if (dpi != kd_dpi) {
        kd_cm = 2.54f / dpi;
        kd_dpi = dpi;
    }
    return kd_cm;
}

// gw v53: sqrtf(dx^2 + dy^2) * 2.54 / dpi as the frame computed it, from the cache when the counts and DPI were seen
static inline __attribute__((always_inline)) float cm_of(int32_t dx, int32_t dy, uint16_t dpi) {
    uint32_t ax = (dx < 0) ? 0u - (uint32_t) dx : (uint32_t) dx, ay = (dy < 0) ? 0u - (uint32_t) dy : (uint32_t) dy;
    uint32_t lo = (ax < ay) ? ax : ay, hi = (ax < ay) ? ay : ax;
    uint32_t k = (hi * 5 + lo) & (GW_CURVE_CC - 1);
    if ((cc_dpi[k] != dpi) || (cc_lo[k] != lo) || (cc_hi[k] != hi)) {
        cc_cm[k] = sqrtf((float) dx * dx + (float) dy * dy) * cm_per(dpi);  // gw v54 (b)
        cc_lo[k] = lo;
        cc_hi[k] = hi;
        cc_dpi[k] = dpi;
    }
    return cc_cm[k];
}

// gw v57: f * 2^32 rounded toward -infinity, in 64 bits: the RP2040's float2fix64(f, 32) (pico/float.h documents the
// rounding), floor on the host (the same: a float times 2^32 is exact in a double). gwtest curve_round's soft-float
// counter (sfcount.h, GW_HAVE_ROMF) brings its own
#if PICO_RP2040
#include "pico/float.h"
static inline int64_t gw_f2fix32(float f) {
    return float2fix64(f, 32);
}
#elif !defined(GW_HAVE_ROMF)
static inline int64_t gw_f2fix32(float f) {
    return (int64_t) floor(ldexp((double) f, 32));
}
#endif
// gw v57: d * g + the carried fraction, rounded (floor(x + 0.5)), the new fraction left in *q (Q32 counts, -0.5 to
// under 0.5). d * g is a float of 2^-9 or more (|d| >= 1, the gain >= 0.25 x 0.25), so p * 2^32 is a whole number: the
// conversion is exact and the rest is integer adds, and the outputs' sum stays within half a count of the exact sum
// of the d * g, with no drift either way (gwtest curve_bias). v54-v56's Q16 rounded d * g to 2^-16 with exact ties up
// (from 64 to 128 counts a float's last bit is 2^-17, so half of all frames there were a tie): +3 counts per million
// frames at 50 counts a frame, toward +x/+y
static inline __attribute__((always_inline)) int32_t round_q32(int32_t d, float g, int64_t* q) {
    float p = d * g;
    int32_t b = fbits(p);
    if ((b & 0x7FFFFFFF) < 0x4F000000) {  // |p| < 2^31 (and not NaN): p * 2^32 plus the carry fits 63 bits
        int64_t acc = gw_f2fix32(p) + *q;
        int64_t n = (acc + 0x80000000LL) >> 32;  // arithmetic shift: floor
        *q = acc - n * 0x100000000LL;
        return (int32_t) n;
    }
    // 2^31 counts or more (past any report: the caller clamps to 16 bits) or NaN: saturated, the carry kept as it was
    return f_nan(b) ? 0 : (b < 0) ? -2147483647 : 2147483647;
}

// v32: the speed is tracked with the curve off too (the live view), but only an applying curve changes the motion
void __not_in_flash_func(gw_curve_filter)(int32_t* x, int32_t* y, uint64_t now_us, uint16_t dpi) {
    if ((x == nullptr) || (y == nullptr)) {
        return;
    }
    if (dpi == 0) {
        dpi = dpi_fb;
    }
    dpi_now = dpi;
    g_now = 1.0f;
    moving = moved && (now_us - moved_us < GW_CURVE_GAP_US) && ge_pn(spd, GW_CURVE_STILL);  // gw v53: by the bits
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
            spd *= ec_e[decay_of(gap)];  // a still frame: the speed decays (gw v53: e^(-dt/tau) cached by the gap)
        }
        if (on) {
            g_now = gain_at(spd) * dc_gn;
        }
        sample(now_us);
        return;
    }
    float a = 0;  // how far this frame moves the smoothed speed (and the slow-down gain) toward its own
    if (!moved || (now_us - moved_us >= GW_CURVE_GAP_US)) {
        spd = 0;  // the first motion after 50 ms without any starts from rest
        dc_stroke();  // v39: and a new stroke
    } else if (gap > 0) {
        uint32_t k = decay_of(gap);  // gw v53: dt (ms) and a = 1 - e^(-dt/tau), cached by the gap
        float cm = cm_of(dx, dy, dpi);  // gw v53: cached by |dx|, |dy|, DPI
        a = ec_a[k];
        spd = spd * ec_e[k] + cm * ec_ka[k];  // gw v54 (a2): the same smoothing in 2 multiplies and an add
    }
    moved = true;
    moved_us = now_us;
    moving = ge_pn(spd, GW_CURVE_STILL);
    if (moving) {
        hist[gw_curve_bin(spd)]++;
        hist_ms++;
    }
    sample(now_us);
    if (!on) {
        return;
    }
    float g = gain_at(spd) * decel_step(a);
    g_now = g;
    // as in gw_dirgain.cc: an axis that didn't move stays 0, floor(x + 0.5) rounding, clamped to a 16-bit report
    if (dx != 0) {
        int32_t nx = round_q32(dx, g, &rxq);  // gw v54 (e), v57: Q32
        *x = (nx > 32767) ? 32767 : (nx < -32767) ? -32767 : nx;
    }
    if (dy != 0) {
        int32_t ny = round_q32(dy, g, &ryq);  // gw v54 (e), v57: Q32
        *y = (ny > 32767) ? 32767 : (ny < -32767) ? -32767 : ny;
    }
}

bool gw_curve_moving(void) {
    return moving;
}

bool gw_curve_speed_known(void) {
    return dpi_now != 0;
}

void gw_curve_status(uint8_t* s) {
    s[0] = (on ? 1 : 0) | (got ? 2 : 0) | (dc_on ? 4 : 0) | (dc_got ? 8 : 0);
    s[1] = table_crc;
    s[2] = dc_crc;  // v39
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
    b[1] = (on ? 1 : 0) | (dpi_now ? 2 : 0) | (moving ? 4 : 0) | ((on && dc_on) ? 8 : 0);
    b[2] = ring_n & 0xFF;
    b[3] = ring_n >> 8;
    put16(b + 4, dpi_now);
    put16(b + 6, (uint32_t) (g_now * 1000.0f + 0.5f));
    put16(b + 8, (uint32_t) (spd * 10.0f + 0.5f));
    uint8_t n = (ring_n < GW_CURVE_RING) ? ring_n : GW_CURVE_RING;
    for (uint8_t k = 0; k < n; k++) {
        put16(b + 10 + 2 * k, ring[(uint16_t) (ring_n - 1 - k) % GW_CURVE_RING]);
    }
    put16(b + 74, (uint32_t) ((on ? dc_gn : 1.0f) * 1000.0f + 0.5f));  // v39
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

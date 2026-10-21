// v24: sensitivity per direction of movement. The QT Py sends the table (LINK_SET_DIRGAIN) after every power-on and
// when it changes, "off" on hosts it shouldn't apply to; nothing is kept in the Feather's flash.
//
// Which gain applies comes from a smoothed motion vector (each frame's X/Y added to the previous vector times
// GW_DIRGAIN_DECAY, about 3 ms at 1 ms frames), not from the frame alone: at high polling a frame can hold (1, 0) or
// (1, 1), and per-frame angles would make the gain jump around. Only the size of the motion changes, never its
// direction. The fractions left after rounding carry over to the next frame, so slow movements lose no counts.

#include "gw_dirgain.h"

#include <math.h>

#include "gw_link.h"

#define GW_DIRGAIN_DECAY 0.7f

static bool on = false;
static bool got = false;
static uint8_t table_crc = 0;
static float gain[GW_DIRGAIN_N];
static float vx = 0, vy = 0;  // smoothed motion
static float rx = 0, ry = 0;  // carried fractions

void gw_dirgain_set(bool want, const uint16_t* gains) {
    uint8_t raw[2 * GW_DIRGAIN_N];
    bool flat = true;
    for (int k = 0; k < GW_DIRGAIN_N; k++) {
        uint16_t g = gains[k];
        raw[2 * k] = g & 0xFF;
        raw[2 * k + 1] = g >> 8;
        if (g < GW_DIRGAIN_MIN) {
            g = GW_DIRGAIN_MIN;
        }
        if (g > GW_DIRGAIN_MAX) {
            g = GW_DIRGAIN_MAX;
        }
        if (g != 1000) {
            flat = false;
        }
        gain[k] = g / 1000.0f;
    }
    table_crc = gw_crc8(raw, sizeof(raw));
    got = true;
    on = want && !flat;
    vx = vy = rx = ry = 0;
}

void gw_dirgain_filter(int32_t* x, int32_t* y) {
    if (!on || (x == nullptr) || (y == nullptr)) {
        return;
    }
    int32_t dx = *x, dy = *y;
    if ((dx * vx + dy * vy) < 0) {
        vx = vy = 0;  // turned more than 90° (a flick back): the old direction mustn't carry into the new one
    }
    vx = vx * GW_DIRGAIN_DECAY + dx;
    vy = vy * GW_DIRGAIN_DECAY + dy;
    if ((dx == 0) && (dy == 0)) {
        return;
    }
    // screen y grows downwards, so atan2 counts clockwise on screen from "right"
    float a = atan2f(vy, vx);
    if (a < 0) {
        a += 6.2831853f;
    }
    float s = a * (GW_DIRGAIN_N / 6.2831853f);
    int k = (int) s;
    float f = s - k;
    k %= GW_DIRGAIN_N;
    float g = gain[k] * (1.0f - f) + gain[(k + 1) % GW_DIRGAIN_N] * f;
    // v25: an axis that didn't move stays 0 (its carried fraction waits for its next motion: lroundf on an exact
    // -0.5/+0.5 carry flipped it -1/+1 every frame, a 1-count shiver), floor(x + 0.5) rounding so a carry of -0.5 is
    // stable, and the result clamped to what a 16-bit report holds (it would wrap and turn the motion around)
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

void gw_dirgain_status(uint8_t* s) {
    s[0] = (on ? 1 : 0) | (got ? 2 : 0);
    s[1] = table_crc;
}

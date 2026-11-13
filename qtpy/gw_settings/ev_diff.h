// fw111: which entries of the Feather's recent-actions list are new since the previous copy of it.
// The list (block bytes 192-255, from 'E'): [1] count (at most 7 since Feather v58, 8 allowed), then newest first, 6 bytes
// each: age in whole s (uint16, the Feather's own seconds counter), source, buttons, action, value. A later copy is the
// earlier one shifted down by the new actions at its head, each old entry the same apart from its age, which grew by the
// whole seconds the Feather counted in between: never less, at most the time between the reads + 1.
#pragma once
#include <stdint.h>
#include <string.h>

// how many entries at the head of n (this copy) are new since o (the previous copy, read el_ms earlier)
static inline uint8_t ev_new_count(const uint8_t* o, const uint8_t* n, uint32_t el_ms) {
    uint8_t on = (o[1] > 8) ? 8 : o[1], nn = (n[1] > 8) ? 8 : n[1];
    // the Feather's seconds counter moved by the whole seconds between the reads, rounded either way (100 ms of slack for
    // when in each read it built the list), and by the same amount for every old entry
    uint32_t lo = (el_ms > 100) ? (el_ms - 100) / 1000 : 0, hi = (el_ms + 100) / 1000 + 1;
    // until the list is full, every old entry is still in it: the extra ones are the new ones (this also settles two
    // identical actions in the same second, which the ages can't tell apart)
    for (uint8_t k = (nn > on) ? nn - on : 0; k < nn; k++) {  // k new ones, then the old list from its head
        uint8_t m = ((uint8_t) (nn - k) < on) ? (uint8_t) (nn - k) : on;
        bool ok = (m > 0);
        uint32_t d0 = 0xFFFFFFFF;
        for (uint8_t j = 0; (j < m) && ok; j++) {
            const uint8_t* a = n + 2 + 6 * (k + j);
            const uint8_t* b = o + 2 + 6 * j;
            uint32_t aa = a[0] | a[1] << 8, ba = b[0] | b[1] << 8;
            // fw115 (review 11-12): a value the Feather didn't know yet (0x7F) is filled in later on the same entry: not a new one
            ok = !memcmp(a + 2, b + 2, 3) && ((a[5] == b[5]) || (b[5] == 0x7F)) && (aa >= ba);
            if (ok && (aa != 65535)) {
                uint32_t d = aa - ba;
                ok = (d >= lo) && (d <= hi) && ((d0 == 0xFFFFFFFF) || (d == d0));
                d0 = d;
            }
        }
        if (ok) {
            return k;
        }
    }
    return nn;  // nothing of the old list left in it: all new (more than the list holds, or the Feather started over)
}

// fw115: Feather v76 counts the actions it added since it started (block bytes 46-49): the new ones are the difference,
// exact even for identical actions in a full list. The count starts at a hash of the first action's time, so after the
// Feather started over it jumps far either way: more than the list holds = all new
static inline uint8_t ev_new_by_count(const uint8_t* o, const uint8_t* n) {
    uint8_t nn = (n[1] > 8) ? 8 : n[1];
    uint32_t a = o[46] | o[47] << 8 | o[48] << 16 | (uint32_t) o[49] << 24, b = n[46] | n[47] << 8 | n[48] << 16 | (uint32_t) n[49] << 24;
    uint32_t d = b - a;
    return (d > nn) ? nn : (uint8_t) d;
}

// the setting a recent action changed: 0 DPI stage (actions 1-7), 1 angle (8-10), 2 position (11-13), 0xFF other
static inline uint8_t ev_group(uint8_t action) {
    return (action >= 1 && action <= 7) ? 0 : (action >= 8 && action <= 10) ? 1 : (action >= 11 && action <= 13) ? 2 : 0xFF;
}

// a mouse chord's action (sources 1-5: tap, hold, double tap, wheel up, wheel down)
static inline bool ev_is_chord(const uint8_t* q) {
    return (q[2] >= 1) && (q[2] <= 5);
}

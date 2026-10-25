// v27: the measured turn behind the QT Py page's cm/360 card. The Feather adds a steady motion along X to each
// frame's input, after the direction gains (the overall level, not one direction's gain), as if the mouse had moved
// that far, so the XIM and the game turn the view with it. `total` counts at `per_s` counts a second, timed from the
// first frame rather than counted in frames, so the rate holds at any frame rate. The page narrows down the counts
// for one full turn by halving (short of the edge / past it). Stops by itself when done, after GW_TURN_MAX_US, or when the real
// mouse moves more than `hand` counts (a hand on the mouse takes over). RAM only.
//
// v28: count mode does the reverse: you turn by hand (in game, through the XIM) and it sums the real X that goes to the
// host, after the gains, from Start to Stop; the page divides by the full turns you made. While the count moves the
// change counter steps at most every 200 ms, so the QT Py reads the block for a live figure; it ends by itself after
// GW_TURN_COUNT_MAX_US so a forgotten count doesn't keep those reads going in game.

#include "gw_turn.h"

#define GW_TURN_MAX_US 120000000ull  // v29: 2 min (was 30 s: a 360 at 10 cm/s and ~265 cm/360 already took 27 s)
#define GW_TURN_FRAME_MAX 30000  // counts added in one frame at most (a 16-bit report holds 32767)
#define GW_TURN_COUNT_MAX_US 180000000ull
#define GW_TURN_BUMP_US 200000

static uint8_t state = GW_TURN_IDLE;
static uint8_t run_no = 0, changes = 0;
static int32_t total_abs = 0, sent_abs = 0;
static int32_t sign = 1;
static uint32_t rate = 0, hand_max = 0, hand_moved = 0;
static uint64_t t0 = 0;
static bool started = false;  // t0 is set at the first frame
static int64_t counted = 0, bumped = 0;  // v28: count mode's net X, and the value the last change step showed
static uint64_t bump_at = 0;

static void set_state(uint8_t s) {
    state = s;
    changes++;
}

void gw_turn_start(int32_t total, uint32_t per_s, uint32_t hand) {
    if ((total == 0) || (per_s == 0)) {
        gw_turn_stop();
        return;
    }
    sign = (total < 0) ? -1 : 1;
    total_abs = (total < 0) ? ((total == INT32_MIN) ? INT32_MAX : -total) : total;
    rate = per_s;
    hand_max = hand;
    hand_moved = 0;
    sent_abs = 0;
    started = false;
    run_no++;
    set_state(GW_TURN_RUN);
}

void gw_turn_stop(void) {
    if (state == GW_TURN_RUN) {
        set_state(GW_TURN_STOPPED);
    } else if (state == GW_TURN_COUNT) {
        set_state(GW_TURN_COUNTED);
    }
}

static bool count_after = false;  // v34

void gw_turn_count(bool after_curve) {
    count_after = after_curve;
    counted = bumped = 0;
    started = false;
    run_no++;
    set_state(GW_TURN_COUNT);
}

static void tally(int32_t x, uint64_t now_us) {
    if (!started) {
        started = true;
        t0 = bump_at = now_us;
    }
    counted += x;
    if (now_us - t0 > GW_TURN_COUNT_MAX_US) {
        set_state(GW_TURN_COUNTED);
    } else if ((counted != bumped) && (now_us - bump_at >= GW_TURN_BUMP_US)) {
        bumped = counted;
        bump_at = now_us;
        changes++;  // a live figure for the page
    }
}

void gw_turn_count_out(const int32_t* x, uint64_t now_us) {
    if ((state == GW_TURN_COUNT) && count_after && (x != nullptr)) {
        tally(*x, now_us);
    }
}

void gw_turn_filter(int32_t* x, int32_t* y, uint64_t now_us) {
    if (x == nullptr) {
        return;
    }
    if (state == GW_TURN_COUNT) {
        if (!count_after) {
            tally(*x, now_us);
        }
        return;
    }
    if (state != GW_TURN_RUN) {
        return;
    }
    if (!started) {
        started = true;
        t0 = now_us;
    }
    uint32_t real = (uint32_t) ((*x < 0) ? -*x : *x) + (uint32_t) ((y == nullptr) ? 0 : (*y < 0) ? -*y : *y);
    hand_moved += real;
    if ((hand_max != 0) && (hand_moved > hand_max)) {
        set_state(GW_TURN_HAND);
        return;
    }
    uint64_t el = now_us - t0;
    if (el > GW_TURN_MAX_US) {
        set_state(GW_TURN_TIMEOUT);
        return;
    }
    uint64_t due = (uint64_t) rate * el / 1000000ull;
    if (due > (uint64_t) total_abs) {
        due = (uint64_t) total_abs;
    }
    int64_t step = (int64_t) due - sent_abs;
    if (step > GW_TURN_FRAME_MAX) {
        step = GW_TURN_FRAME_MAX;
    }
    if (step > 0) {
        *x += sign * (int32_t) step;
        sent_abs += (int32_t) step;
    }
    if (sent_abs >= total_abs) {
        set_state(GW_TURN_DONE);
    }
}

uint8_t gw_turn_seq(void) {
    return changes;
}

void gw_turn_status(uint8_t* s) {
    int32_t v = sign * sent_abs;
    if ((state == GW_TURN_COUNT) || (state == GW_TURN_COUNTED)) {
        v = (counted > INT32_MAX) ? INT32_MAX : (counted < -INT32_MAX) ? -INT32_MAX : (int32_t) counted;
    }
    s[0] = state;
    s[1] = run_no;
    s[2] = (uint8_t) v;
    s[3] = (uint8_t) (v >> 8);
    s[4] = (uint8_t) (v >> 16);
    s[5] = (uint8_t) (v >> 24);
}

// randomized check of ev_new_count (by the entries' ages, any Feather) and ev_new_by_count (Feather v76's count of the
// actions it added) against a simulated Feather event list. Exits 1 on a miss it should never make.
#include "../gw_settings/ev_diff.h"  // g++ -O2 -std=c++17 evdiff_test.cc && ./a
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct Ev { uint32_t sec; uint8_t src, buttons, action; int8_t value; };

struct Feather {
    std::vector<Ev> ev;  // oldest first
    uint32_t t0_ms;      // phase of its seconds counter
    uint32_t added = 0;  // v76: actions added since it started (a value filled in later is not one)
    void build(uint32_t now_ms, uint8_t cap, uint8_t* b) {
        memset(b, 0, 64);
        b[0] = 'E';
        uint32_t secs = (now_ms + t0_ms) / 1000;
        uint8_t n = ev.size() > cap ? cap : (uint8_t) ev.size();
        b[1] = n;
        for (uint8_t k = 0; k < n; k++) {
            const Ev& e = ev[ev.size() - 1 - k];
            uint8_t* q = b + 2 + 6 * k;
            uint32_t age = secs - e.sec;
            if (age > 65535) age = 65535;
            q[0] = age & 0xFF; q[1] = age >> 8;
            q[2] = e.src; q[3] = e.buttons; q[4] = e.action; q[5] = (uint8_t) e.value;
        }
        if (cap == 7) {  // v58+: 7 entries (2-43); v76: the count at 46-49
            b[46] = added & 0xFF; b[47] = (added >> 8) & 0xFF; b[48] = (added >> 16) & 0xFF; b[49] = added >> 24;
        }
    }
};

int main() {
    std::mt19937 rng(12345);
    int fails = 0;
    // mode 0: like the mouse (angle / position steps, stages, page writes); 1, 2: many identical actions (2: quick ones);
    // 3: as 0, with angle / position values the Feather didn't know yet (0x7F) filled in on the same entry a little later
    for (int mode = 0; mode < 4; mode++) {
        long reads = 0, newtot = 0, overflow = 0, bad = 0, bad_kept = 0, xreads = 0, xbad = 0;
        for (int run = 0; run < 400; run++) {
            Feather f;
            f.t0_ms = rng() % 100000;
            uint8_t cap = (run % 4 == 0) ? 8 : 7;
            uint32_t now = 1000;
            int angle = -15, pos = 0;
            uint8_t ob[64], nb[64];
            f.build(now, cap, ob);
            uint32_t last = now;
            size_t seen = f.ev.size();
            long fill = -1;   // index in f.ev of the entry whose value comes later (mode 3)
            int fill_in = 0;  // reads until it does
            int8_t fill_v = 0;
            for (int step = 0; step < 3000; step++) {
                if ((fill >= 0) && (--fill_in <= 0)) {  // the Feather read the mouse: the value goes into the same entry
                    f.ev[fill].value = fill_v;
                    fill = -1;
                }
                // a burst of actions, then a read
                int burst = (rng() % 10 < 7) ? rng() % 3 : rng() % 12;
                for (int i = 0; i < burst; i++) {
                    now += (mode == 2) ? rng() % 30 : rng() % 400;
                    Ev e;
                    e.sec = (now + f.t0_ms) / 1000;
                    e.src = 1 + rng() % 9;
                    e.buttons = rng() % 4;
                    bool later = false;
                    if ((mode == 0) || (mode == 3)) {
                        int r = rng() % 4;
                        if (r == 0) { angle += (rng() % 2) ? 1 : -1; e.action = 8; e.value = angle; later = true; }
                        else if (r == 1) { pos += (rng() % 2) ? 1 : -1; e.action = 11; e.value = pos; later = true; }
                        else if (r == 2) { e.action = 1 + rng() % 7; e.value = rng() % 5; }
                        else { e.action = 9; angle = -13; e.value = angle; }
                    } else {
                        e.action = 8; e.buttons = 0; e.src = 1; e.value = rng() % 2;
                    }
                    if ((mode == 3) && later && (fill < 0) && (rng() % 3 == 0)) {
                        fill_v = e.value;
                        e.value = 0x7F;
                        fill = (long) f.ev.size();
                        fill_in = 1 + rng() % 3;
                    }
                    f.ev.push_back(e);
                    f.added++;
                    if (f.ev.size() > 16) {
                        f.ev.erase(f.ev.begin());
                        seen = seen ? seen - 1 : 0;
                        if (fill >= 0) fill--;  // its entry moved down (or left: overwritten before its value came)
                    }
                }
                now += (rng() % 10 < 8) ? 50 + rng() % 1500 : rng() % 20000;
                if (rng() % 500 == 0) {  // the Feather started over
                    f.ev.clear(); f.t0_ms = rng() % 100000; seen = 0; f.added = rng(); fill = -1;  // v76: its count starts at a hash of its first action's time
                    int k = rng() % 3;
                    for (int i = 0; i < k; i++) { Ev e{(now + f.t0_ms) / 1000, 1, 0, 8, (int8_t) i}; f.ev.push_back(e); f.added++; }
                }
                f.build(now, cap, nb);
                size_t truth = f.ev.size() - seen;
                if (truth > nb[1]) { truth = nb[1]; overflow++; }
                long elj = (long) (now - last) + (long) (rng() % 81) - 40; if (elj < 0) elj = 0;  // the read's own timing jitters
                uint8_t got = ev_new_count(ob, nb, (uint32_t) elj);
                reads++;
                newtot += truth;
                if (got != truth) {
                    bad++;
                    if (truth < nb[1]) bad_kept++;  // some of the old list was still in it: the ages could tell
                }
                if (cap == 7) {
                    uint8_t gx = ev_new_by_count(ob, nb);
                    xreads++;
                    if (gx != truth) {
                        xbad++;
                        if (xbad <= 5) printf("  exact: mode %d run %d step %d: truth %zu got %u\n", mode, run, step, truth, gx);
                    }
                }
                memcpy(ob, nb, 64);
                last = now;
                seen = f.ev.size();
            }
        }
        printf("mode %d: %ld reads, %ld new actions, %ld with more new than the list holds; by ages %ld wrong (%ld with old entries left); by count (v76) %ld of %ld wrong\n",
               mode, reads, newtot, overflow, bad, bad_kept, xbad, xreads);
        // by count: never wrong. By ages: never wrong while old entries are left, for varied actions (0) and filled-in
        // values (3); identical actions in a full list (1, 2) are what it can't tell apart: v76's count is for those
        if (xbad || (((mode == 0) || (mode == 3)) && bad_kept)) {
            printf("FAIL mode %d\n", mode);
            fails++;
        }
    }
    printf("evdiff: %d failed\n", fails);
    return fails ? 1 : 0;
}

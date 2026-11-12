// randomized check of ev_new_count against a simulated Feather event list
#include "../gw_settings/ev_diff.h"  // g++ -O2 -std=c++17 evdiff_test.cc && ./a
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct Ev { uint32_t sec; uint8_t src, buttons, action; int8_t value; };

struct Feather {
    std::vector<Ev> ev;  // oldest first
    uint32_t t0_ms;      // phase of its seconds counter
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
    }
};

int main() {
    std::mt19937 rng(12345);
    for (int mode = 0; mode < 3; mode++) {
        long reads = 0, bad = 0, newtot = 0, overflow = 0;
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
            for (int step = 0; step < 3000; step++) {
                // a burst of actions, then a read
                int burst = (rng() % 10 < 7) ? rng() % 3 : rng() % 12;
                for (int i = 0; i < burst; i++) {
                    now += (mode == 2) ? rng() % 30 : rng() % 400;
                    Ev e;
                    e.sec = (now + f.t0_ms) / 1000;
                    e.src = 1 + rng() % 9;
                    e.buttons = rng() % 4;
                    if (mode == 0) {  // like the mouse: angle and position steps, stages, page writes
                        int r = rng() % 4;
                        if (r == 0) { angle += (rng() % 2) ? 1 : -1; e.action = 8; e.value = angle; }
                        else if (r == 1) { pos += (rng() % 2) ? 1 : -1; e.action = 11; e.value = pos; }
                        else if (r == 2) { e.action = 1 + rng() % 7; e.value = rng() % 5; }
                        else { e.action = 9; angle = -13; e.value = angle; }
                    } else {  // many identical actions
                        e.action = 8; e.buttons = 0; e.src = 1; e.value = rng() % 2;
                    }
                    f.ev.push_back(e);
                    if (f.ev.size() > 16) { f.ev.erase(f.ev.begin()); seen = seen ? seen - 1 : 0; }
                }
                now += (rng() % 10 < 8) ? 50 + rng() % 1500 : rng() % 20000;
                if (rng() % 500 == 0) {  // the Feather started over
                    f.ev.clear(); f.t0_ms = rng() % 100000; seen = 0;
                    int k = rng() % 3;
                    for (int i = 0; i < k; i++) { Ev e{(now + f.t0_ms) / 1000, 1, 0, 8, (int8_t) i}; f.ev.push_back(e); }
                }
                f.build(now, cap, nb);
                size_t truth = f.ev.size() - seen;
                if (truth > nb[1]) { truth = nb[1]; overflow++; }
                long elj = (long) (now - last) + (long) (rng() % 81) - 40; if (elj < 0) elj = 0; uint8_t got = ev_new_count(ob, nb, (uint32_t) elj);  // the read's own timing jitters
                reads++;
                newtot += truth;
                if (got != truth) {
                    bad++;
                    if (mode == 0 && bad <= 5) printf("mode %d run %d step %d: truth %zu got %u (old %u new %u)\n", mode, run, step, truth, got, ob[1], nb[1]);
                }
                memcpy(ob, nb, 64);
                last = now;
                seen = f.ev.size();
            }
        }
        printf("mode %d: %ld reads, %ld new actions, %ld reads with more new than the list holds, %ld wrong\n", mode, reads, newtot, overflow, bad);
    }
}

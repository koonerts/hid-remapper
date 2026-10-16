#include "tick.h"

#include <pico/critical_section.h>
#include <pico/platform.h>
#include <pico/time.h>

static critical_section_t crit_sec;

static volatile bool tick_pending = false;
static volatile uint32_t tick_set_at = 0;
static uint32_t tick_taken_at = 0;
static volatile uint32_t merged = 0;
static volatile uint32_t sof_sum = 0, sof_n = 0, sof_max = 0;

void tick_init() {
    critical_section_init(&crit_sec);
}

void __no_inline_not_in_flash_func(set_tick_pending)() {
    critical_section_enter_blocking(&crit_sec);
    if (tick_pending) {
        merged++;  // the previous tick was never taken: a frame went unprocessed
    } else {
        tick_set_at = time_us_32();
    }
    tick_pending = true;
    critical_section_exit(&crit_sec);
}

bool get_and_clear_tick_pending() {
    critical_section_enter_blocking(&crit_sec);
    uint8_t tmp = tick_pending;
    tick_pending = false;
    if (tmp) {
        tick_taken_at = tick_set_at;
    }
    critical_section_exit(&crit_sec);
    return tmp;
}

uint32_t tick_taken_time() {
    return tick_taken_at;
}

uint32_t ticks_merged() {
    return merged;
}

// called from the SOF interrupt only, so nothing else updates these at the same time
void __no_inline_not_in_flash_func(tick_sof_time)(uint32_t us) {
    sof_sum += us;
    sof_n++;
    if (us > sof_max) {
        sof_max = us;
    }
}

void tick_sof_take(uint32_t* sum, uint32_t* n, uint32_t* max) {
    critical_section_enter_blocking(&crit_sec);
    *sum = sof_sum;
    *n = sof_n;
    *max = sof_max;
    sof_sum = sof_n = sof_max = 0;
    critical_section_exit(&crit_sec);
}

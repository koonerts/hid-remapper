#ifndef _TICK_H_
#define _TICK_H_

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

void tick_init();
void set_tick_pending();
bool get_and_clear_tick_pending();

// Frame statistics (shown on the QT Py settings page):
// time_us_32() when the tick last taken by get_and_clear_tick_pending() was set
uint32_t tick_taken_time();
// ticks set while the previous one was still pending, since boot: frames the main loop never processed
uint32_t ticks_merged();
// time spent in the 1 ms SOF interrupt (USB host frame); called from it
void tick_sof_time(uint32_t us);
// that time summed, counted and maxed since the last call, then reset
void tick_sof_take(uint32_t* sum, uint32_t* n, uint32_t* max);

#ifdef __cplusplus
}
#endif

#endif

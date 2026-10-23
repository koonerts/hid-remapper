#ifndef _GW_TURN_H_
#define _GW_TURN_H_

#include <stdint.h>

// v27: a measured turn for the QT Py page's cm/360 card (gw_turn.cc): the Feather moves along X by itself, steadily.

#define GW_TURN_IDLE 0
#define GW_TURN_RUN 1
#define GW_TURN_DONE 2
#define GW_TURN_HAND 3     // stopped: the real mouse moved
#define GW_TURN_STOPPED 4  // stopped by the QT Py
#define GW_TURN_TIMEOUT 5

// total counts (the sign is the direction) at per_s counts a second; hand = real movement that stops it (0 = none).
// It starts at the next frame. total 0 or per_s 0 = stop.
void gw_turn_start(int32_t total, uint32_t per_s, uint32_t hand);
void gw_turn_stop(void);

// once per frame on the frame's summed relative X and Y, after the direction gains; now_us = the frame's time
void gw_turn_filter(int32_t* x, int32_t* y, uint64_t now_us);

// changes of state (start, end): goes into the status's change nibble so the QT Py reads the block again
uint8_t gw_turn_seq(void);

// status bytes 246-251: state, run number, counts sent so far (int32 little-endian, signed like total)
void gw_turn_status(uint8_t* s);

#endif

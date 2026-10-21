#ifndef _GW_DIRGAIN_H_
#define _GW_DIRGAIN_H_

#include <stdint.h>

// v24: sensitivity per direction of movement (like Corsair's PureVector): 8 gains, one per 45° from "right", going
// clockwise on screen (right, down-right, down, down-left, left, up-left, up, up-right), blended between neighbours.

#define GW_DIRGAIN_N 8
#define GW_DIRGAIN_MIN 500   // per mille
#define GW_DIRGAIN_MAX 2000

// on = apply them; gains in per mille (1000 = 1.0), clamped to GW_DIRGAIN_MIN..MAX. All 1000 or off = untouched.
void gw_dirgain_set(bool on, const uint16_t* gains);

// once per frame on the frame's summed relative X and Y (either may be NULL: nothing is done)
void gw_dirgain_filter(int32_t* x, int32_t* y);

// status bytes 242-243: bit 0 applying, bit 1 got a table since power-on; crc8 of the 16 gain bytes as received
void gw_dirgain_status(uint8_t* s);

#endif

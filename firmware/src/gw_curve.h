#ifndef _GW_CURVE_H_
#define _GW_CURVE_H_

#include <stdint.h>

// v30: a speed curve (gw_curve.cc): sensitivity by how fast the mouse moves, like an XIM ballistic curve, so the XIM
// can stay linear (its own curve reads the mouse at its clock, not the mouse's, and turned the same hand movement
// into different turns).

#define GW_CURVE_N 10       // points at most (a link frame holds 44 bytes)
#define GW_CURVE_GMIN 250   // per mille
#define GW_CURVE_GMAX 4000

// on = apply it; tau_ms smooths the speed it reads (1-200); dpi_fallback is used while the mouse's DPI isn't known;
// n points (2-GW_CURVE_N) of speed in 0.1 cm/s, rising, and gain per mille (clamped to GMIN..GMAX), straight lines
// between them, held at the ends. Anything malformed = off.
void gw_curve_set(bool on, uint8_t tau_ms, uint16_t dpi_fallback, uint8_t n, const uint16_t* sp, const uint16_t* g);

// the payload as LINK_SET_CURVE carries it: [on, tau ms, dpi fallback uint16, n x (speed uint16, gain uint16)]
void gw_curve_link(const uint8_t* d, uint8_t len);

// once per frame on the frame's summed relative X and Y, after the turn (whose steady motion is a hand speed too);
// now_us = the frame's time; dpi = the mouse's DPI now (0 = unknown)
void gw_curve_filter(int32_t* x, int32_t* y, uint64_t now_us, uint16_t dpi);

// status bytes 252-253: bit 0 applying, bit 1 got a table since power-on; crc8 of the payload after its "on" byte
void gw_curve_status(uint8_t* s);

#endif

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

// v39: the slow-down gain, on top of the curve's while the curve applies: once a stroke slows from its peak speed, a gain
// by the speed as a share of that peak; speeding up is left alone. n points (1-GW_DECEL_N) of share in 0.1 % (v47;
// 10-990 = 1.0-99.0 %, rising; whole % before) and gain per mille (clamped to GMIN..GMAX), straight lines between them,
// the first held below it, and 100% = 1.0 implied (so it starts with no step). n = 0 or anything malformed = off.
#define GW_DECEL_N 6
void gw_curve_decel_set(bool on, uint8_t n, const uint16_t* p10, const uint16_t* g);
// the payload as LINK_SET_DECEL carries it: [flags, n x point]. flags bit 0 = on; v47: bit 1 = 4-byte points (share
// uint16 in 0.1 %, gain uint16), len 1 + 4n; without it 3-byte points (share whole % uint8, gain uint16), len 1 + 3n
void gw_curve_decel_link(const uint8_t* d, uint8_t len);

// once per frame on the frame's summed relative X and Y, after the turn (whose steady motion is a hand speed too);
// now_us = the frame's time; dpi = the mouse's DPI now (0 = unknown)
void gw_curve_filter(int32_t* x, int32_t* y, uint64_t now_us, uint16_t dpi);

// status bytes 252-253: bit 0 applying, bit 1 got a table since power-on; crc8 of the payload after its "on" byte
// v39: 252 bit 2 slow-down gain applying, bit 3 got one; 254 crc8 of its payload after the "on" byte
void gw_curve_status(uint8_t* s);

// v32: the live view (the QT Py page's speed curve chart). The hand speed is tracked whenever the DPI is known, curve on
// or off, sampled every GW_CURVE_SAMPLE_US, and every frame with motion counts 1 (= 1 ms) in its speed's bin
// (v36: when the speed is at least GW_CURVE_STILL, 0.1 cm/s; a resting mouse's stray counts stay out).
#define GW_CURVE_SAMPLE_US 5000
#define GW_CURVE_RING 32  // samples in the live block, newest first (v33: 32 = 160 ms, so one missed read leaves no gap)
#define GW_CURVE_BINS 26  // bin 0 below 5 cm/s, 1-24 log-spaced 5-160 cm/s (x 32^(1/24) each), 25 from 160 up
// the live block (GW_LINK_LIVE_LEN = 80 since v33, 48 in v32): 0 'L', 1 bit 0 applying, bit 1 DPI known, bit 2 moved in the last 50 ms (v36: at GW_CURVE_STILL or faster);
// 2-3 samples taken (uint16, wraps), 4-5 DPI used, 6-7 gain now per mille (1000 while not applying), 8-9 speed now
// (0.1 cm/s), 10-73 the last GW_CURVE_RING samples (0.1 cm/s, newest first; 10-41 in v32), the last byte crc8 of the rest;
// v39: 1 bit 3 the slow-down gain applying, 74-75 its gain now per mille (included in 6-7)
void gw_curve_live(uint8_t* b);
// the histogram block (GW_LINK_HIST_LEN = 116): 0 'H', 1 GW_CURVE_BINS, 2 resets since power-on, 4-7 ms in motion,
// 8-111 the bins (uint32 ms each), 115 crc8 of 0-114
void gw_curve_hist(uint8_t* b);
void gw_curve_hist_reset();
// the bin a speed (cm/s) counts in
uint8_t gw_curve_bin(float v);
// v37: real movement now (in the last 50 ms, at GW_CURVE_STILL or faster): the idle stage's "in use" (gw_mouse.cc)
bool gw_curve_moving(void);
// gw v59: the speed is known (a DPI from the stage or the curve's fallback): gw_curve_moving means something
bool gw_curve_speed_known(void);

#endif

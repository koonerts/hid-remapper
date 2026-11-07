#ifndef _GW_MOUSE_H_
#define _GW_MOUSE_H_

#include <stdint.h>

// Sends G-Wolves mouse settings commands for the chords in the chord table (gw_chords.h) and the
// QT Py settings link. Each model gets only the commands captured from it (capture-snippet.js);
// models are told apart by USB product ID, see gw_mouse.cc. Payloads and protocol notes:
// gw_payloads.h. Up to v14 the chords came from expression registers (hid-remapper-gwolves-chords*.json);
// since v15 those mappings are ignored and the table does it.

// registers: the remapper's expression registers (unused since v15)
void gw_tick(const int32_t* registers);
void gw_macro_tap(uint32_t usage, uint8_t frames);  // v35 (remapper.cc): press this usage for about frames ms, then let go
// remapper.cc, once per frame: how long the frame's processing took, and the chord filter's share (us)
void gw_frame_time(uint32_t total_us, uint32_t chords_us);
void gw_usb_stall(uint32_t merged);  // v64: main.cc, frames a blocked USB host task held up (settling)
// v50: where each frame's time goes, for the QT Py (link block 5, built each second in gw_frame_time). remapper.cc calls
// gw_prof_start() as process_mapping starts (section 0 = from the frame's tick to there) and gw_prof_mark(k) as each of
// its sections 1..GW_PROF_MAIN-1 ends. v51: sections GW_PROF_MAIN.. are parts of gw_tick (inside section 7, so not in a
// frame's total), timed by gw_prof_sub. Layout v2: 'P', 2, GW_PROF_N, frames uint16, the link interrupt's us uint32 and
// count uint16 (both a second), the heap in use and its size (16-byte units, uint16 each), then per section: average
// (0.1 us, uint16), worst (4 us steps), its share of the second's worst frame (4 us steps); crc8 last
// v55: layout 3 (GW_LINK_PROF_LEN 112), sections 20-23 = the main loop pass that took the frame's tick (main.cc,
// remapper_single.cc): 20 the USB host task (tuh_task: the mouse's reports), 21 of which the reports' parsing, 22 of which
// the G-Wolves hook, 23 from the tick to that pass's start (the pass before finishing). gw_prof_set / gw_prof_add
#define GW_PROF_MAIN 13
#define GW_PROF_N 24
void gw_prof_start(void);
void gw_prof_mark(uint8_t k);
void gw_prof_set(uint8_t k, uint32_t us);
void gw_prof_add(uint8_t k, uint32_t us);
void gw_on_set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint16_t len);
void gw_on_get_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, const uint8_t* report, uint16_t len);
void gw_on_input_report(uint8_t dev_addr, uint8_t instance, const uint8_t* report, uint16_t len);
void gw_on_hid_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num, uint16_t desc_len);
void gw_on_hid_umount(uint8_t dev_addr, uint8_t instance);
void gw_probe_interface(const uint8_t* desc_itf, uint16_t max_len);
// QT Py settings link (gw_link.h): the status block it reads, and a command frame it wrote
void gw_link_status(uint8_t* status);
bool gw_link_apply(const uint8_t* frame, uint8_t len, uint32_t now);
// v30: the X DPI of the stage in use (0 = not known), for the speed curve (gw_curve.h)
uint16_t gw_mouse_dpi(void);
// v37: once per frame before the chords, on the raw inputs: ch = gw_chords_filter's (buttons 1-5, the wheel), x/y the
// frame's motion. A button, the wheel or real movement is use: two idle minutes put the mouse on its lowest-DPI stage
// so it can sleep, and use puts the stage back.
void gw_idle_frame(int32_t* const* ch, const int32_t* x, const int32_t* y);
void gw_gap_totals(uint32_t* cnt, uint32_t* ms, uint32_t* frames);  // v66: the report pauses since the start (cnt: 5 lengths), for tests

#endif

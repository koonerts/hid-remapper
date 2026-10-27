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

#endif

#ifndef _GW_MOUSE_H_
#define _GW_MOUSE_H_

#include <stdint.h>

// Sends G-Wolves mouse settings commands when chord registers change (the expressions in
// hid-remapper-gwolves-chords.json). Each model gets only the commands captured from it
// (capture-snippet.js); models are told apart by USB product ID, see gw_mouse.cc.
// Payloads and protocol notes: gw_payloads.h.

// 0 -> nonzero: VUK front/rear sensor toggle. Warg: sensor angle back to GW_ANGLE_HOME.
#define GW_SENSOR_REGISTER 1
// 0 -> nonzero: next DPI stage, wrapping (VUK: on the active sensor).
#define GW_DPI_REGISTER 2
// Registers 3/4 are the Mid+Back/Mid+Fwd chords (XIM buttons 6/7); not used here.
// Warg sensor angle, two ways; the chord config decides which one is wired up:
//   0 -> nonzero: one step down / up (side-button layout: Mid+Back / Mid+Fwd)
#define GW_ANGLE_DOWN_REGISTER 7
#define GW_ANGLE_UP_REGISTER 8
//   wheel movement this frame while Middle is held (x1000), and Middle's state: a notch steps
//   the angle only once Middle has been down for GW_WHEEL_HOLD_US (wheel layout), so the
//   wheel nudging as Middle is pressed changes nothing
#define GW_WHEEL_REGISTER 5
#define GW_MIDDLE_REGISTER 6
#define GW_WHEEL_HOLD_US 300000

void gw_tick(int32_t sensor_register_value, int32_t dpi_register_value, int32_t angle_down_register_value, int32_t angle_up_register_value,
    int32_t wheel_register_value, int32_t middle_register_value);
void gw_on_set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint16_t len);
void gw_on_get_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, const uint8_t* report, uint16_t len);
void gw_on_input_report(uint8_t dev_addr, uint8_t instance, const uint8_t* report, uint16_t len);
void gw_on_hid_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num, uint16_t desc_len);
void gw_on_hid_umount(uint8_t dev_addr, uint8_t instance);
void gw_probe_interface(const uint8_t* desc_itf, uint16_t max_len);

#endif

#ifndef _GW_MOUSE_H_
#define _GW_MOUSE_H_

#include <stdint.h>

// Sends G-Wolves mouse settings commands when chord registers change (the expressions in
// hid-remapper-gwolves-chords.json). Each model gets only the commands captured from it
// (capture-snippet.js); models are told apart by USB product ID, see gw_mouse.cc.
// Payloads and protocol notes: gw_payloads.h.

// 0 -> nonzero: VUK front/rear sensor toggle. Warg: virtual sensor position to 0, and back to
// where it was on the next press.
#define GW_SENSOR_REGISTER 1
// 0 -> nonzero: next DPI stage, wrapping (VUK: on the active sensor).
#define GW_DPI_REGISTER 2
// Mouse wheel movement this frame while Middle is held (x1000). Warg: each notch moves the
// virtual sensor position to the next multiple of 5 up or down.
#define GW_WHEEL_REGISTER 5

void gw_tick(int32_t sensor_register_value, int32_t dpi_register_value, int32_t wheel_register_value);
void gw_on_set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint16_t len);
void gw_on_get_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, const uint8_t* report, uint16_t len);
void gw_on_input_report(uint8_t dev_addr, uint8_t instance, const uint8_t* report, uint16_t len);
void gw_on_hid_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num, uint16_t desc_len);
void gw_on_hid_umount(uint8_t dev_addr, uint8_t instance);
void gw_probe_interface(const uint8_t* desc_itf, uint16_t max_len);

#endif

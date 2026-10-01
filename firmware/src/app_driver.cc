#include "usb_midi_host.h"
#include "xbox.h"
#include "gw_mouse.h"

// Never claims anything; just records every interface descriptor for the Monitor diagnostics.
static bool probe_init() { return true; }
static bool probe_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const* desc_itf, uint16_t max_len) {
    gw_probe_interface((const uint8_t*) desc_itf, max_len);
    return false;
}
static bool probe_set_config(uint8_t dev_addr, uint8_t itf_num) { return false; }
static bool probe_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) { return false; }
static void probe_close(uint8_t dev_addr) {}

usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count) {
    static usbh_class_driver_t host_driver[] = {
        {
#if CFG_TUSB_DEBUG >= 2
            .name = "PROBE",
#endif
            .init = probe_init,
            .open = probe_open,
            .set_config = probe_set_config,
            .xfer_cb = probe_xfer_cb,
            .close = probe_close,
        },
        {
#if CFG_TUSB_DEBUG >= 2
            .name = "XBOXH",
#endif
            .init = xboxh_init,
            .open = xboxh_open,
            .set_config = xboxh_set_config,
            .xfer_cb = xboxh_xfer_cb,
            .close = xboxh_close,
        },
        {
#if CFG_TUSB_DEBUG >= 2
            .name = "MIDIH",
#endif
            .init = midih_init,
            .open = midih_open,
            .set_config = midih_set_config,
            .xfer_cb = midih_xfer_cb,
            .close = midih_close,
        },
    };
    *driver_count = 3;
    return host_driver;
}

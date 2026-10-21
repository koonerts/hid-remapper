// v23: the setup requests of the host the Feather is plugged into (through the ATEN), for telling hosts apart.
//
// TinyUSB calls tud_event_hook_cb from the USB interrupt for every device event it queues. For a setup packet the
// request is still in the controller's DPRAM then, so it is copied from there: no TinyUSB change needed.
//
// A capture starts at a bus reset that comes GW_HOSTCAP_GAP_US after the last request (or the first one since
// power-on: the ATEN cuts the Feather's power when it switches, so that is the usual case). Bus resets inside it
// are kept as entries too: Windows resets the device again after its first descriptor request.
//
// Status bytes 256-511:
//   256 'U'   257 capture number since power-on (0 = none yet)   258 changes to it   259 requests kept (at most
//   GW_HOSTCAP_MAX, bus resets included)   260 bus resets   261-262 ms from the capture's first reset to SET_CONFIGURATION
//   (0xFFFF none yet)   263-264 requests seen (more than are kept)   265 bit 0 configured, bit 1 suspended
//   266 0.1 s since the last request (at most 255)   267-271 0
//   272-503 the requests, 8 bytes each as sent (bmRequestType, bRequest, wValue, wIndex, wLength; little-endian);
//   a bus reset is FF FF 00 00 00 00 00 00   504-510 0   511 crc8 of 256-510

#include "gw_hostcap.h"

#include <string.h>

#include "hardware/structs/usb.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "tusb.h"
#include "device/dcd.h"

#include "gw_link.h"

#define GW_HOSTCAP_GAP_US 2000000

static uint8_t cap[GW_HOSTCAP_MAX][8];
static volatile uint8_t cap_n = 0;
static volatile uint8_t cap_id = 0;
static volatile uint8_t cap_upd = 0;
static volatile uint8_t cap_resets = 0;
static volatile uint16_t cap_total = 0;
static volatile bool cap_cfg = false;
static volatile uint32_t cap_start_us = 0;
static volatile uint32_t cap_cfg_us = 0;
static volatile uint32_t cap_last_us = 0;

static void __not_in_flash_func(cap_begin)(uint32_t now) {
    cap_id++;
    if (cap_id == 0) {
        cap_id = 1;
    }
    cap_n = 0;
    cap_resets = 0;
    cap_total = 0;
    cap_cfg = false;
    cap_start_us = now;
}

// Everything the interrupt runs is in RAM with no library calls or const tables: HID Remapper writes its config and
// the chord table to flash with XIP off, and USB interrupts may still come (as in gw_link.cc's I2C handler).
static void __not_in_flash_func(cap_add)(const uint8_t* p) {
    if (cap_n < GW_HOSTCAP_MAX) {
        for (int i = 0; i < 8; i++) {
            cap[cap_n][i] = p[i];
        }
        cap_n++;
    }
    if (cap_total < 0xFFFF) {
        cap_total++;
    }
    cap_upd++;
}

// TinyUSB's weak hook: every queued device event, from the USB interrupt
void __not_in_flash_func(tud_event_hook_cb)(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void) rhport;
    (void) in_isr;
    if ((eventid != DCD_EVENT_BUS_RESET) && (eventid != DCD_EVENT_SETUP_RECEIVED)) {
        return;  // SOF and transfers: nothing to do, and they're frequent
    }
    uint32_t now = time_us_32();
    if ((cap_id == 0) || ((eventid == DCD_EVENT_BUS_RESET) && ((now - cap_last_us) >= GW_HOSTCAP_GAP_US))) {
        cap_begin(now);
    }
    cap_last_us = now;
    uint8_t p[8];
    if (eventid == DCD_EVENT_BUS_RESET) {
        p[0] = p[1] = 0xFF;
        for (int i = 2; i < 8; i++) {
            p[i] = 0;
        }
        if (cap_resets < 255) {
            cap_resets++;
        }
        cap_add(p);
        return;
    }
    const volatile uint8_t* sp = (const volatile uint8_t*) &usb_dpram->setup_packet;
    for (int i = 0; i < 8; i++) {
        p[i] = sp[i];
    }
    if (!cap_cfg && (p[0] == 0x00) && (p[1] == 0x09)) {  // SET_CONFIGURATION
        cap_cfg = true;
        cap_cfg_us = now;
    }
    cap_add(p);
}

// v26: the capture number and its change counter, also put in status bytes 244/245 so the QT Py reads the 256-byte
// capture block (256-511) only when they moved, not on every recent-actions change
uint8_t gw_hostcap_id() {
    return cap_id;
}

uint8_t gw_hostcap_upd() {
    return cap_upd;
}

uint8_t gw_hostcap_seq() {
    static uint8_t seq = 0;
    static uint8_t seen = 0;
    static uint32_t at = 0;
    uint32_t now = time_us_32();
    uint8_t u = cap_upd;
    if ((u != seen) && ((now - at) >= 300000)) {
        seen = u;
        at = now;
        seq++;
    }
    return seq;
}

void gw_hostcap_fill(uint8_t* s) {
    memset(s, 0, 256);
    uint32_t irq = save_and_disable_interrupts();
    uint32_t now = time_us_32();
    s[0] = 'U';
    s[1] = cap_id;
    s[2] = cap_upd;
    s[3] = cap_n;
    s[4] = cap_resets;
    uint32_t cfg_ms = cap_cfg ? (cap_cfg_us - cap_start_us) / 1000 : 0xFFFF;
    if (cfg_ms > 0xFFFE) {
        cfg_ms = cap_cfg ? 0xFFFE : 0xFFFF;
    }
    s[5] = cfg_ms & 0xFF;
    s[6] = cfg_ms >> 8;
    s[7] = cap_total & 0xFF;
    s[8] = cap_total >> 8;
    uint32_t since = cap_id ? (now - cap_last_us) / 100000 : 255;
    s[10] = (since > 255) ? 255 : since;
    memcpy(s + 16, cap, cap_n * 8);
    restore_interrupts(irq);
    s[9] = (tud_mounted() ? 1 : 0) | (tud_suspended() ? 2 : 0);
    s[255] = gw_crc8(s, 255);
}

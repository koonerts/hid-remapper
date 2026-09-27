#ifndef _VUK_PAYLOADS_H_
#define _VUK_PAYLOADS_H_

#include <stdint.h>

// Captured 2026-09-27 from the G-Wolves web driver (capture-snippet.js).
// type: 0 = output report (sendReport), 1 = feature report (sendFeatureReport)
// data excludes the report ID byte, exactly as WebHID passes it.
//
// Clicking front/rear sent SET_FEATURE (report ID 0, 64 bytes):
//   00 01 18 01 <1=front|2=rear> 00...   <- cmd 0x18 = sensor select (write)
// followed by 23 identical reports in both captures whose command byte has
// 0x80 set (8f, 82, 85, 92, 87, 81, 86, 88, 95, 98, 97, 96, 94, 91, 84 and
// the 00 00 02 xx yy 8x family): the driver re-reading settings for its UI.
// Those are not replayed.

struct vuk_report_t {
    uint8_t type;
    uint8_t report_id;
    uint8_t len;
    uint8_t data[64];
};

static const vuk_report_t VUK_SELECT_FRONT[] = {
    { 1, 0x00, 64, { 0x00, 0x01, 0x18, 0x01, 0x01 } },
};

static const vuk_report_t VUK_SELECT_REAR[] = {
    { 1, 0x00, 64, { 0x00, 0x01, 0x18, 0x01, 0x02 } },
};

#endif

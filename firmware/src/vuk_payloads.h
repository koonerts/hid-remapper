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

// DPI stages, captured 2026-09-30 from the web driver (wired, 33E4:3908). The "00 00 02" family:
//   byte 3 = payload length, 4 = page, 5 = command (0x80 set = read), then data from byte 6.
// A read is a SET_FEATURE request followed by GET_FEATURE; the reply is 0xa1 + bytes 1..6 of
// the request + data. No packet carries a sensor number, so they act on the active sensor.
//   read stage   00 00 02 02 01 82 01 00  -> a1 00 02 02 01 82 01 <stage>   (stage is 1-based)
//   write stage  00 00 02 02 01 02 01 <stage>
//   write table  00 00 02 1e 01 01 01 <count> then 7 x (X, Y) 16-bit big-endian (4500 = 11 94);
//                the driver sends it after every stage change
static const vuk_report_t VUK_DPI_READ_STAGE[] = {
    { 1, 0x00, 64, { 0x00, 0x00, 0x02, 0x02, 0x01, 0x82, 0x01 } },
};

static const vuk_report_t VUK_DPI_WRITE_STAGE[] = {
    { 1, 0x00, 64, { 0x00, 0x00, 0x02, 0x02, 0x01, 0x02, 0x01, 0x01 } },
};

// Never seen in a capture: built from the colour-table read (00 00 02 16 02 81 01) and the
// table write. A wrong guess just fails the reply check and the stage count falls back.
static const vuk_report_t VUK_DPI_READ_TABLE[] = {
    { 1, 0x00, 64, { 0x00, 0x00, 0x02, 0x1e, 0x01, 0x81, 0x01 } },
};

static const vuk_report_t VUK_DPI_WRITE_TABLE[] = {
    { 1, 0x00, 64, { 0x00, 0x00, 0x02, 0x1e, 0x01, 0x01, 0x01 } },
};

#endif

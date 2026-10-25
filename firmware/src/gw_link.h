#ifndef _GW_LINK_H_
#define _GW_LINK_H_

#include <stdint.h>

// Settings link to a QT Py ESP32-S3 on the Feather's STEMMA QT port. The QT Py is the I2C master;
// the Feather answers as a slave from an interrupt, so the mouse's USB timing never waits on it.
//   QT Py writes a command frame:  cmd, seq, len, payload[len], crc8(cmd..payload)
//   QT Py reads the status block:  GW_LINK_STATUS_LEN bytes (layout in gw_mouse.cc, gw_link_status)
// Boards without the port (or other targets) get no-op versions.
#define GW_LINK_ADDR 0x42
#define GW_LINK_STATUS_LEN 512  // 64 since v11; 128 since v14 (64-127: extension, own crc); 192 since v15; 256 since v21; 512 since v23 (256-511: gw_hostcap.cc)
                                 // (128-191: the chord table, own crc; read only when it changes)
#define GW_LINK_FRAME_MAX 48
// v32: a 2-byte write [GW_LINK_SEL, block] makes the next read return that block (from its byte 0) instead of the
// status; the read after it is the status again. 1 = the live speed, 2 = the speed histogram (gw_curve.h)
#define GW_LINK_SEL 0xF5
#define GW_LINK_LIVE_LEN 80  // v33 (48 in v32)
#define GW_LINK_HIST_LEN 116

void gw_link_init();
// a complete frame written by the QT Py, if there is one; len = bytes in buf
bool gw_link_take_frame(uint8_t* buf, uint8_t* len);
// the next status block the QT Py reads (GW_LINK_STATUS_LEN bytes)
void gw_link_set_status(const uint8_t* status);
// v32: the next block 1 (live, GW_LINK_LIVE_LEN bytes) or 2 (histogram, GW_LINK_HIST_LEN bytes) a selected read returns
void gw_link_set_block(uint8_t which, const uint8_t* data);
// CRC-8, polynomial 0x07, init 0 (the QT Py uses the same)
uint8_t gw_crc8(const uint8_t* data, uint8_t len);

#endif

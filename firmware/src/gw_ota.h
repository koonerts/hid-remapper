#ifndef _GW_OTA_H_
#define _GW_OTA_H_

#include <stdint.h>

// v40: firmware updates through the QT Py (over Wi-Fi), wherever the Feather is plugged in. C, so gw_boot shares it.
// Flash layout of the remapper_ota build (8 MB):
//   0x000000  gw_boot (32 KB): after every reset, copies a staged update into the app slot, then starts the app
//   0x008000  the app slot: HID Remapper linked here (remapper_single_ota.ld)
//   0x200000  the staging slot: an update arrives here over the link
//   0x3FF000  the update record (one sector)
// HID Remapper's config and the chord table stay where they are, at the top of flash. The plain build (remapper,
// linked at 0) has no bootloader and refuses updates (GW_OTA_ERR_NOBOOT).
#define GW_OTA_BOOT_SIZE 0x8000u
#define GW_OTA_APP_OFFSET 0x8000u
#define GW_OTA_STAGE_OFFSET 0x200000u
#define GW_OTA_SLOT_MAX 0x1F8000u  // the largest image (both slots)
#define GW_OTA_REC_OFFSET 0x3FF000u
#define GW_OTA_DONE_OFFSET (GW_OTA_REC_OFFSET + 256u)  // the done word: 0xFFFFFFFF = still to copy, 0 = copied (or dropped)
#define GW_OTA_MAGIC 0x41544F47u                       // "GOTA"

typedef struct {  // page 0 of the record sector
    uint32_t magic;
    uint32_t size;  // bytes staged
    uint32_t crc;   // their CRC-32
    uint32_t seq;   // updates staged so far (diagnostics)
} gw_ota_rec_t;

// link commands (gw_mouse.cc passes them to gw_ota_link): cmd, seq, len, payload, crc8 as every link frame
#define GW_OTA_BEGIN 0x13  // [size uint32, crc32 uint32]: a new image starts (the record is dropped)
#define GW_OTA_DATA 0x14   // [offset uint32, 1-40 bytes]: only the next expected offset is taken (the QT Py goes back to it)
#define GW_OTA_END 0x15    // []: all sent: checked (CRC-32 of the staged copy), then the record is written
#define GW_OTA_APPLY 0x16  // []: after a good END: restart into gw_boot, which copies it in
#define GW_OTA_ABORT 0x17  // []

// the status block a selected read returns (GW_LINK_SEL, block 3): 0 'U', 1 state, 2 bit 0 this build takes updates
// (it runs after gw_boot), 3 error, 4-7 next expected offset, 8-11 size, 12-15 CRC-32 wanted, 16-19 CRC-32 of the
// staged copy (after END), 20-21 updates staged (the record's seq), 22 this firmware's version, 23 crc8 of 0-22
#define GW_OTA_IDLE 0
#define GW_OTA_RX 1
#define GW_OTA_READY 2
#define GW_OTA_APPLYING 3
#define GW_OTA_ERR_NOBOOT 1
#define GW_OTA_ERR_SIZE 2
#define GW_OTA_ERR_CRC 3
#define GW_OTA_ERR_FLASH 4  // a page didn't read back as written

// CRC-32 as zlib's (reflected 0xEDB88320, ~0 in and out); crc = 0 to start, or a previous result to continue.
// Bit by bit: no table in gw_boot's 32 KB, and an image takes well under a second either way.
static inline uint32_t gw_ota_crc32(const uint8_t* d, uint32_t n, uint32_t crc) {
    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

#ifdef __cplusplus
// the app side (gw_ota.cc)
void gw_ota_link(uint8_t cmd, const uint8_t* d, uint8_t len, uint32_t now);
void gw_ota_tick(uint32_t now);  // publishes the status block; the restart after APPLY
bool gw_ota_busy();              // receiving or about to restart
#endif

#endif

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
#define GW_OTA_DATA 0x14   // [offset uint32, 1-40 bytes; v43: 1-240]: only the next expected offset is taken (the QT Py goes back to it)
#define GW_OTA_END 0x15    // []: all sent: checked (CRC-32 of the staged copy), then the record is written
#define GW_OTA_APPLY 0x16  // []: after a good END: restart into gw_boot, which copies it in
#define GW_OTA_ABORT 0x17  // []: stops a transfer; v42: a staged (READY) image is dropped too, so no later reset installs it

// the status block a selected read returns (GW_LINK_SEL, block 3): 0 'U', 1 state, 2 bit 0 this build takes updates
// (it runs after gw_boot), bit 1 (v43) it takes data frames of up to GW_OTA_DATA_MAX bytes and erases ahead, bit 2 (v48)
// block 4 holds what a freeze left (the last restart was the watchdog's), 3 error, 4-7 next expected offset, 8-11 size, 12-15 CRC-32 wanted, 16-19 CRC-32 of the
// staged copy (after END), 20-21 updates staged (the record's seq), 22 this firmware's version, 23 crc8 of 0-22
#define GW_OTA_IDLE 0
#define GW_OTA_RX 1
#define GW_OTA_READY 2
#define GW_OTA_APPLYING 3
#define GW_OTA_ERASING 4  // v43: after BEGIN, the staging slot is erased ahead (a 64 KB block a main-loop pass); DATA waits for RX
#define GW_OTA_DATA_MAX 240  // v43: data bytes in one DATA frame (4 + 240 + the frame's 4 = 248 <= GW_LINK_FRAME_MAX)
#define GW_OTA_ERR_NOBOOT 1
#define GW_OTA_ERR_SIZE 2
#define GW_OTA_ERR_CRC 3
#define GW_OTA_ERR_FLASH 4  // a page didn't read back as written
#define GW_OTA_ERR_IMAGE 5  // v42: the staged image wouldn't start (its vector table isn't one for the app slot)

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
void gw_ota_tick(uint32_t now);
// v46: a word kept through the Feather's own restart (an update's, or any watchdog reset); 0 after a power-on
void gw_keep_set(uint32_t v);
uint32_t gw_keep_get();  // publishes the status block; the restart after APPLY

// v48: what the Feather was doing when it froze, for after the watchdog's restart (main.cc). gw_where(step) marks each
// part of the main loop (and returns the step it replaces, to put back); gw_freeze_init() runs once at start, before the
// watchdog is on: it takes what the frozen run left into block 4 and starts the catch (gw_ota.cc).
// Block 4 (GW_LINK_SEL 4, GW_LINK_DIAG_LEN bytes): 0 'F'   1 layout (1)   2 freezes since power-on (at most 15)
//   3 1 = the last restart was a freeze (the rest describes it; else 4-30 are 0)   4 step (GW_AT_*)   5 the last link
//   command taken   6 the update's state (GW_OTA_*)   7 what the catch found: 0 nothing (it never ran: interrupts off, or
//   the CPU stalled on the bus), 1 the main loop still for 2.5 s, 2 a hard fault   8 the exception that was running
//   (0 the main loop, 3 a hard fault, 16 + n IRQ n)   9 times caught (every 0.5 s while still)   10-13 the update's
//   offset (the next expected; erasing: the next block)   14-17 the first PC caught   18-21 its LR   22-25 the latest
//   PC   26-30 0   31 crc8 of 0-30
#define GW_AT_LOOP 1     // the USB host's task (the mouse); a pass of the main loop starts here
#define GW_AT_TICK 2     // a tick's processing (the mapping, gw_tick)
#define GW_AT_TUD 3      // the USB device's task (the XIM)
#define GW_AT_SEND 4     // reports out, the rest of the main loop
#define GW_AT_PERSIST 5  // HID Remapper's config written to flash
#define GW_AT_LINK 6     // a link frame applied (byte 5: its command)
#define GW_AT_ERASE 7    // an update: a staging block erased
#define GW_AT_PAGE 8     // an update: a page written
#define GW_AT_RECORD 9   // an update: its record written or dropped
#define GW_AT_CHECK 10   // an update: the staged copy's CRC-32
#define GW_AT_HALT 11    // after APPLY: waiting for the restart into gw_boot
#define GW_AT_CHORDS 12  // the chord table written to flash
uint8_t gw_where(uint8_t step);
void gw_where_cmd(uint8_t cmd);
void gw_freeze_init();
bool gw_freeze_kept();  // the last restart was a freeze: block 4 says what it left
#endif

// v42: an image whose vector table (at +0x100, as boot2 starts one) has a stack pointer in RAM and a Thumb reset vector
// inside the app slot: what gw_boot will start. vt = the image's word at +0x100 wherever it lies (staging or app slot);
// base = the app slot's address as images are linked: GW_OTA_LINK_BASE
#ifndef GW_OTA_LINK_BASE
#define GW_OTA_LINK_BASE ((uint32_t) (XIP_BASE + GW_OTA_APP_OFFSET))  // a host test with real images sets 0x10008000u
#endif
static inline int gw_ota_startable(const uint32_t* vt, uint32_t base) {
    uint32_t sp = vt[0], pc = vt[1];
    return (sp > 0x20000000u) && (sp <= 0x20042000u) && (pc & 1u) && (pc - base < GW_OTA_SLOT_MAX);
}

#endif

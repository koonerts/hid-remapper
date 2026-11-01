// gw_boot (v40): the Feather's bootloader for updates through the QT Py (layout in ../src/gw_ota.h). It runs first
// after every reset. When an update is staged and its done word is still erased, it checks the staged copy (CRC-32),
// copies it into the app slot a 4 KB sector at a time, checks the app slot, and only then writes the done word. A power
// cut anywhere in that leaves the staged copy and the record as they were, so the next power-on simply copies again
// (sectors that already match are skipped). Then it starts the app at 0x10008000 the way the ROM's boot2 starts a
// normal image. With nothing there to start, it hands over to the ROM's USB drive, so a USB cable can always recover
// the Feather. Built only for feather_host (CMakeLists.txt); the image must stay under GW_OTA_BOOT_SIZE (feather-build
// checks).

#include <stdbool.h>
#include <string.h>

#include "gw_ota.h"
#include "hardware/flash.h"

#define GW_BOOT_TRIES 3  // copies in one power-on before giving up on it (the USB drive, below)

static const uint8_t* xip(uint32_t off) {
    return (const uint8_t*) (XIP_BASE + off);
}

static uint32_t crc_at(uint32_t off, uint32_t n) {
    return gw_ota_crc32(xip(off), n, 0);
}

// v43: in chunks that end on 64 KB flash blocks (the app slot starts at 32 KB, so the first is 32 KB): a chunk that differs
// is erased whole (a block erase where it covers one, about 4x quicker than its 16 sectors) and written a sector at a time
static void copy_stage(uint32_t size) {
    static uint8_t buf[FLASH_SECTOR_SIZE];
    for (uint32_t o = 0; o < size;) {
        uint32_t a = GW_OTA_APP_OFFSET + o, end = ((a / 0x10000) + 1) * 0x10000 - GW_OTA_APP_OFFSET;
        if (end > size) {
            end = size;
        }
        if (!memcmp(xip(GW_OTA_STAGE_OFFSET + o), xip(a), end - o)) {
            o = end;
            continue;  // already there (a copy a power cut stopped goes on from here)
        }
        uint32_t span = (end - o + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE;
        flash_range_erase(a, span);
        for (uint32_t q = o; q < end; q += FLASH_SECTOR_SIZE) {
            uint32_t n = (end - q < FLASH_SECTOR_SIZE) ? end - q : FLASH_SECTOR_SIZE;
            memcpy(buf, xip(GW_OTA_STAGE_OFFSET + q), n);
            memset(buf + n, 0xFF, FLASH_SECTOR_SIZE - n);
            flash_range_program(GW_OTA_APP_OFFSET + q, buf, FLASH_SECTOR_SIZE);
        }
        o = end;
    }
}

static void mark_done(void) {
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memset(page, 0, 4);  // programming only clears bits: the rest of the page stays as it is
    flash_range_program(GW_OTA_DONE_OFFSET, page, FLASH_PAGE_SIZE);
}

// the app slot holds something startable: a stack pointer in RAM and a Thumb reset vector inside the slot
static bool app_ok(void) {
    return gw_ota_startable((const uint32_t*) xip(GW_OTA_APP_OFFSET + 0x100), GW_OTA_LINK_BASE);
}

// the decision: copy a staged update in if there is one; true = start the app, false = the ROM's USB drive
bool gw_boot_run(void) {
    const gw_ota_rec_t* r = (const gw_ota_rec_t*) xip(GW_OTA_REC_OFFSET);
    uint32_t done = *(const uint32_t*) xip(GW_OTA_DONE_OFFSET);
    if ((r->magic == GW_OTA_MAGIC) && (done == 0xFFFFFFFFu)) {
        if ((r->size < 0x108) || (r->size > GW_OTA_SLOT_MAX) || (crc_at(GW_OTA_STAGE_OFFSET, r->size) != r->crc) ||
            !gw_ota_startable((const uint32_t*) xip(GW_OTA_STAGE_OFFSET + 0x100), GW_OTA_LINK_BASE)) {
            mark_done();  // a damaged staged copy, or one that wouldn't start (v42): dropped, the app there stays
        } else {
            bool copied = false;
            for (int k = 0; (k < GW_BOOT_TRIES) && !copied; k++) {
                copy_stage(r->size);
                copied = crc_at(GW_OTA_APP_OFFSET, r->size) == r->crc;
            }
            if (!copied) {
                return false;  // the app slot won't take it: the USB drive (the record stays pending for a retry)
            }
            mark_done();
        }
    }
    return app_ok();
}

#ifndef GW_BOOT_TEST

#include "hardware/structs/nvic.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "pico/bootrom.h"

// as boot2 ends: the vector table, then its stack pointer and reset vector. This bootloader enabled no interrupt; any
// pending one is cleared so the app starts as from a reset (its runtime sets the clocks up again).
static void __attribute__((noreturn)) start_app(void) {
    uint32_t vt = XIP_BASE + GW_OTA_APP_OFFSET + 0x100;
    systick_hw->csr = 0;
    nvic_hw->icer = 0xFFFFFFFFu;
    nvic_hw->icpr = 0xFFFFFFFFu;
    scb_hw->vtor = vt;
    __asm volatile(
        "ldr r0, [%0, #0]\n"
        "msr msp, r0\n"
        "ldr r0, [%0, #4]\n"
        "bx r0\n" ::"r"(vt)
        : "r0");
    __builtin_unreachable();
}

int main(void) {
    if (gw_boot_run()) {
        start_app();
    }
    reset_usb_boot(0, 0);
    return 0;
}

#endif

// v40: the app side of updates through the QT Py (layout and commands in gw_ota.h). The QT Py streams the image over
// the link into the staging slot, a 256-byte page at a time as it fills (each 4 KB sector erased when its first page
// is written); END checks the staged copy's CRC-32 and writes the record; APPLY restarts into gw_boot, which copies it
// into the app slot. A frame that doesn't carry the next expected offset is ignored: the main loop stands still while
// a sector erases (~45 ms), the link's one-frame mailbox drops what comes meanwhile, and the QT Py goes back to the
// offset this status block says. Flash is written as gw_chords.cc writes it: interrupts stay on (copy_to_ram), their
// handlers are in RAM.

#include "gw_ota.h"

#include <string.h>

#include "gw_link.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"

extern const char gw_fw_tag[];  // gw_mouse.cc: "GWFW:v<version>;"

#ifndef GW_OTA_APP
#define GW_OTA_APP 0  // the remapper_ota build sets it (CMakeLists.txt): it runs after gw_boot
#endif

static uint8_t st = GW_OTA_IDLE, err = 0;
static uint32_t size = 0, crc = 0, expect = 0, got_crc = 0, seq_last = 0;
static uint32_t erase_at = 0, erase_end = 0;  // v43: the staging slot still to erase after BEGIN (offsets in it)  // v42: seq_last = the record's count before BEGIN dropped it
static uint8_t page[FLASH_PAGE_SIZE];
static uint16_t page_n = 0;
static bool reboot_due = false;
static uint32_t reboot_at = 0;
static bool dirty = true;

static uint32_t get32(const uint8_t* p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static void put32(uint8_t* p, uint32_t v) {
    for (int k = 0; k < 4; k++) {
        p[k] = (v >> (8 * k)) & 0xFF;
    }
}

static const gw_ota_rec_t* rec() {
    return (const gw_ota_rec_t*) (XIP_BASE + GW_OTA_REC_OFFSET);
}

static void fail(uint8_t e) {
    st = GW_OTA_IDLE;
    err = e;
    dirty = true;
}

// the page buffer to staging offset off (the bytes after page_n as 0xFF); v43: the slot was erased ahead (ERASING)
static bool flash_page(uint32_t off) {
    memset(page + page_n, 0xFF, FLASH_PAGE_SIZE - page_n);
    uint32_t a = GW_OTA_STAGE_OFFSET + off;
    flash_range_program(a, page, FLASH_PAGE_SIZE);
    page_n = 0;
    return !memcmp((const void*) (XIP_BASE + a), page, FLASH_PAGE_SIZE);
}

static bool write_record() {
    gw_ota_rec_t r = { GW_OTA_MAGIC, size, crc, ++seq_last };
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &r, sizeof(r));
    flash_range_erase(GW_OTA_REC_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(GW_OTA_REC_OFFSET, page, FLASH_PAGE_SIZE);
    return !memcmp((const void*) (XIP_BASE + GW_OTA_REC_OFFSET), page, FLASH_PAGE_SIZE);
}

void gw_ota_link(uint8_t cmd, const uint8_t* d, uint8_t len, uint32_t now) {
    switch (cmd) {
        case GW_OTA_BEGIN:
            if (len != 8) {
                return;
            }
            if (!GW_OTA_APP) {
                fail(GW_OTA_ERR_NOBOOT);
                return;
            }
            size = get32(d);
            crc = get32(d + 4);
            if ((size == 0) || (size > GW_OTA_SLOT_MAX)) {
                fail(GW_OTA_ERR_SIZE);
                return;
            }
            // an older record goes: a restart mid-transfer must not copy a half-written staging slot
            if (rec()->magic == GW_OTA_MAGIC) {
                seq_last = rec()->seq;
            }
            if (rec()->magic != 0xFFFFFFFFu) {
                flash_range_erase(GW_OTA_REC_OFFSET, FLASH_SECTOR_SIZE);
            }
            expect = 0;
            page_n = 0;
            got_crc = 0;
            err = 0;
            reboot_due = false;
            // v43: erased ahead, a 64 KB block a pass (gw_ota_tick), not a 4 KB sector as each one's first page came in: an
            // erase stopped the main loop ~45 ms, and the frames that came meanwhile were lost and sent again
            erase_at = 0;
            erase_end = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE;
            st = GW_OTA_ERASING;
            dirty = true;
            return;
        case GW_OTA_DATA: {
            if ((st != GW_OTA_RX) || (len < 5) || (get32(d) != expect)) {
                return;
            }
            uint8_t n = len - 4;
            if (expect + n > size) {
                fail(GW_OTA_ERR_SIZE);
                return;
            }
            for (uint8_t i = 0; i < n; i++) {
                page[page_n++] = d[4 + i];
                if ((page_n == FLASH_PAGE_SIZE) && !flash_page(expect + i + 1 - FLASH_PAGE_SIZE)) {
                    fail(GW_OTA_ERR_FLASH);
                    return;
                }
            }
            expect += n;
            dirty = true;
            return;
        }
        case GW_OTA_END:
            if ((st != GW_OTA_RX) || (expect != size)) {
                return;
            }
            if ((page_n > 0) && !flash_page(size - page_n)) {
                fail(GW_OTA_ERR_FLASH);
                return;
            }
            got_crc = gw_ota_crc32((const uint8_t*) (XIP_BASE + GW_OTA_STAGE_OFFSET), size, 0);
            if (got_crc != crc) {
                fail(GW_OTA_ERR_CRC);
                return;
            }
            if ((size < 0x108) || !gw_ota_startable((const uint32_t*) (XIP_BASE + GW_OTA_STAGE_OFFSET + 0x100), GW_OTA_LINK_BASE)) {
                fail(GW_OTA_ERR_IMAGE);  // v42: e.g. the plain build (linked at 0): installed, it would leave only the USB drive
                return;
            }
            if (!write_record()) {
                fail(GW_OTA_ERR_FLASH);
                return;
            }
            st = GW_OTA_READY;
            dirty = true;
            return;
        case GW_OTA_APPLY:
            if (st != GW_OTA_READY) {
                return;
            }
            st = GW_OTA_APPLYING;
            reboot_due = true;
            reboot_at = now + 200000;  // the QT Py reads "applying" first
            dirty = true;
            return;
        case GW_OTA_ABORT:
            if (st == GW_OTA_READY) {
                flash_range_erase(GW_OTA_REC_OFFSET, FLASH_SECTOR_SIZE);  // v42: staged but stopped: no later reset installs it
            }
            if (st != GW_OTA_APPLYING) {
                st = GW_OTA_IDLE;
                dirty = true;
            }
            return;
        default:
            return;
    }
}

void gw_ota_tick(uint32_t now) {
    static uint32_t pub_at = 0;
    if (st == GW_OTA_ERASING) {  // v43: the next 64 KB block of the staging slot (block erases: about 4x quicker than its sectors)
        uint32_t n = 0x10000 - (erase_at % 0x10000);
        if (n > erase_end - erase_at) {
            n = erase_end - erase_at;
        }
        flash_range_erase(GW_OTA_STAGE_OFFSET + erase_at, n);
        erase_at += n;
        if (erase_at >= erase_end) {
            st = GW_OTA_RX;
        }
        dirty = true;
    }
    if (reboot_due && ((int32_t) (now - reboot_at) >= 0)) {
        reboot_due = false;
        watchdog_reboot(0, 0, 10);  // through the ROM and gw_boot, as a power-on
    }
    if (!dirty && (now - pub_at < 100000)) {
        return;
    }
    pub_at = now;
    uint8_t b[GW_LINK_OTA_LEN];
    memset(b, 0, sizeof(b));
    b[0] = 'U';
    b[1] = st;
    b[2] = GW_OTA_APP ? 3 : 0;  // bit 1 (v43): 240-byte data frames, erased ahead
    b[3] = err;
    put32(b + 4, expect);
    put32(b + 8, size);
    put32(b + 12, crc);
    put32(b + 16, got_crc);
    uint32_t seq = (rec()->magic == GW_OTA_MAGIC) ? rec()->seq : 0;
    b[20] = seq & 0xFF;
    b[21] = (seq >> 8) & 0xFF;
    uint8_t v = 0;  // the running version, from the tag (which this use keeps in the image: the QT Py finds an upload's there)
    for (const char* p = gw_fw_tag + 6; (*p >= '0') && (*p <= '9'); p++) {
        v = v * 10 + (*p - '0');
    }
    b[22] = v;
    b[GW_LINK_OTA_LEN - 1] = gw_crc8(b, GW_LINK_OTA_LEN - 1);
    dirty = !gw_link_set_block(3, b);  // v42: refused while that buffer was being read: again next pass
}


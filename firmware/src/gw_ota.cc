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
#ifndef GW_NO_CATCH  // v48: the host tests have no timer or interrupts
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico/bootrom.h"
#endif

extern const char gw_fw_tag[];  // gw_mouse.cc: "GWFW:v<version>;"

#ifndef GW_OTA_APP
#define GW_OTA_APP 0  // the remapper_ota build sets it (CMakeLists.txt): it runs after gw_boot
#endif

#ifndef GW_KEEP  // v46: the host tests' hardware/watchdog.h points it at a plain variable
#define GW_KEEP (watchdog_hw->scratch[0])
#endif
void gw_keep_set(uint32_t v) {
    GW_KEEP = v;
}
uint32_t gw_keep_get() {
    return GW_KEEP;
}

#ifndef gw_ota_halt  // audit 11-01: the host tests' hardware/watchdog.h defines it to count the stop and go on
#define gw_ota_halt() while (true) { tight_loop_contents(); }
#endif

static uint8_t st = GW_OTA_IDLE, err = 0;
static uint32_t size = 0, crc = 0, expect = 0, got_crc = 0, seq_last = 0;
static uint32_t erase_at = 0, erase_end = 0;  // v43: the staging slot still to erase after BEGIN (offsets in it)  // v42: seq_last = the record's count before BEGIN dropped it
static uint8_t page[FLASH_PAGE_SIZE];
static uint16_t page_n = 0;
static bool reboot_due = false;
static uint32_t reboot_at = 0;
static bool dirty = true;

// v48: what a freeze left. Watchdog scratch registers outlive the watchdog's restart (main.cc) and gw_boot; a power-on
// clears them (0 is gw_keep's, 4 the SDK's: watchdog_enable_caused_reboot). 1: where the main loop is (gw_where): bits
// 0-7 the step, 8-15 the last link command, 16-23 the update's state, 24-27 freezes since power-on, 28-31 0xA. 2: the
// update's offset. Written only by gw_catch (below): 3 the first PC caught, 5 bits 0-7 the exception that was running,
// 8-15 1 still / 2 fault, 16-23 times caught, 24-31 0xC7; 6 the LR with the first PC; 7 the latest PC.
#ifndef GW_SCRATCH
#define GW_SCRATCH(i) (watchdog_hw->scratch[i])
#endif
#define GW_CATCH_EVERY_US 500000u
#define GW_CATCH_STILL_US 2500000u  // under the watchdog's 4 s, over any one flash erase (a 64 KB block: 2 s at most)

static uint32_t where_hi = 0;
static uint8_t where_step = 0, where_cmd = 0;
static volatile uint32_t beat_us = 0;  // the main loop's last pass (gw_catch compares)
#ifndef GW_NO_CATCH
static volatile bool caught = false;  // gw_catch recorded something since the main loop's last pass
static int catch_alarm = -1;
#endif
static uint8_t fz[GW_LINK_DIAG_LEN];
static bool fz_kept = false;

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

// v49: the freezes during updates (11-01, 11-02; v48's record: the main loop running junk from flash right after a page
// write's boot2 had finished). The SDK's flash_range_erase/program run boot2 (it puts the flash back into XIP's
// continuous-read mode) with interrupts on, and boot2 sends each flash command as separate writes to the SSI with
// nothing holding chip select: an interrupt between two of them lets the FIFO run dry, CS rises and the command is cut.
// The flash then isn't in continuous-read mode, XIP reads return zeros (movs r0, r0) and the CPU slides through them.
// During an update the link's interrupt comes once per I2C byte (44k a second at 400 kHz), and boot2 ran after each of
// ~900 pages. Here interrupts are off for everything but the ROM's erase/program itself (which holds CS by an IO
// override, so an interrupt there only pauses the clock): a few us before and after, the long part with them on.
#ifndef GW_NO_CATCH
static uint32_t gw_boot2[64];  // boot2, copied from flash's first 256 bytes (gw_boot's: the same as the app's)
static bool gw_boot2_ok = false;

static void __no_inline_not_in_flash_func(gw_flash_op)(uint32_t off, const uint8_t* data, uint32_t count, bool erase) {
    rom_connect_internal_flash_fn connect = (rom_connect_internal_flash_fn) rom_func_lookup_inline(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    rom_flash_exit_xip_fn exit_xip = (rom_flash_exit_xip_fn) rom_func_lookup_inline(ROM_FUNC_FLASH_EXIT_XIP);
    rom_flash_range_erase_fn erase_fn = (rom_flash_range_erase_fn) rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_ERASE);
    rom_flash_range_program_fn program_fn = (rom_flash_range_program_fn) rom_func_lookup_inline(ROM_FUNC_FLASH_RANGE_PROGRAM);
    rom_flash_flush_cache_fn flush = (rom_flash_flush_cache_fn) rom_func_lookup_inline(ROM_FUNC_FLASH_FLUSH_CACHE);
    if (!gw_boot2_ok) {
        for (int i = 0; i < 64; i++) {
            gw_boot2[i] = ((const volatile uint32_t*) XIP_BASE)[i];
        }
        gw_boot2_ok = true;
    }
    __compiler_memory_barrier();
    uint32_t irq = save_and_disable_interrupts();
    connect();
    exit_xip();
    restore_interrupts(irq);
    if (erase) {
        erase_fn(off, count, 1u << 16, 0xD8);  // as the SDK: 64 KB block erases where they fit, else 4 KB sectors
    } else {
        program_fn(off, data, count);
    }
    irq = save_and_disable_interrupts();
    flush();  // also lets go of the CS override
    ((void (*)(void))((uintptr_t) gw_boot2 + 1))();
    restore_interrupts(irq);
}

void gw_flash_erase(uint32_t off, uint32_t count) {
    gw_flash_op(off, nullptr, count, true);
}

void gw_flash_program(uint32_t off, const uint8_t* data, uint32_t count) {
    gw_flash_op(off, data, count, false);
}
#else
void gw_flash_erase(uint32_t off, uint32_t count) {  // the host tests' simulated flash
    flash_range_erase(off, count);
}

void gw_flash_program(uint32_t off, const uint8_t* data, uint32_t count) {
    flash_range_program(off, data, count);
}
#endif

uint8_t gw_where(uint8_t step) {
    uint8_t was = where_step;
    where_step = step;
#ifndef GW_NO_CATCH
    if (step == GW_AT_LOOP) {
        beat_us = time_us_32();
        if (catch_alarm >= 0) {
            // review 11-02: the catch's alarm goes 2.5 s ahead every pass, so while the loop runs it never fires (its
            // interrupt is above the USB frame's: a needless one could land inside a packet's handshake)
            timer_hw->alarm[catch_alarm] = beat_us + GW_CATCH_STILL_US;
        }
        if (caught) {  // review 11-02: a stall the loop came out of isn't the freeze a later restart reports
            caught = false;
            GW_SCRATCH(3) = 0;
            GW_SCRATCH(5) = 0;
            GW_SCRATCH(6) = 0;
            GW_SCRATCH(7) = 0;
        }
    }
#endif
    GW_SCRATCH(1) = where_hi | (uint32_t) st << 16 | (uint32_t) where_cmd << 8 | step;
    GW_SCRATCH(2) = (st == GW_OTA_ERASING) ? erase_at : expect;
    return was;
}

void gw_where_cmd(uint8_t cmd) {
    where_cmd = cmd;
}

bool gw_freeze_kept() {
    return fz_kept;
}

#ifndef GW_NO_CATCH
// From a top-priority timer interrupt every 0.5 s (kind 1), and the hard-fault handler (kind 2), with the frame the
// CPU stacked on entry: r0-r3, r12, LR, PC, xPSR. In RAM, as everything an interrupt runs (gw_link.cc). Kind 1 fires
// only once the main loop has been still for 2.5 s (gw_where pushes its alarm ahead), then every 0.5 s until the
// watchdog restarts the Feather 1.5 s later.
extern "C" void __not_in_flash_func(gw_catch_c)(const uint32_t* frame, uint32_t kind) {
    if (kind == 1) {
        timer_hw->intr = 1u << catch_alarm;
        timer_hw->alarm[catch_alarm] = timer_hw->timerawl + GW_CATCH_EVERY_US;
        if (timer_hw->timerawl - beat_us < GW_CATCH_STILL_US) {
            return;
        }
    }
    uint32_t info = GW_SCRATCH(5), n = ((info >> 24) == 0xC7) ? (info >> 16) & 0xFF : 0;
    if (n == 0) {
        GW_SCRATCH(3) = frame[6];
        GW_SCRATCH(6) = frame[5];
    }
    GW_SCRATCH(7) = frame[6];
    GW_SCRATCH(5) = 0xC7000000u | ((n < 255) ? n + 1 : 255) << 16 | kind << 8 | (frame[7] & 0x3F);
    caught = true;
    if (kind == 2) {
        __asm volatile("1: b 1b");  // a fault: the watchdog restarts it
    }
}

// the stacked frame (MSP, or PSP when the LR says so) and the kind, to gw_catch_c; no prologue, so SP is the frame
#define GW_CATCH_ENTRY(kind)                                                                                       \
    "movs r1, #4\n mov r2, lr\n tst r2, r1\n beq 1f\n mrs r0, psp\n b 2f\n1: mrs r0, msp\n2: movs r1, #" #kind "\n" \
    " ldr r2, =gw_catch_c\n bx r2\n .ltorg\n"
extern "C" void __attribute__((naked, section(".time_critical.gw_catch_isr"))) gw_catch_isr() {
    __asm volatile(GW_CATCH_ENTRY(1));
}
// in place of crt0's (a bkpt that locks the CPU up until the watchdog restarts it, recording nothing)
extern "C" void __attribute__((naked, section(".time_critical.isr_hardfault"))) isr_hardfault() {
    __asm volatile(GW_CATCH_ENTRY(2));
}
#endif

void gw_freeze_init() {
    uint32_t w = GW_SCRATCH(1), info = GW_SCRATCH(5);
    uint32_t n = ((w >> 28) == 0xA) ? (w >> 24) & 0x0F : 0;
    fz_kept = watchdog_enable_caused_reboot();
    memset(fz, 0, sizeof(fz));
    fz[0] = 'F';
    fz[1] = 1;
    if (fz_kept) {
        n += (n < 15) ? 1 : 0;
        fz[3] = 1;
        fz[4] = w & 0xFF;
        fz[5] = (w >> 8) & 0xFF;
        fz[6] = (w >> 16) & 0xFF;
        put32(fz + 10, GW_SCRATCH(2));
        if ((info >> 24) == 0xC7) {
            fz[7] = (info >> 8) & 0xFF;
            fz[8] = info & 0xFF;
            fz[9] = (info >> 16) & 0xFF;
            put32(fz + 14, GW_SCRATCH(3));
            put32(fz + 18, GW_SCRATCH(6));
            put32(fz + 22, GW_SCRATCH(7));
        }
    }
    fz[2] = n;
    fz[GW_LINK_DIAG_LEN - 1] = gw_crc8(fz, GW_LINK_DIAG_LEN - 1);
    gw_link_set_block(4, fz);
    GW_SCRATCH(2) = 0;
    GW_SCRATCH(3) = 0;
    GW_SCRATCH(5) = 0;
    GW_SCRATCH(6) = 0;
    GW_SCRATCH(7) = 0;
    where_hi = 0xA0000000u | n << 24;
    gw_where(0);
#ifndef GW_NO_CATCH
    catch_alarm = hardware_alarm_claim_unused(false);  // the default alarm pool has 3; PIO-USB none (skip_alarm_pool)
    if (catch_alarm >= 0) {
        uint irq = TIMER_IRQ_0 + catch_alarm;
        irq_set_exclusive_handler(irq, gw_catch_isr);
        irq_set_priority(irq, 0);  // above the USB frame timer and the link: it must get in while one of them is stuck
        hw_set_bits(&timer_hw->inte, 1u << catch_alarm);
        timer_hw->alarm[catch_alarm] = timer_hw->timerawl + GW_CATCH_STILL_US;
        irq_set_enabled(irq, true);
    }
#endif
}

// the page buffer to staging offset off (the bytes after page_n as 0xFF); v43: the slot was erased ahead (ERASING)
static bool flash_page(uint32_t off) {
    memset(page + page_n, 0xFF, FLASH_PAGE_SIZE - page_n);
    uint32_t a = GW_OTA_STAGE_OFFSET + off;
    uint8_t was = gw_where(GW_AT_PAGE);  // v48
    gw_flash_program(a, page, FLASH_PAGE_SIZE);  // v49
    gw_where(was);
    page_n = 0;
    return !memcmp((const void*) (XIP_BASE + a), page, FLASH_PAGE_SIZE);
}

static void record_erase() {  // v48: the record dropped, marked
    uint8_t was = gw_where(GW_AT_RECORD);
    gw_flash_erase(GW_OTA_REC_OFFSET, FLASH_SECTOR_SIZE);
    gw_where(was);
}

static bool write_record() {
    gw_ota_rec_t r = { GW_OTA_MAGIC, size, crc, ++seq_last };
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &r, sizeof(r));
    uint8_t was = gw_where(GW_AT_RECORD);  // v48
    gw_flash_erase(GW_OTA_REC_OFFSET, FLASH_SECTOR_SIZE);
    gw_flash_program(GW_OTA_REC_OFFSET, page, FLASH_PAGE_SIZE);
    gw_where(was);
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
                record_erase();
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
            {
                uint8_t was = gw_where(GW_AT_CHECK);  // v48
                got_crc = gw_ota_crc32((const uint8_t*) (XIP_BASE + GW_OTA_STAGE_OFFSET), size, 0);
                gw_where(was);
            }
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
                record_erase();  // v42: staged but stopped: no later reset installs it
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
        uint8_t was = gw_where(GW_AT_ERASE);  // v48
        gw_flash_erase(GW_OTA_STAGE_OFFSET + erase_at, n);
        erase_at += n;
        gw_where(was);
        if (erase_at >= erase_end) {
            st = GW_OTA_RX;
        }
        dirty = true;
    }
    if (reboot_due && ((int32_t) (now - reboot_at) >= 0)) {
        reboot_due = false;
        gw_where(GW_AT_HALT);  // v48
        watchdog_reboot(0, 0, 10);  // through the ROM and gw_boot, as a power-on
        // audit 11-01: and never back to the main loop: in those 10 ms a chord table or config save could start a 4 KB
        // erase (~45 ms) that the reset cuts off. Interrupts (USB, link) keep running until it lands.
        gw_ota_halt();
    }
    if (!dirty && (now - pub_at < 100000)) {
        return;
    }
    pub_at = now;
    uint8_t b[GW_LINK_OTA_LEN];
    memset(b, 0, sizeof(b));
    b[0] = 'U';
    b[1] = st;
    b[2] = (GW_OTA_APP ? 3 : 0) | (fz_kept ? 4 : 0);  // bit 1 (v43): 240-byte data frames, erased ahead; bit 2 (v48): block 4 holds a freeze
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


#include "gw_chords.h"

#include <cstring>

#include "gw_link.h"
// pico.h brings in the board header (FEATHER_HOST_BOARD), as in gw_link.cc
#include "pico.h"
#include "pico/time.h"

// ---- the table --------------------------------------------------------------------------------

// What the chord JSON did up to v14 (and the tuning's hold times then: 500, 600, 300 ms).
static const gw_rule_t DEFAULTS[] = {
    { 3, 1, GA_ANGLE_RESET, GA_POS_RESET, 50 },  // Middle + Left: tap angle reset, hold position reset
    { 3, 2, GA_DPI_NEXT, GA_RADIO, 60 },         // Middle + Right: tap next DPI stage, hold Wi-Fi
    { 3, 4, GA_POS_UP, GA_NONE, 0 },             // Middle + Back: position up (toward the back)
    { 3, 5, GA_POS_DOWN, GA_NONE, 0 },           // Middle + Forward: position down (toward the front)
    { 3, GW_CH_WHEEL, GA_ANGLE_UP, GA_ANGLE_DOWN, 30 },  // Middle held 300 ms, wheel: angle up / down
};
#define N_DEFAULTS (sizeof(DEFAULTS) / sizeof(DEFAULTS[0]))

static_assert(sizeof(gw_rule_t) == GW_CH_RULE_BYTES, "rules are stored and sent as 5 bytes");

static gw_rule_t table[GW_CH_MAX];
static uint8_t n_rules = 0;
static uint8_t seq = 1;
static uint8_t flags = 0;
static bool loaded = false;

static bool enabled(const gw_rule_t& r) {
    return (r.tap != GA_NONE) || (r.hold != GA_NONE);
}

bool gw_chords_valid(const gw_rule_t* rules, uint8_t n) {
    if (n > GW_CH_MAX) {
        return false;
    }
    for (uint8_t i = 0; i < n; i++) {
        const gw_rule_t& r = rules[i];
        if ((r.mod < 1) || (r.mod > GW_CH_BUTTONS) || (r.trig < 1) || (r.trig > GW_CH_WHEEL) || (r.mod == r.trig) ||
            (r.tap >= GA_COUNT) || (r.hold >= GA_COUNT) || !enabled(r)) {
            return false;
        }
        // a button's hold needs a hold time, or a tap could never happen
        if ((r.trig != GW_CH_WHEEL) && (r.hold != GA_NONE) && (r.hold10 < 10)) {
            return false;
        }
        for (uint8_t j = 0; j < i; j++) {
            if ((rules[j].mod == r.mod) && (rules[j].trig == r.trig)) {
                return false;
            }
        }
    }
    return true;
}

// ---- flash -----------------------------------------------------------------------------------
// Record: 'G' 'W' 'C' 'H', version 1, count, the rules (5 bytes each), crc8 of everything before it.
// One flash sector of its own, just below HID Remapper's config (the last sector), so neither
// write ever touches the other.

#define REC_VERSION 1
#define REC_MAX (6 + GW_CH_MAX * GW_CH_RULE_BYTES + 1)

static uint8_t rec_build(uint8_t* b, const gw_rule_t* rules, uint8_t n) {
    b[0] = 'G';
    b[1] = 'W';
    b[2] = 'C';
    b[3] = 'H';
    b[4] = REC_VERSION;
    b[5] = n;
    memcpy(b + 6, rules, n * GW_CH_RULE_BYTES);
    uint8_t len = 6 + n * GW_CH_RULE_BYTES;
    b[len] = gw_crc8(b, len);
    return len + 1;
}

static bool rec_parse(const uint8_t* b, gw_rule_t* rules, uint8_t* n) {
    if ((b[0] != 'G') || (b[1] != 'W') || (b[2] != 'C') || (b[3] != 'H') || (b[4] != REC_VERSION) || (b[5] > GW_CH_MAX)) {
        return false;
    }
    uint8_t len = 6 + b[5] * GW_CH_RULE_BYTES;
    if (gw_crc8(b, len) != b[len]) {
        return false;
    }
    memcpy(rules, b + 6, b[5] * GW_CH_RULE_BYTES);
    *n = b[5];
    return gw_chords_valid(rules, *n);
}

#if defined(FEATHER_HOST_BOARD)

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"

// one sector below HID Remapper's config, placed by the same rule as main.cc's CONFIG_OFFSET_IN_FLASH
#if PICO_RP2350
#define CH_CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - PERSISTED_CONFIG_SIZE - 4096)
#else
#define CH_CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - PERSISTED_CONFIG_SIZE)
#endif
#define CH_FLASH_OFFSET (CH_CONFIG_OFFSET - FLASH_SECTOR_SIZE)
static const uint8_t* flash_rec() {
    return (const uint8_t*) (XIP_BASE + CH_FLASH_OFFSET);
}
static bool flash_write(const uint8_t* rec, uint8_t len) {
    static uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    if (rec != NULL) {
        memcpy(page, rec, len);
    }
    // as main.cc's do_persist_config. Interrupts stay on (the PIO-USB frames must go on), so every
    // interrupt handler that can run meanwhile must be in RAM: the SDK's and TinyUSB's are, the
    // fork's own (firmware/src runs from flash) are marked __not_in_flash_func (gw_link.cc's I2C one)
#if !PICO_COPY_TO_RAM
    uint32_t ints = save_and_disable_interrupts();
#endif
    flash_range_erase(CH_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    if (rec != NULL) {
        flash_range_program(CH_FLASH_OFFSET, page, FLASH_PAGE_SIZE);
    }
#if !PICO_COPY_TO_RAM
    restore_interrupts(ints);
#endif
    return memcmp(flash_rec(), page, len ? len : 6) == 0;
}

#else

// other boards and the host test: a stand-in in RAM (the test looks at it)
uint8_t gw_chords_fake_flash[256];
int gw_chords_fake_writes = 0;
static bool fake_init = false;
static const uint8_t* flash_rec() {
    if (!fake_init) {
        memset(gw_chords_fake_flash, 0xFF, sizeof(gw_chords_fake_flash));
        fake_init = true;
    }
    return gw_chords_fake_flash;
}
static bool flash_write(const uint8_t* rec, uint8_t len) {
    flash_rec();
    memset(gw_chords_fake_flash, 0xFF, sizeof(gw_chords_fake_flash));
    if (rec != NULL) {
        memcpy(gw_chords_fake_flash, rec, len);
    }
    gw_chords_fake_writes++;
    return true;
}
// the test reloads the table as after a power cycle
void gw_chords_test_reboot() {
    loaded = false;
    seq = 1;
    flags = 0;
}

#endif

static void load() {
    loaded = true;
    if (rec_parse(flash_rec(), table, &n_rules)) {
        flags |= GW_CH_FROM_FLASH;
    } else {
        memcpy(table, DEFAULTS, sizeof(DEFAULTS));
        n_rules = N_DEFAULTS;
        flags &= ~GW_CH_FROM_FLASH;
    }
}

// ---- the engine ------------------------------------------------------------------------------

#define LATCH_NONE (-1)
#define LATCH_ORPHAN (-2)  // held back, but its rule went away with a table change: no actions

struct Button {
    bool raw;          // down, as the mouse reports it
    uint32_t down_at;  // when it went down
    int8_t latch;      // the rule whose chord this press is, or LATCH_*
    bool hold_fired;
    bool has_hold;     // its rule has a hold action the plugged-in mouse supports (decided at the press)
};
// latch must start at LATCH_NONE: a zeroed one would mean "in a chord of rule 0" (every button
// firing rule 0's tap at the first frame)
static Button btn[GW_CH_BUTTONS] = { { false, 0, LATCH_NONE, false, false }, { false, 0, LATCH_NONE, false, false },
    { false, 0, LATCH_NONE, false, false }, { false, 0, LATCH_NONE, false, false }, { false, 0, LATCH_NONE, false, false } };
static int32_t saved[GW_CH_BUTTONS];  // the slots' contents this frame, put back afterwards

void gw_chords_filter(int32_t* const* in) {
    if (!loaded) {
        load();
    }
    uint32_t now = time_us_32();
    bool raw[GW_CH_BUTTONS];
    for (int i = 0; i < GW_CH_BUTTONS; i++) {
        saved[i] = (in[i] != NULL) ? *in[i] : 0;  // a bit per interface that has it down
        raw[i] = (saved[i] != 0);
        if (raw[i] && !btn[i].raw) {
            btn[i].down_at = now;
        }
    }
    int32_t wheel = (in[GW_CH_WHEEL - 1] != NULL) ? *in[GW_CH_WHEEL - 1] : 0;

    // a trigger going down while its modifier is down starts a chord (the first rule that matches).
    // A button that starts a chord in this same frame isn't a modifier for another one, so two buttons
    // going down together can't each hold the other back.
    uint8_t started = 0;
    for (int i = 0; i < GW_CH_BUTTONS; i++) {
        if (!raw[i] || btn[i].raw || (btn[i].latch != LATCH_NONE)) {
            continue;
        }
        for (uint8_t r = 0; r < n_rules; r++) {
            const gw_rule_t& rule = table[r];
            if ((rule.trig == i + 1) && enabled(rule) && raw[rule.mod - 1] && !(started & (1 << (rule.mod - 1)))) {
                Button& b = btn[i];
                b.latch = r;
                b.hold_fired = false;
                // a hold the mouse can't do isn't waited for: the tap fires on the press, as v14 did for
                // the VUK's sensor switch (Middle + Left, whose hold only means something on the Warg)
                b.has_hold = (rule.hold != GA_NONE) && gw_action_supported(rule.hold);
                started |= 1 << i;
                if (!b.has_hold && (rule.tap != GA_NONE)) {
                    gw_action(rule.tap, now);
                }
                break;
            }
        }
    }
    // holds and releases
    for (int i = 0; i < GW_CH_BUTTONS; i++) {
        Button& b = btn[i];
        if (b.latch == LATCH_NONE) {
            continue;
        }
        if (b.latch == LATCH_ORPHAN) {
            if (!raw[i]) {
                b.latch = LATCH_NONE;
            }
            continue;
        }
        const gw_rule_t& rule = table[b.latch];
        if (raw[i]) {
            if (b.has_hold && !b.hold_fired && ((now - b.down_at) >= rule.hold10 * 10000u)) {
                b.hold_fired = true;
                gw_action(rule.hold, now);
            }
        } else {
            if (b.has_hold && !b.hold_fired && (rule.tap != GA_NONE)) {
                gw_action(rule.tap, now);
            }
            b.latch = LATCH_NONE;
        }
    }
    // the wheel: held back while a wheel rule's modifier is down; acts once that has lasted long
    // enough, one step per frame with wheel movement (a high-resolution wheel reports 120 a notch)
    // (with several wheel rules down at once, the first in table order whose hold has passed acts)
    bool hold_wheel = false;
    bool acted = false;
    for (uint8_t r = 0; r < n_rules; r++) {
        const gw_rule_t& rule = table[r];
        if ((rule.trig != GW_CH_WHEEL) || !raw[rule.mod - 1]) {
            continue;
        }
        hold_wheel = true;
        if (!acted && (wheel != 0) && ((now - btn[rule.mod - 1].down_at) >= rule.hold10 * 10000u)) {
            uint8_t a = (wheel > 0) ? rule.tap : rule.hold;
            if (a != GA_NONE) {
                gw_action(a, now);
            }
            acted = true;
        }
    }

    for (int i = 0; i < GW_CH_BUTTONS; i++) {
        btn[i].raw = raw[i];
        if ((in[i] != NULL) && (btn[i].latch != LATCH_NONE)) {
            *in[i] = 0;
        }
    }
    if ((in[GW_CH_WHEEL - 1] != NULL) && hold_wheel) {
        *in[GW_CH_WHEEL - 1] = 0;  // a relative input: the remapper zeroes it after the frame anyway
    }
}

// the buttons' slots hold a bit per interface, updated bit by bit as reports come in, so they
// get their real contents back once the mappings have run
void gw_chords_restore(int32_t* const* in) {
    for (int i = 0; i < GW_CH_BUTTONS; i++) {
        if (in[i] != NULL) {
            *in[i] = saved[i];
        }
    }
}

bool gw_chords_ignore_mapping(uint32_t source_usage) {
    uint32_t page = source_usage & 0xFFFF0000;
    return (page == 0xFFF30000) || (page == 0xFFF50000);  // EXPR_USAGE_PAGE, REGISTER_USAGE_PAGE
}

// ---- the link ----------------------------------------------------------------------------------

uint8_t gw_chords_count() {
    if (!loaded) {
        load();
    }
    return n_rules;
}

const gw_rule_t* gw_chords_rules() {
    if (!loaded) {
        load();
    }
    return table;
}

uint8_t gw_chords_seq() {
    return seq;
}

uint8_t gw_chords_flags() {
    return flags;
}

static gw_rule_t staged[GW_CH_MAX];
static uint8_t staged_total = 0;
static uint8_t staged_seq = 0;  // the link seq of the staged first part
static bool staging = false;
static bool save_wanted = false;
static bool erase_wanted = false;
static uint32_t save_at = 0;
#define SAVE_DELAY_US 200000  // a quick second change saves once

static void apply(const gw_rule_t* rules, uint8_t n) {
    memcpy(table, rules, n * sizeof(gw_rule_t));
    n_rules = n;
    seq++;
    if (seq == 0) {
        seq = 1;  // 0 never shows, so a reader can use it for "nothing yet"
    }
    // a chord in progress keeps its button held back until release, without its old actions
    for (Button& b : btn) {
        if (b.latch >= 0) {
            b.latch = LATCH_ORPHAN;
        }
    }
    flags &= ~GW_CH_REJECTED;
    flags |= GW_CH_SAVING;
    save_at = time_us_32();
}

// [part, total, rules...]: part 0 holds rules 0-7, part 1 rules 8-11; total 0xFF in part 0 = the
// built-in defaults (and the flash record is erased, so a later firmware's defaults apply)
// the QT Py numbers its frames 1-255, then 1 again
static uint8_t seq_after(uint8_t s) {
    return (s == 255) ? 1 : s + 1;
}

// The second part only completes the first one sent right before it (the next seq): a first part
// that never arrived can't leave an older half-sent table to be completed with this one's rest.
bool gw_chords_link(const uint8_t* d, uint8_t len, uint8_t link_seq) {
    if (!loaded) {
        load();
    }
    if (len < 2) {
        return false;
    }
    uint8_t part = d[0], total = d[1];
    if ((part == 0) && (total == 0xFF)) {
        if (len != 2) {
            return false;
        }
        staging = false;
        apply(DEFAULTS, N_DEFAULTS);
        erase_wanted = true;
        save_wanted = false;
        return true;
    }
    if ((total > GW_CH_MAX) || (part > 1)) {
        return false;
    }
    uint8_t first = part * 8;
    uint8_t count = (total > first) ? ((total - first > 8) ? 8 : total - first) : 0;
    if ((part == 1) && (total <= 8)) {
        return false;
    }
    if (len != 2 + count * GW_CH_RULE_BYTES) {
        return false;
    }
    if (part == 0) {
        staging = true;
        staged_total = total;
        staged_seq = link_seq;
    } else if (!staging || (staged_total != total) || (link_seq != seq_after(staged_seq))) {
        staging = false;
        return false;
    }
    memcpy(staged + first, d + 2, count * GW_CH_RULE_BYTES);
    if (first + count < total) {
        return true;  // the rest comes in the next frame
    }
    staging = false;
    if (gw_chords_valid(staged, total)) {
        apply(staged, total);
        erase_wanted = false;
        save_wanted = true;
    } else {
        flags |= GW_CH_REJECTED;
    }
    return true;
}

void gw_chords_service() {
    if (!(save_wanted || erase_wanted) || ((time_us_32() - save_at) < SAVE_DELAY_US)) {
        return;
    }
    bool ok;
    if (erase_wanted) {
        ok = flash_write(NULL, 0);
        flags &= ~GW_CH_FROM_FLASH;
    } else {
        uint8_t rec[REC_MAX];
        uint8_t len = rec_build(rec, table, n_rules);
        if (memcmp(flash_rec(), rec, len) == 0) {
            ok = true;  // already there
        } else {
            ok = flash_write(rec, len);
        }
        if (ok) {
            flags |= GW_CH_FROM_FLASH;
        }
    }
    save_wanted = erase_wanted = false;
    flags &= ~GW_CH_SAVING;
    if (ok) {
        flags &= ~GW_CH_SAVE_FAILED;
    } else {
        flags |= GW_CH_SAVE_FAILED;
    }
}

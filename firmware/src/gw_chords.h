#ifndef _GW_CHORDS_H_
#define _GW_CHORDS_H_

#include <stdint.h>

// Chords, from a table the QT Py settings page edits (kept in its own flash sector, so it
// survives without the QT Py). A rule is "hold a modifier button, then press a trigger":
//   modifier  button 1-5 (1 Left, 2 Right, 3 Middle, 4 Back, 5 Forward)
//   trigger   button 1-5, or 6 = the wheel
//   button trigger: tap = action on release, hold = action once held hold10 x 10 ms; with no
//     hold action the tap action fires on the press instead (nothing to wait for)
//   (v21) a tap action with GA_DOUBLE set acts only on a double tap: the second tap within
//     GW_DOUBLE_TAP_US of the first (release to release; press to press when there's no hold action)
//   wheel trigger: tap = action per frame of wheel up, hold = per frame of wheel down, once the
//     modifier has been held hold10 x 10 ms (the wheel nudging as the modifier goes down then
//     does nothing)
// A trigger pressed while its rule's modifier is down is a chord until it's released, even if
// the modifier comes up first; the game never sees that press. Pressing the trigger first and the
// modifier second is not a chord (Right then Middle stays an ADS click). The modifier itself always
// reaches the game, and the wheel is held back while any wheel rule's modifier is down.
#define GW_CH_MAX 12
#define GW_CH_RULE_BYTES 5
#define GW_CH_BUTTONS 5
#define GW_CH_WHEEL 6  // trigger number of the wheel
// the inputs gw_chords_filter works on: input_state slots of buttons 1-5, then the wheel
#define GW_CH_IN 6

struct gw_rule_t {
    uint8_t mod;     // 1-5
    uint8_t trig;    // 1-6
    uint8_t tap;     // action (GA_*)
    uint8_t hold;    // action (GA_*)
    uint8_t hold10;  // x 10 ms
};

// Actions. The numbers are stored in flash and sent by the QT Py: never renumber.
#define GA_NONE 0
#define GA_DPI_NEXT 1
#define GA_DPI_PREV 2
#define GA_DPI_1 3  // GA_DPI_1 + n = stage n + 1, up to GA_DPI_1 + 4
#define GA_ANGLE_UP 8
#define GA_ANGLE_DOWN 9
#define GA_ANGLE_RESET 10  // on a VUK: front/rear sensor toggle (what Middle + Left always did there)
#define GA_POS_UP 11       // toward the back
#define GA_POS_DOWN 12     // toward the front
#define GA_POS_RESET 13
#define GA_RADIO 14  // QT Py Wi-Fi on/off
#define GA_SYNC 15   // Motion Sync on/off
#define GA_SNAP 16   // Angle Snap on/off
#define GA_LOD 17    // lift-off distance, next step (wraps)
#define GA_COUNT 18
#define GA_DOUBLE 0x80  // v21, on a tap action of a button trigger: only a double tap acts
#define GA_ACTION(a) ((uint8_t) ((a) & 0x7F))
#define GW_DOUBLE_TAP_US 500000

// flags (status byte 125)
#define GW_CH_FROM_FLASH 1  // the table came from flash (else: the built-in defaults)
#define GW_CH_SAVING 2      // a change is waiting to be written to flash
#define GW_CH_REJECTED 4    // the last table sent was invalid and was dropped
#define GW_CH_SAVE_FAILED 8 // a flash write read back wrong

// remapper.cc: once per frame before the mappings run, then after them. in[i] = the input_state
// slot of button i + 1 (in[5] = the wheel), NULL if there's none.
void gw_chords_filter(int32_t* const* in);
void gw_chords_restore(int32_t* const* in);
// remapper.cc: mappings from expressions and registers are how the old chord JSON worked; with the
// chord table doing that job they're ignored, so the buttons pass through untouched.
bool gw_chords_ignore_mapping(uint32_t source_usage);

// the table, for the link status block
uint8_t gw_chords_count();
const gw_rule_t* gw_chords_rules();
uint8_t gw_chords_seq();
uint8_t gw_chords_flags();
// a LINK_SET_CHORDS payload; false = malformed (a well-formed but invalid table is dropped and
// reported in the flags instead)
bool gw_chords_link(const uint8_t* d, uint8_t len, uint8_t seq);
// from gw_tick: writes a pending change to flash
void gw_chords_service();
// validation, shared with the tests
bool gw_chords_valid(const gw_rule_t* rules, uint8_t n);

// implemented in gw_mouse.cc. src/mod/trig say where it came from, for the page's list of recent actions
// (v21): GW_SRC_*, and the chord's buttons (0 for a page command)
#define GW_SRC_TAP 1
#define GW_SRC_HOLD 2
#define GW_SRC_DOUBLE 3
#define GW_SRC_WHEEL_UP 4
#define GW_SRC_WHEEL_DOWN 5
void gw_action_src(uint8_t action, uint8_t src, uint8_t mod, uint8_t trig, uint32_t now);
inline void gw_action(uint8_t action, uint32_t now) { gw_action_src(action, 0, 0, 0, now); }
// whether an action does anything with the mice plugged in (a hold that does nothing isn't waited for)
bool gw_action_supported(uint8_t action);

#endif

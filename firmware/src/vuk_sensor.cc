#include "vuk_sensor.h"

#include <cstddef>
#include <cstdlib>

#include "globals.h"
#include "remapper.h"
#include "vuk_payloads.h"

// Arm's prebuilt toolchains ship libstdc++ with the verbose terminate handler, which drags in
// the 33 KB demangler (cp-demangle.o). In this copy_to_ram image that comes straight out of
// the heap and the Feather dies at boot. Ubuntu's toolchain (CI) is built without it.
namespace __gnu_cxx {
void __verbose_terminate_handler() {
    abort();
}
}  // namespace __gnu_cxx

static int32_t prev_trigger = 0;
static bool front_selected = true;  // VUK default; one extra chord re-syncs if wrong
static int32_t trigger_count = 0;
static uint32_t completions = 0;

// Diagnostics, visible on remapper.org/config's Monitor tab while it's open:
//   0xFFF40001  chord count (register 1 rising edges)
//   0xFFF40002  number of SET_FEATURE/OUTPUT reports queued for this chord
//   0xFFF401nn  mounted HID interface/feature report pair nn: (dev_addr<<8|itf) << 8 | report ID (0xFF = none)
//   0xFFF402nn  SET_REPORT completion nn (per chord): dev_addr<<16 | itf<<8 | bytes (0 = device rejected it)
#define VUK_DIAG(n) (0xFFF40000 | (n))

static int send_one(const vuk_report_t& r) {
    int sent = 0;
    if (r.type == 0) {
        for (auto const& [key, size] : out_report_sizes) {
            if ((key & 0xFF) == r.report_id) {
                queue_out_report(key >> 16, r.report_id, r.data, r.len);
                sent++;
            }
        }
    } else {
        for (auto const& [itf, reports] : their_feature_usages) {
            if (reports.count(r.report_id)) {
                queue_set_feature_report(itf, r.report_id, r.data, r.len);
                sent++;
            }
        }
        if (sent == 0) {
            // The descriptor parser only records feature reports that declare usages. If the
            // dongle's vendor interface didn't register, try every HID interface; the wrong
            // ones just STALL (shows up as 0 bytes in 0xFFF40004).
            for (auto const& [itf, reports] : their_feature_usages) {
                queue_set_feature_report(itf, r.report_id, r.data, r.len);
                sent++;
            }
        }
    }
    return sent;
}

// Sends to EVERY matching interface, so with a hub both the dongle and the wired
// mouse get it (whichever one is live acts on it).
static void send_all(const vuk_report_t* reports, size_t n) {
    int sent = 0;
    for (size_t i = 0; i < n; i++) {
        if (reports[i].len != 0) {
            sent += send_one(reports[i]);
        }
    }
    if (monitor_enabled) {
        monitor_usage(VUK_DIAG(1), trigger_count, 0);
        monitor_usage(VUK_DIAG(2), sent, 0);
        uint32_t n = 0;
        for (auto const& [itf, reports] : their_feature_usages) {
            if (reports.empty()) {
                monitor_usage(VUK_DIAG(0x100 + (n++ & 0xFF)), (int32_t) itf << 8 | 0xFF, 0);
            }
            for (auto const& [report_id, usages] : reports) {
                monitor_usage(VUK_DIAG(0x100 + (n++ & 0xFF)), (int32_t) itf << 8 | report_id, 0);
            }
        }
    }
}

void vuk_on_set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint16_t len) {
    if (monitor_enabled) {
        monitor_usage(VUK_DIAG(0x200 + (completions++ & 0xFF)), (int32_t) dev_addr << 16 | instance << 8 | (len & 0xFF), 0);
    }
}

void vuk_sensor_tick(int32_t trigger) {
    if (trigger != 0 && prev_trigger == 0) {
        trigger_count++;
        completions = 0;
        front_selected = !front_selected;
        if (front_selected) {
            send_all(VUK_SELECT_FRONT, sizeof(VUK_SELECT_FRONT) / sizeof(VUK_SELECT_FRONT[0]));
        } else {
            send_all(VUK_SELECT_REAR, sizeof(VUK_SELECT_REAR) / sizeof(VUK_SELECT_REAR[0]));
        }
    }
    prev_trigger = trigger;
}
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

// Sends to EVERY interface that owns report_id of the given type, so with a hub
// both the dongle and the wired mouse get it (whichever one is live acts on it).
// Keys: out_report_sizes = (dev_addr<<8|itf) << 16 | report_id.
static void send_one(const vuk_report_t& r) {
    if (r.type == 0) {
        for (auto const& [key, size] : out_report_sizes) {
            if ((key & 0xFF) == r.report_id) {
                queue_out_report(key >> 16, r.report_id, r.data, r.len);
            }
        }
    } else {
        for (auto const& [itf, reports] : their_feature_usages) {
            if (reports.count(r.report_id)) {
                queue_set_feature_report(itf, r.report_id, r.data, r.len);
            }
        }
    }
}

static void send_all(const vuk_report_t* reports, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (reports[i].len != 0) {
            send_one(reports[i]);
        }
    }
}
void vuk_sensor_tick(int32_t trigger) {
    if (trigger != 0 && prev_trigger == 0) {
        front_selected = !front_selected;
        if (front_selected) {
            send_all(VUK_SELECT_FRONT, sizeof(VUK_SELECT_FRONT) / sizeof(VUK_SELECT_FRONT[0]));
        } else {
            send_all(VUK_SELECT_REAR, sizeof(VUK_SELECT_REAR) / sizeof(VUK_SELECT_REAR[0]));
        }
    }
    prev_trigger = trigger;
}

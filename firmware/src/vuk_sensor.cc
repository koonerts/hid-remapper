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

// Finds the dongle interface that owns report_id of the given type.
// Keys: out_report_sizes = (dev_addr<<8|itf) << 16 | report_id.
static bool find_interface(const vuk_report_t& r, uint16_t* interface) {
    if (r.type == 0) {
        for (auto const& [key, size] : out_report_sizes) {
            if ((key & 0xFF) == r.report_id) {
                *interface = key >> 16;
                return true;
            }
        }
    } else {
        for (auto const& [itf, reports] : their_feature_usages) {
            if (reports.count(r.report_id)) {
                *interface = itf;
                return true;
            }
        }
    }
    return false;
}

static void send_all(const vuk_report_t* reports, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint16_t interface;
        if ((reports[i].len == 0) || !find_interface(reports[i], &interface)) {
            continue;
        }
        if (reports[i].type == 0) {
            queue_out_report(interface, reports[i].report_id, reports[i].data, reports[i].len);
        } else {
            queue_set_feature_report(interface, reports[i].report_id, reports[i].data, reports[i].len);
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

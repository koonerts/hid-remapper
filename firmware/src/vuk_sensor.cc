#include "vuk_sensor.h"

#include <cstddef>
#include <cstdlib>
#include <map>

#include <tusb.h>

#include "globals.h"
#include "remapper.h"
#include "vuk_payloads.h"
#include "pico/time.h"

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

// Diagnostics, visible on remapper.org/config's Monitor tab while it's open.
// Emitted a few per millisecond (a monitor report only holds 7 items).
//   0xFFF40001  chord count (register 1 rising edges)
//   0xFFF40002  number of SET_FEATURE/OUTPUT reports queued for this chord
//   0xFFF40005  device-level mounts (tuh_mount_cb) since boot
//   0xFFF40006  device-level unmounts since boot
//   0xFFF401nn  known interface/feature report pair nn: (dev_addr<<8|itf) << 8 | report ID (0xFF = none)
//   0xFFF402nn  SET_REPORT completion nn (per chord): dev_addr<<16 | idx<<8 | bytes (0 = device rejected it)
//   0xFFF403nn  HID interface mount nn: dev_addr<<28 | idx<<20 | bInterfaceNumber<<16 | report descriptor length
//   0xFFF404nn  HID interface unmount nn: dev_addr<<8 | idx
#define VUK_DIAG(n) (0xFFF40000 | (n))

struct diag_item_t {
    uint32_t usage;
    int32_t value;
};
#define DIAG_Q 128
static diag_item_t diag_q[DIAG_Q];
static uint16_t diag_head = 0;
static uint16_t diag_count = 0;

static void diag(uint32_t n, int32_t value) {
    if (diag_count == DIAG_Q) {
        return;
    }
    diag_q[(diag_head + diag_count) % DIAG_Q] = { VUK_DIAG(n), value };
    diag_count++;
}

static void diag_flush() {
    // 3 per tick leaves room for the regular inputs in the same monitor report
    for (int i = 0; (i < 3) && (diag_count > 0); i++) {
        if (monitor_enabled) {
            monitor_usage(diag_q[diag_head].usage, diag_q[diag_head].value, 0);
        }
        diag_head = (diag_head + 1) % DIAG_Q;
        diag_count--;
    }
}

// 0xFFF405nn: config descriptor as TinyUSB saw it, per interface, from the probe driver:
//   interface  itf<<24 | class<<16 | subclass<<8 | protocol
//   next word  0x7F<<24 | bNumEndpoints<<16 | bytes available to the driver
//   HID desc   0x21<<24 | bNumDescriptors<<16 | wReportLength
//   endpoint   0x05<<24 | bEndpointAddress<<16 | xfer type<<12 | wMaxPacketSize
//   other      type<<24 | length<<16
#define PROBE_LOG 48
static int32_t probe_log[PROBE_LOG];
static uint8_t probe_log_n = 0;

static void probe(uint32_t v) {
    if (probe_log_n < PROBE_LOG) {
        probe_log[probe_log_n++] = (int32_t) v;
    }
}

void vuk_probe_interface(const uint8_t* d, uint16_t max_len) {
    probe((uint32_t) d[2] << 24 | (uint32_t) d[5] << 16 | d[6] << 8 | d[7]);
    probe(0x7Fu << 24 | (uint32_t) d[4] << 16 | max_len);
    uint16_t off = d[0];
    while (off + 2 <= max_len && d[off] >= 2) {
        const uint8_t* p = d + off;
        uint8_t type = p[1];
        if (type == 0x21 && p[0] >= 9) {
            probe(0x21u << 24 | (uint32_t) p[5] << 16 | (p[7] | p[8] << 8));
        } else if (type == 0x05 && p[0] >= 7) {
            probe(0x05u << 24 | (uint32_t) p[2] << 16 | (p[3] & 3) << 12 | ((p[4] | p[5] << 8) & 0xFFF));
        } else {
            probe((uint32_t) type << 24 | (uint32_t) p[0] << 16);
        }
        off += p[0];
    }
}

#define MOUNT_LOG 32
static int32_t mount_log[MOUNT_LOG];
static uint8_t mount_log_n = 0;
static int32_t umount_log[MOUNT_LOG];
static uint8_t umount_log_n = 0;
static int32_t dev_mounts = 0;
static int32_t dev_umounts = 0;

void vuk_on_hid_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num, uint16_t desc_len) {
    if (mount_log_n < MOUNT_LOG) {
        mount_log[mount_log_n++] = (int32_t) ((uint32_t) (dev_addr & 0x7) << 28 | (uint32_t) (instance & 0xF) << 20 | (uint32_t) (itf_num & 0xF) << 16 | desc_len);
    }
}

void vuk_on_hid_umount(uint8_t dev_addr, uint8_t instance) {
    if (umount_log_n < MOUNT_LOG) {
        umount_log[umount_log_n++] = (int32_t) dev_addr << 8 | instance;
    }
}

extern "C" void tuh_mount_cb(uint8_t dev_addr) {
    dev_mounts++;
}

extern "C" void tuh_umount_cb(uint8_t dev_addr) {
    dev_umounts++;
}

#define VUK_VID 0x33E4

static bool is_vuk(uint16_t interface) {
    uint16_t vid;
    uint16_t pid;
    return tuh_vid_pid_get(interface >> 8, &vid, &pid) && (vid == VUK_VID);
}

// One target interface per attached VUK device (VID 33E4), never other devices on a hub:
// the interface that declares the report ID if there is one (wired mouse: interface 2),
// otherwise the device's first HID interface. The dongle only exposes its boot-mouse
// interface to the Feather, but it still accepts the command there.
static int send_one(const vuk_report_t& r) {
    std::map<uint8_t, uint16_t> targets;  // dev_addr -> interface
    std::map<uint8_t, bool> declared;
    if (r.type == 0) {
        for (auto const& [key, size] : out_report_sizes) {
            uint16_t itf = key >> 16;
            if (((key & 0xFF) == r.report_id) && is_vuk(itf)) {
                targets[itf >> 8] = itf;
            }
        }
        for (auto const& [dev_addr, itf] : targets) {
            queue_out_report(itf, r.report_id, r.data, r.len);
        }
        return targets.size();
    }
    for (auto const& [itf, reports] : their_feature_usages) {
        if (!is_vuk(itf)) {
            continue;
        }
        uint8_t dev_addr = itf >> 8;
        // the maps are unordered, so pick the lowest interface explicitly
        bool has = reports.count(r.report_id);
        bool better = !targets.count(dev_addr) || (has && !declared[dev_addr]) ||
                      ((has == declared[dev_addr]) && (itf < targets[dev_addr]));
        if (better) {
            targets[dev_addr] = itf;
            declared[dev_addr] = has;
        }
    }
    for (auto const& [dev_addr, itf] : targets) {
        queue_set_feature_report(itf, r.report_id, r.data, r.len);
    }
    return targets.size();
}

static int last_sent = 0;

static void diag_all() {
    diag(1, trigger_count);
    diag(2, last_sent);
    diag(5, dev_mounts);
    diag(6, dev_umounts);
    uint32_t k = 0;
    for (auto const& [itf, reports] : their_feature_usages) {
        if (reports.empty()) {
            diag(0x100 + (k++ & 0xFF), (int32_t) itf << 8 | 0xFF);
        }
        for (auto const& [report_id, usages] : reports) {
            diag(0x100 + (k++ & 0xFF), (int32_t) itf << 8 | report_id);
        }
    }
    for (uint8_t i = 0; i < mount_log_n; i++) {
        diag(0x300 + i, mount_log[i]);
    }
    for (uint8_t i = 0; i < probe_log_n; i++) {
        diag(0x500 + i, probe_log[i]);
    }
    for (uint8_t i = 0; i < umount_log_n; i++) {
        diag(0x400 + i, umount_log[i]);
    }
}

// With a hub, the dongle and the wired mouse each get it (whichever is live acts on it,
// and both stay in sync).
static void send_all(const vuk_report_t* reports, size_t n) {
    int sent = 0;
    for (size_t i = 0; i < n; i++) {
        if (reports[i].len != 0) {
            sent += send_one(reports[i]);
        }
    }
    last_sent = sent;
    diag_all();
}

void vuk_on_set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint16_t len) {
    diag(0x200 + (completions++ & 0xFF), (int32_t) dev_addr << 16 | instance << 8 | (len & 0xFF));
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
    // Also dump everything every 3 s while the Monitor tab is open, so a device that
    // never mounts (no Mid+Left possible) still shows its enumeration trace.
    static uint32_t last_dump_us = 0;
    uint32_t now = time_us_32();
    if (monitor_enabled && (diag_count == 0) && (now - last_dump_us > 3000000)) {
        last_dump_us = now;
        diag_all();
    }
    diag_flush();
}
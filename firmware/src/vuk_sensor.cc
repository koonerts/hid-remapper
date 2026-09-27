#include "vuk_sensor.h"

#include <cstddef>
#include <cstdlib>

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

// 0xFFF406ss: PC-like enumeration trace (patches/tinyusb-pc-enum.patch), last device enumerated:
//   01 VID<<16|PID   02 bNumInterfaces<<16|wTotalLength   1x string read: result<<16|bytes
#define ENUM_LOG 8
static uint8_t enum_stage[ENUM_LOG];
static uint32_t enum_value[ENUM_LOG];
static uint8_t enum_log_n = 0;

// 0xFFF407nn: every enumeration step, ring of the last 24 (nn = seq % 24):
//   seq<<24 | next state<<16 | previous transfer result<<12 | bytes
//   states: 3 addr0 dev desc, 7 set addr, 8 dev desc, 9 cfg 255, 10 full cfg, 11-13 strings,
//   14 set config, 15 config drivers; result 0 ok, 1 failed, 2 stalled, 3 timeout
#define TRANS_LOG 24
static uint32_t trans_log[TRANS_LOG];
static uint32_t trans_seq = 0;

extern "C" void tuh_enum_diag_cb(uint8_t stage, uint32_t value) {
    if (stage & 0x80) {
        trans_log[trans_seq % TRANS_LOG] = (trans_seq & 0xFF) << 24 | (uint32_t) (stage & 0x7F) << 16 | (value & 0xFFFF);
        trans_seq++;
        return;
    }
    if (stage == 1) {
        enum_log_n = 0;
    }
    if (enum_log_n < ENUM_LOG) {
        enum_stage[enum_log_n] = stage;
        enum_value[enum_log_n++] = value;
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
            // ones just STALL (shows up as 0 bytes in 0xFFF402nn).
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
    for (uint8_t i = 0; i < enum_log_n; i++) {
        diag(0x600 + enum_stage[i], (int32_t) enum_value[i]);
    }
    for (uint8_t i = 0; i < probe_log_n; i++) {
        diag(0x500 + i, probe_log[i]);
    }
    for (uint8_t i = 0; i < umount_log_n; i++) {
        diag(0x400 + i, umount_log[i]);
    }
    uint32_t n = trans_seq < TRANS_LOG ? trans_seq : TRANS_LOG;
    for (uint32_t i = trans_seq - n; i < trans_seq; i++) {
        diag(0x700 + (i % TRANS_LOG), (int32_t) trans_log[i % TRANS_LOG]);
    }
}

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
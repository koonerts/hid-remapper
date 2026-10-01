#include "vuk_sensor.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
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
//   0xFFF40003  DPI chord count (register 2 rising edges)
//   0xFFF40005  device-level mounts (tuh_mount_cb) since boot
//   0xFFF40006  device-level unmounts since boot
//   0xFFF401nn  known interface/feature report pair nn: (dev_addr<<8|itf) << 8 | report ID (0xFF = none)
//   0xFFF402nn  SET_REPORT completion nn (per chord): dev_addr<<16 | idx<<8 | bytes (0 = device rejected it)
//   0xFFF403nn  HID interface mount nn: dev_addr<<28 | idx<<20 | bInterfaceNumber<<16 | report descriptor length
//   0xFFF404nn  HID interface unmount nn: dev_addr<<8 | idx
//   0xFFF40600  DPI: stage read from the mouse (0 = read failed)
//   0xFFF40601  DPI: stage count read from the mouse (0 = read failed, VUK_DPI_STAGES used)
//   0xFFF40602  DPI: stage written
//   0xFFF40603  DPI: failures, bit 0 stage read, bit 1 table read, bit 2 timeout,
//               bit 3 reply stayed "pending" (a0), bit 4 reads skipped for this interface
//   0xFFF40604  DPI: stage writes queued (one per device)
//   0xFFF40605  DPI: interface read from, dev_addr<<8 | idx
//   0xFFF40606  DPI: GET_FEATUREs issued
//   0xFFF40607  DPI: bytes 1..4 of the last finished stage-count reply
//   0xFFF4061n, 0xFFF4062n  DPI: GET_FEATURE reply n: byte0<<24 | byte5<<16 | byte6<<8 | byte7 (-1 = failed)
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

static uint16_t dpi_noread_itf = 0;  // DPI: interface whose replies never finished; reads skipped

void vuk_on_hid_umount(uint8_t dev_addr, uint8_t instance) {
    if (dpi_noread_itf == ((uint16_t) (dev_addr << 8 | instance))) {
        dpi_noread_itf = 0;  // the address can come back as a different device
    }
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
static std::map<uint8_t, uint16_t> feature_targets(uint8_t report_id, std::map<uint8_t, bool>& declared) {
    std::map<uint8_t, uint16_t> targets;  // dev_addr -> interface
    for (auto const& [itf, reports] : their_feature_usages) {
        if (!is_vuk(itf)) {
            continue;
        }
        uint8_t dev_addr = itf >> 8;
        // the maps are unordered, so pick the lowest interface explicitly
        bool has = reports.count(report_id);
        bool better = !targets.count(dev_addr) || (has && !declared[dev_addr]) ||
                      ((has == declared[dev_addr]) && (itf < targets[dev_addr]));
        if (better) {
            targets[dev_addr] = itf;
            declared[dev_addr] = has;
        }
    }
    return targets;
}

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
    targets = feature_targets(r.report_id, declared);
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

// DPI stage cycle. On a register 2 rising edge, read the active sensor's stage and stage count
// from one VUK interface (the wired mouse when it's attached), then write stage+1 (wrapping)
// to every attached VUK device. The web driver also writes the DPI table back after a stage
// change; that isn't needed (stage-only writes verified 2026-09-30) and isn't done, since the
// table read is inferred and its reply isn't fully decoded.
// The stage written is absolute, so the dongle and the cable both getting it is harmless.
// Replies are handled in the tick, never inside the USB callback.
// Through the dongle a reply first comes back as a0 + the echoed command and no data (seen
// 2026-09-30); a1 is the finished reply. On a0 only the GET is repeated: re-sending the
// request would restart the dongle's round trip to the mouse.
#define VUK_DPI_STAGES 4  // used when the stage count can't be read
#define DPI_MAX_STAGES 7
#define DPI_TIMEOUT_US 400000
#define DPI_RETRY_US 4000
#define DPI_TRIES 3
#define DPI_POLL_US 8000
#define DPI_POLL_BUDGET_US 120000  // per read; a device that never finishes is skipped next time

enum class DpiPhase : uint8_t {
    IDLE,
    STAGE,
    TABLE,
};

static DpiPhase dpi_phase = DpiPhase::IDLE;
static int32_t dpi_prev_trigger = 0;
static int32_t dpi_trigger_count = 0;
static uint16_t dpi_itf = 0;
static uint8_t dpi_tries = 0;
static bool dpi_waiting = false;   // a GET_FEATURE for dpi_itf is queued or in flight
static uint32_t dpi_retry_at = 0;  // 0 = no retry scheduled
static uint32_t dpi_deadline = 0;
static int dpi_stage = 0;
static int dpi_count = 0;
static int32_t dpi_table_echo = 0;  // bytes 1..4 of the last stage-count reply (diagnostics)
static int dpi_last_written[2] = { 0, 0 };  // per sensor (front, rear); fallback when the stage read fails
static int32_t dpi_fail = 0;
static uint32_t dpi_replies = 0;
static int32_t dpi_gets = 0;
static bool dpi_get_only = false;   // the scheduled retry repeats just the GET
static uint32_t dpi_read_started = 0;

static uint8_t dpi_reply[64];
static uint16_t dpi_reply_len = 0;
static bool dpi_reply_ready = false;

void vuk_on_get_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, const uint8_t* report, uint16_t len) {
    if (!dpi_waiting || ((uint16_t) (dev_addr << 8 | instance) != dpi_itf)) {
        return;
    }
    dpi_waiting = false;
    dpi_reply_len = (len < sizeof(dpi_reply)) ? len : sizeof(dpi_reply);
    memcpy(dpi_reply, report, dpi_reply_len);
    dpi_reply_ready = true;
}

static const vuk_report_t& dpi_request() {
    return (dpi_phase == DpiPhase::STAGE) ? VUK_DPI_READ_STAGE[0] : VUK_DPI_READ_TABLE[0];
}

static void dpi_send_get() {
    queue_get_feature_report(dpi_itf, dpi_request().report_id, 64);
    dpi_gets++;
    dpi_waiting = true;
}

static void dpi_send_request() {
    const vuk_report_t& r = dpi_request();
    queue_set_feature_report(dpi_itf, r.report_id, r.data, r.len);
    dpi_read_started = time_us_32();
    dpi_send_get();
}

static void dpi_finish() {
    int& last = dpi_last_written[front_selected ? 0 : 1];
    int n = ((dpi_count >= 1) && (dpi_count <= DPI_MAX_STAGES)) ? dpi_count : VUK_DPI_STAGES;
    int cur = ((dpi_stage >= 1) && (dpi_stage <= n)) ? dpi_stage
              : ((last >= 1) && (last <= n))         ? last
                                                     : 1;
    int next = cur % n + 1;
    vuk_report_t w = VUK_DPI_WRITE_STAGE[0];
    w.data[7] = next;
    int sent = send_one(w);
    last = next;
    diag(3, dpi_trigger_count);
    diag(0x600, dpi_stage);
    diag(0x601, dpi_count);
    diag(0x602, next);
    diag(0x603, dpi_fail);
    diag(0x604, sent);
    diag(0x605, dpi_itf);
    diag(0x606, dpi_gets);
    diag(0x607, dpi_table_echo);
    dpi_phase = DpiPhase::IDLE;
    dpi_waiting = false;
    dpi_retry_at = 0;
    dpi_reply_ready = false;
}

static void dpi_handle_reply(uint32_t now) {
    const vuk_report_t& req = dpi_request();
    // The stage reply echoes the whole request header. The table reply (2026-09-30, dongle:
    // a1 .. 81 01 <count>) differs somewhere in bytes 1..4, so only status + command are checked.
    bool ok = (dpi_reply_len >= 8) && (dpi_reply[0] == 0xa1) &&
              ((dpi_phase == DpiPhase::STAGE) ? (memcmp(dpi_reply + 1, req.data + 1, 6) == 0)
                                              : ((dpi_reply[5] == req.data[5]) && (dpi_reply[6] == req.data[6])));
    if ((dpi_phase == DpiPhase::TABLE) && (dpi_reply_len >= 8) && (dpi_reply[0] == 0xa1)) {
        dpi_table_echo = (int32_t) ((uint32_t) dpi_reply[1] << 24 | dpi_reply[2] << 16 | dpi_reply[3] << 8 | dpi_reply[4]);
    }
    bool pending = (dpi_reply_len >= 8) && (dpi_reply[0] == 0xa0) && (dpi_reply[5] == req.data[5]) && (dpi_reply[6] == req.data[6]);
    diag(0x610 + (dpi_replies++ & 0x1F),
        (dpi_reply_len >= 8) ? (int32_t) ((uint32_t) dpi_reply[0] << 24 | dpi_reply[5] << 16 | dpi_reply[6] << 8 | dpi_reply[7]) : -1);
    if (!ok) {
        if (pending && ((now - dpi_read_started) < DPI_POLL_BUDGET_US)) {
            dpi_get_only = true;
            dpi_retry_at = (now + DPI_POLL_US) | 1;  // never 0, which means "none"
            return;
        }
        if (!pending && (++dpi_tries < DPI_TRIES)) {
            dpi_get_only = false;
            dpi_retry_at = (now + DPI_RETRY_US) | 1;
            return;
        }
        dpi_fail |= (dpi_phase == DpiPhase::STAGE) ? 1 : 2;
        if (pending) {
            // it accepts the request but never finishes the reply: stop asking this interface
            dpi_fail |= 8;
            dpi_noread_itf = dpi_itf;
        }
        if (dpi_phase == DpiPhase::STAGE) {
            dpi_finish();  // no stage means no point reading the table
            return;
        }
    } else if (dpi_phase == DpiPhase::STAGE) {
        dpi_stage = dpi_reply[7];
    } else {
        dpi_count = dpi_reply[7];  // range-checked in dpi_finish
    }
    if (dpi_phase == DpiPhase::STAGE) {
        dpi_phase = DpiPhase::TABLE;
        dpi_tries = 0;
        dpi_send_request();
    } else {
        dpi_finish();
    }
}

static void dpi_start(uint32_t now) {
    dpi_trigger_count++;
    std::map<uint8_t, bool> declared;
    std::map<uint8_t, uint16_t> targets = feature_targets(VUK_DPI_READ_STAGE[0].report_id, declared);
    if (targets.empty()) {
        diag(3, dpi_trigger_count);
        diag(0x604, 0);
        return;
    }
    // read from the wired mouse when it's there (the interface that declares report 0)
    dpi_itf = targets.begin()->second;
    for (auto const& [dev_addr, itf] : targets) {
        if (declared[dev_addr]) {
            dpi_itf = itf;
            break;
        }
    }
    dpi_stage = 0;
    dpi_count = 0;
    dpi_fail = 0;
    dpi_replies = 0;
    dpi_gets = 0;
    dpi_tries = 0;
    dpi_retry_at = 0;
    dpi_get_only = false;
    dpi_reply_ready = false;
    dpi_deadline = now + DPI_TIMEOUT_US;
    dpi_phase = DpiPhase::STAGE;
    if (dpi_itf == dpi_noread_itf) {
        dpi_fail = 16;  // reads skipped
        dpi_finish();
        return;
    }
    dpi_send_request();
}

static void dpi_tick(int32_t trigger, uint32_t now) {
    if ((trigger != 0) && (dpi_prev_trigger == 0) && (dpi_phase == DpiPhase::IDLE)) {
        dpi_start(now);
    }
    dpi_prev_trigger = trigger;
    if (dpi_phase == DpiPhase::IDLE) {
        return;
    }
    if (dpi_reply_ready) {
        dpi_reply_ready = false;
        dpi_handle_reply(now);
    } else if ((dpi_retry_at != 0) && ((int32_t) (now - dpi_retry_at) >= 0)) {
        dpi_retry_at = 0;
        if (dpi_get_only) {
            dpi_send_get();
        } else {
            dpi_send_request();
        }
    }
    if ((dpi_phase != DpiPhase::IDLE) && ((int32_t) (now - dpi_deadline) >= 0)) {
        dpi_fail |= 4;
        dpi_finish();
    }
}

void vuk_sensor_tick(int32_t trigger, int32_t dpi_trigger) {
    uint32_t now = time_us_32();
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
    dpi_tick(dpi_trigger, now);
    // Also dump everything every 3 s while the Monitor tab is open, so a device that
    // never mounts (no Mid+Left possible) still shows its enumeration trace.
    static uint32_t last_dump_us = 0;
    if (monitor_enabled && (diag_count == 0) && (now - last_dump_us > 3000000)) {
        last_dump_us = now;
        diag_all();
    }
    diag_flush();
}
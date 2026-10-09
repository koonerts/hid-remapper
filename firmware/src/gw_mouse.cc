#include "gw_mouse.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include <tusb.h>

#include "globals.h"
#include "gw_link.h"
#include "gw_payloads.h"
#include "out_report.h"
#include "remapper.h"
#include "pico/time.h"

// Arm's prebuilt toolchains ship libstdc++ with the verbose terminate handler, which drags in
// the 33 KB demangler (cp-demangle.o). In this copy_to_ram image that comes straight out of
// the heap and the Feather dies at boot. Ubuntu's toolchain (CI) is built without it.
namespace __gnu_cxx {
void __verbose_terminate_handler() {
    abort();
}
}  // namespace __gnu_cxx

// Diagnostics, visible on remapper.org/config's Monitor tab while it's open.
// Emitted a few per millisecond (a monitor report only holds 7 items).
//   0xFFF40001  sensor chord count (register 1 rising edges)
//   0xFFF40002  number of SET_FEATURE/OUTPUT reports queued for this chord (VUK)
//   0xFFF40003  DPI chord count (register 2 rising edges)
//   0xFFF40005  device-level mounts (tuh_mount_cb) since boot
//   0xFFF40006  device-level unmounts since boot
//   0xFFF401nn  known interface/feature report pair nn: (dev_addr<<8|itf) << 8 | report ID (0xFF = none)
//   0xFFF402nn  SET_REPORT completion nn (per chord): dev_addr<<16 | idx<<8 | bytes (0 = device rejected it)
//   0xFFF403nn  HID interface mount nn: dev_addr<<28 | idx<<20 | bInterfaceNumber<<16 | report descriptor length
//   0xFFF404nn  HID interface unmount nn: dev_addr<<8 | idx
//   0xFFF40600  VUK DPI: stage read from the mouse (0 = read failed)
//   0xFFF40601  VUK DPI: stage count read from the mouse (0 = read failed, VUK_DPI_STAGES used)
//   0xFFF40602  VUK DPI: stage written
//   0xFFF40603  VUK DPI: failures, bit 0 stage read, bit 1 table read, bit 2 timeout,
//               bit 3 reply stayed "pending" (a0), bit 4 reads skipped for this interface
//   0xFFF40604  VUK DPI: stage writes queued (one per device)
//   0xFFF40605  VUK DPI: interface read from, dev_addr<<8 | idx
//   0xFFF40606  VUK DPI: GET_FEATUREs issued
//   0xFFF40607  VUK DPI: bytes 1..4 of the last finished stage-count reply
//   0xFFF4061n, 0xFFF4062n  VUK DPI: GET_FEATURE reply n: byte0<<24 | byte5<<16 | byte6<<8 | byte7 (-1 = failed)
//   0xFFF40700  Warg DPI: stage read from the mouse, 1-based (0 = not read)
//   0xFFF40701  Warg DPI: stage written, 1-based
//   0xFFF40702  Warg: byte at address 0x0002 (a guess at the stage count; -1 = no reply)
//   0xFFF40703  Warg: reports queued by the last write (one per device)
//   0xFFF40704  Warg: failures, bit 0 stage read, bit 2 angle/position read, bit 3 a read timed out
//               (reads skipped for that device until it's replugged), bit 4 read skipped
//   0xFFF40705  Warg: device read from (dev_addr)
//   0xFFF40706  Warg: input reports with ID 8 seen during the last read
//   0xFFF40710  Warg angle: read from the mouse (0x7FFF = not read)
//   0xFFF40711  Warg angle: written
//   0xFFF40713  Warg angle: chord count since boot (steps + resets)
//   0xFFF40718  Warg position: read from the mouse (0x7FFF = not read)
//   0xFFF40719  Warg position: written
//   0xFFF4071B  Warg position: chord count since boot (steps + resets)
//   0xFFF40740  Warg LED: flashes since boot
//   0xFFF40741  Warg LED: stage whose colour slot was flashed, 1-based (0 = all stages: stage unknown)
//   0xFFF40742  Warg LED: stage colours read from the mouse (1) or the built-in GW_LED_PALETTE (0)
//   0xFFF40743  Warg LED: colour flashed (0xRRGGBB)
//   0xFFF40744  Warg: writes dropped because the write queue was full
//   0xFFF40745  Warg LED: flashes restored after an unplug
//   0xFFF4072n, 0xFFF4073n  Warg: input report n with ID 8 during a read: bytes 1..4, bytes 5..7 << 8 | length
//   0xFFF40750  Mid+Right holds (QT Py radio switches requested)
//   0xFFF40751  QT Py link: rejected frame (length shown)
//   0xFFF40752  QT Py link: unknown command
//   0xFFF407Fn  G-Wolves device n: dev_addr<<24 | family<<16 | PID (family 1 VUK, 2 Warg, 0 unknown: gets nothing)
#define GW_DIAG(n) (0xFFF40000 | (n))

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
    diag_q[(diag_head + diag_count) % DIAG_Q] = { GW_DIAG(n), value };
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

// Models, told apart by USB product ID under the G-Wolves vendor ID. Each family only gets the
// commands captured from it. Any other G-Wolves device gets nothing; it still shows up in the
// Monitor (0xFFF407Fn), so a new PID is easy to add.
//   VUK: SET_FEATURE report 0, 64 bytes. 3908 = cable, 0017 = dongle.
//   RS:  output report 8, 16 bytes. 4219 = Warg 8K cable, 3854 = "G-Wolves Receiver RS".
#define GW_VID 0x33E4
#define RS_PID_WIRED 0x4219

enum class Family : uint8_t {
    NONE = 0,
    VUK = 1,
    RS = 2,
};

static Family family_of_dev(uint8_t dev_addr) {
    uint16_t vid;
    uint16_t pid;
    if (!tuh_vid_pid_get(dev_addr, &vid, &pid) || (vid != GW_VID)) {
        return Family::NONE;
    }
    switch (pid) {
        case 0x3908:
        case 0x0017:
            return Family::VUK;
        case RS_PID_WIRED:
        case 0x3854:
            return Family::RS;
        default:
            return Family::NONE;
    }
}

static Family family_of(uint16_t interface) {
    return family_of_dev(interface >> 8);
}

static uint16_t pid_of(uint8_t dev_addr) {
    uint16_t vid = 0;
    uint16_t pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    return pid;
}

// Every HID interface the remapper parsed (some declare only inputs, some only outputs).
static std::set<uint16_t> known_interfaces() {
    std::set<uint16_t> itfs;
    for (auto const& [itf, reports] : their_usages) {
        itfs.insert(itf);
    }
    for (auto const& [itf, reports] : their_feature_usages) {
        itfs.insert(itf);
    }
    for (auto const& [key, size] : out_report_sizes) {
        itfs.insert(key >> 16);
    }
    itfs.erase(OUR_OUT_INTERFACE);
    return itfs;
}

static bool family_present(Family f) {
    for (uint16_t itf : known_interfaces()) {
        if (family_of(itf) == f) {
            return true;
        }
    }
    return false;
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

void gw_probe_interface(const uint8_t* d, uint16_t max_len) {
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

static void rs_forget(uint8_t dev_addr, bool mounted);

void gw_on_hid_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num, uint16_t desc_len) {
    if (mount_log_n < MOUNT_LOG) {
        mount_log[mount_log_n++] = (int32_t) ((uint32_t) (dev_addr & 0x7) << 28 | (uint32_t) (instance & 0xF) << 20 | (uint32_t) (itf_num & 0xF) << 16 | desc_len);
    }
    if (family_of_dev(dev_addr) == Family::RS) {
        rs_forget(dev_addr, true);  // the mouse may have been changed in the web app in between
    }
}

static uint16_t dpi_noread_itf = 0;  // VUK DPI: interface whose replies never finished; reads skipped

void gw_on_hid_umount(uint8_t dev_addr, uint8_t instance) {
    if (dpi_noread_itf == ((uint16_t) (dev_addr << 8 | instance))) {
        dpi_noread_itf = 0;  // the address can come back as a different device
    }
    rs_forget(dev_addr, false);  // VID/PID may already be gone here, so for any device
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

// ---- VUK ------------------------------------------------------------------------------------

static bool front_selected = true;  // VUK default; one extra chord re-syncs if wrong
static int32_t trigger_count = 0;
static uint32_t completions = 0;

static bool is_vuk(uint16_t interface) {
    return family_of(interface) == Family::VUK;
}

// One target interface per attached VUK device, never other devices on a hub:
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

static int send_one(const gw_report_t& r) {
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
    std::set<uint8_t> devs;
    for (uint16_t itf : known_interfaces()) {
        devs.insert(itf >> 8);
    }
    k = 0;
    for (uint8_t dev_addr : devs) {
        uint16_t vid = 0;
        uint16_t pid = 0;
        if (tuh_vid_pid_get(dev_addr, &vid, &pid) && (vid == GW_VID)) {
            diag(0x7F0 + (k++ & 0xF), (int32_t) dev_addr << 24 | (int32_t) family_of_dev(dev_addr) << 16 | pid);
        }
    }
}

// With a hub, the dongle and the wired mouse each get it (whichever is live acts on it,
// and both stay in sync).
static void send_all(const gw_report_t* reports, size_t n) {
    int sent = 0;
    for (size_t i = 0; i < n; i++) {
        if (reports[i].len != 0) {
            sent += send_one(reports[i]);
        }
    }
    last_sent = sent;
    diag_all();
}

void gw_on_set_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, uint16_t len) {
    diag(0x200 + (completions++ & 0xFF), (int32_t) dev_addr << 16 | instance << 8 | (len & 0xFF));
}

// VUK DPI stage cycle. On a register 2 rising edge, read the active sensor's stage and stage
// count from one VUK interface (the wired mouse when it's attached), then write stage+1
// (wrapping) to every attached VUK device. The web driver also writes the DPI table back after
// a stage change; that isn't needed (stage-only writes verified 2026-09-30) and isn't done,
// since the table read is inferred and its reply isn't fully decoded.
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

void gw_on_get_report_complete(uint8_t dev_addr, uint8_t instance, uint8_t report_id, const uint8_t* report, uint16_t len) {
    if (!dpi_waiting || ((uint16_t) (dev_addr << 8 | instance) != dpi_itf)) {
        return;
    }
    dpi_waiting = false;
    dpi_reply_len = (len < sizeof(dpi_reply)) ? len : sizeof(dpi_reply);
    memcpy(dpi_reply, report, dpi_reply_len);
    dpi_reply_ready = true;
}

static const gw_report_t& dpi_request() {
    return (dpi_phase == DpiPhase::STAGE) ? VUK_DPI_READ_STAGE[0] : VUK_DPI_READ_TABLE[0];
}

static void dpi_send_get() {
    queue_get_feature_report(dpi_itf, dpi_request().report_id, 64);
    dpi_gets++;
    dpi_waiting = true;
}

static void dpi_send_request() {
    const gw_report_t& r = dpi_request();
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
    gw_report_t w = VUK_DPI_WRITE_STAGE[0];
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
    const gw_report_t& req = dpi_request();
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
    std::map<uint8_t, bool> declared;
    std::map<uint8_t, uint16_t> targets = feature_targets(VUK_DPI_READ_STAGE[0].report_id, declared);
    if (targets.empty()) {
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

static void dpi_tick(uint32_t now) {
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

// ---- Warg 8K ("RS" protocol, see gw_payloads.h) ---------------------------------------------
// Writes go to every attached RS device (cable and dongle; the values are absolute, so a
// duplicate is harmless), through a write queue paced by the room left in the out-report queue
// (8 reports). Reads go to one device, the cable when it's attached. The reply is expected as
// input report 8 echoing the address and length; that is a guess (no reply was captured yet),
// so a read that doesn't come back in time falls back: after power-up or a replug the first DPI
// step assumes stage 1, a setting assumes its home, and the LED uses the built-in palette.
#define RS_REPORT_ID 0x08
#define RS_LEN 16
#define RS_WRITE 0x07
#define RS_READ 0x08
#define RS_ADDR_DPI_STAGE 0x0004
#define RS_ADDR_DPI_COUNT 0x0002  // guess; only shown in the Monitor
#define RS_ADDR_LED_SLOT 0x002c   // + 4 per stage: R, G, B, then a byte making the 4 sum to 0x55
#define RS_ADDR_DPI_TABLE 0x1b00  // + 6 per stage: X-1, Y-1 (16-bit LE), 00, a byte making the 6 sum to 0x55
// Other settings, one byte each (warg-wired-polling+lod+anglesnap+motionsync+spdt.txt, 2026-10-09):
#define RS_ADDR_POLL 0x0000  // polling rate: 125 Hz 08, 250 04, 500 02, 1000 01, 2000 10, 4000 20, 8000 40
#define RS_ADDR_SPDT 0x0008  // SPDT switches: bit 0 left, bit 1 right
#define RS_ADDR_LOD 0x000a   // lift-off distance: 1 = 0.7 mm, 2 = 0.9, 3 = 1.2, 4 = 1.4, 5 = 1.6
#define RS_ADDR_SYNC 0x00ab  // Motion Sync 0/1
#define RS_ADDR_SNAP 0x00af  // Angle Snap 0/1
#define RS_DPI_STAGES 5  // as configured on the Warg (1600/5000/10000/20000/40000); not read yet
#define RS_DPI_MIN 50
#define RS_DPI_MAX 50000
// Defaults for the settings the QT Py page can change (gw_tune).
// Sensor angle in degrees. Mid+Left (tap) goes back to GW_ANGLE_HOME, the angle the Warg is set
// to in the web app (2026-10-01); it's also assumed after a replug when the mouse doesn't answer.
#define GW_ANGLE_HOME (-13)
#define RS_ANGLE_STEP 1
#define RS_ANGLE_MIN (-30)  // the web app's range on G-Wolves' 8K mice
#define RS_ANGLE_MAX 30
// Virtual sensor position, web app range -100..101. Mid+Left held for the position-reset hold goes
// back to GW_POS_HOME; steps move to the next multiple of the position step.
#define GW_POS_HOME 0
#define GW_POS_RESET_HOLD_MS 500
#define RS_POS_STEP 5
#define RS_POS_MIN (-100)
#define RS_POS_MAX 101
// LED flash: a chord briefly puts a colour in the current DPI stage's colour slot, then the
// stage's own colour comes back. The colours stay clear of the stage colours (red, orange,
// yellow, green, purple).
#define GW_LED_POS 0x00ffffu       // cyan: sensor position step (Mid+Back / Mid+Fwd)
#define GW_LED_ANGLE 0x0000ffu     // blue: sensor angle step (Mid+wheel)
#define GW_LED_RESET 0xffffffu     // white: angle reset (Mid+Left tap) or position reset (hold)
#define GW_LED_LIMIT 0x000000u     // off: a step at the end of the range, nothing changed
#define GW_LED_RADIO_ON 0x0080ffu  // azure: the QT Py's radio came on (Mid+Right hold); off = GW_LED_LIMIT
#define GW_LED_FLASH_MS 1200
#define GW_LED_RESTORE_DELAY_US 300000  // after a replug, so all the mouse's interfaces are up
// The stage colours as set in the web app (2026-10-02). Used to put a slot back when the mouse
// doesn't answer reads; when it does, the real colours are read once per plug-in instead. The QT Py
// page keeps it current (SET_PALETTE / SET_PALETTE_DEFAULT).
static uint32_t gw_palette_default[RS_DPI_STAGES] = { 0xff0000, 0xff8000, 0xffff00, 0x00ff00, 0xff00ff };
#define RS_NONE 0x7FFF
#define RS_READ_TIMEOUT_US 150000
#define RS_NOREAD_HOLD_US 1000000  // after a read timed out, input from the device this much later means it's awake
#define RS_SETTLE_US 40000  // one write for a quick run of presses or notches
#define RS_INPUT_LOG 8

struct GwTune {
    uint8_t angle_step;
    uint8_t pos_step;
    int8_t angle_home;
    int8_t pos_home;
    uint16_t flash_ms;
    uint16_t pos_hold_ms;    // Mid+Left hold: position reset
    uint16_t wheel_hold_ms;  // Middle down this long before the wheel steps the angle
    uint16_t radio_hold_ms;  // Mid+Right hold: QT Py radio on/off
};
static GwTune tune = { RS_ANGLE_STEP, RS_POS_STEP, GW_ANGLE_HOME, GW_POS_HOME, GW_LED_FLASH_MS,
    GW_POS_RESET_HOLD_MS, GW_WHEEL_HOLD_US / 1000, GW_RADIO_HOLD_MS };

static void rs_build(uint8_t* r, uint8_t cmd, uint16_t addr, const uint8_t* data, uint8_t len) {
    memset(r, 0, RS_LEN);
    r[0] = cmd;
    r[2] = addr >> 8;
    r[3] = addr & 0xFF;
    r[4] = len;
    if (data != NULL) {
        memcpy(r + 5, data, len);
    }
    uint8_t sum = RS_REPORT_ID;
    for (int i = 0; i < RS_LEN - 1; i++) {
        sum += r[i];
    }
    r[RS_LEN - 1] = 0x55 - sum;
}

// One interface per RS device: the one that declares output report 8, else its lowest.
static std::map<uint8_t, uint16_t> rs_targets() {
    std::map<uint8_t, uint16_t> targets;  // dev_addr -> interface
    std::map<uint8_t, bool> declared;
    for (uint16_t itf : known_interfaces()) {
        if (family_of(itf) != Family::RS) {
            continue;
        }
        uint8_t dev_addr = itf >> 8;
        bool has = out_report_sizes.count((uint32_t) itf << 16 | RS_REPORT_ID) > 0;
        bool better = !targets.count(dev_addr) || (has && !declared[dev_addr]) ||
                      ((has == declared[dev_addr]) && (itf < targets[dev_addr]));
        if (better) {
            targets[dev_addr] = itf;
            declared[dev_addr] = has;
        }
    }
    return targets;
}

static uint8_t rs_read_target() {
    uint8_t best = 0;
    for (auto const& [dev_addr, itf] : rs_targets()) {
        if ((best == 0) || (pid_of(dev_addr) == RS_PID_WIRED)) {
            best = dev_addr;
        }
    }
    return best;
}

// ---- write queue

struct RsWrite {
    uint16_t addr;
    uint8_t len;
    uint8_t data[6];
};
#define RS_WQ 40
static RsWrite rs_wq[RS_WQ];
static uint8_t rs_wq_head = 0;
static uint8_t rs_wq_n = 0;
static int32_t rs_wq_drops = 0;

static bool rs_refresh_reading();  // a refresh read is in flight (defined with the reads below)
static void rs_mark_stale();

static void rs_write(uint16_t addr, const uint8_t* data, uint8_t len) {
    if (rs_refresh_reading()) {
        rs_mark_stale();
    }
    if (rs_wq_n == RS_WQ) {
        rs_wq_drops++;
        diag(0x744, rs_wq_drops);
        return;
    }
    RsWrite& w = rs_wq[(rs_wq_head + rs_wq_n) % RS_WQ];
    w.addr = addr;
    w.len = len;
    memcpy(w.data, data, len);
    rs_wq_n++;
}

// a one-byte setting, stored as (v, 0x55 - v)
static void rs_write_value(uint16_t addr, uint8_t v) {
    uint8_t d[2] = { v, (uint8_t) (0x55 - v) };
    rs_write(addr, d, 2);
}

static void rs_write_colour(uint8_t stage, uint32_t rgb) {
    uint8_t d[4] = { (uint8_t) (rgb >> 16), (uint8_t) (rgb >> 8), (uint8_t) rgb, 0 };
    d[3] = 0x55 - (uint8_t) (d[0] + d[1] + d[2]);
    rs_write(RS_ADDR_LED_SLOT + 4 * stage, d, 4);
}

static void rs_write_dpi(uint8_t stage, uint16_t x, uint16_t y) {
    uint8_t d[6] = { (uint8_t) (x - 1), (uint8_t) ((x - 1) >> 8), (uint8_t) (y - 1), (uint8_t) ((y - 1) >> 8), 0, 0 };
    d[5] = 0x55 - (uint8_t) (d[0] + d[1] + d[2] + d[3] + d[4]);
    rs_write(RS_ADDR_DPI_TABLE + 6 * stage, d, 6);
}

// Sends queued writes while the out-report queue has room for one per RS device.
static void rs_pump() {
    if (rs_wq_n == 0) {
        return;
    }
    std::map<uint8_t, uint16_t> targets = rs_targets();
    if (targets.empty()) {
        rs_wq_n = 0;  // nobody to send to (an interrupted LED flash is handled on the next mount)
        return;
    }
    for (int k = 0; (k < 2) && (rs_wq_n > 0) && (out_report_free_slots() >= targets.size()); k++) {
        const RsWrite& w = rs_wq[rs_wq_head];
        uint8_t r[RS_LEN];
        rs_build(r, RS_WRITE, w.addr, w.data, w.len);
        for (auto const& [dev_addr, itf] : targets) {
            queue_out_report(itf, RS_REPORT_ID, r, RS_LEN);
        }
        rs_wq_head = (rs_wq_head + 1) % RS_WQ;
        rs_wq_n--;
    }
}

// ---- settings (angle, position)

static int clamp(int v, int lo, int hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

// angle: a signed byte
static int angle_decode(int b) {
    int a = (int8_t) b;
    return ((a >= RS_ANGLE_MIN) && (a <= RS_ANGLE_MAX)) ? a : RS_NONE;
}
static uint8_t angle_encode(int a) {
    return (uint8_t) (int8_t) a;
}
static int angle_step(int a, int dir) {
    return clamp(a + dir * tune.angle_step, RS_ANGLE_MIN, RS_ANGLE_MAX);
}

// position: byte = -p for p <= 0, 100 + p for p > 0
static int pos_decode(int b) {
    return (b <= 100) ? -b : (b <= 100 + RS_POS_MAX) ? b - 100 : RS_NONE;
}
static uint8_t pos_encode(int p) {
    return (p <= 0) ? -p : 100 + p;
}
// up: the next multiple of the step above, down: the next one below
static int pos_step(int p, int dir) {
    int st = tune.pos_step;
    int floor_ = (p >= 0) ? p / st : -((-p + st - 1) / st);
    int ceil_ = (p >= 0) ? (p + st - 1) / st : -((-p) / st);
    return clamp((dir > 0) ? (floor_ + 1) * st : (ceil_ - 1) * st, RS_POS_MIN, RS_POS_MAX);
}

// A one-byte Warg setting the chords change. The web driver writes 01 to on_addr right before
// every value write, so the firmware does too.
struct RsSetting {
    uint16_t on_addr;
    uint16_t addr;
    int home;  // reset target, also assumed when the mouse doesn't answer a read
    int lo;
    int hi;
    int (*decode)(int b);  // RS_NONE if out of range
    uint8_t (*encode)(int v);
    int (*step)(int v, int dir);
    uint16_t diag;  // read at diag, written at diag + 1, chord count at diag + 3
    int value;
    bool known;   // read, reset, or assumed after a failed read
    bool wanted;  // a read is needed before the pending steps can apply
    int sent;
    int pending;  // steps that came in before the value was known
    uint32_t changed_at;
    int32_t ops;
};

#define RS_ANGLE 0
#define RS_POS 1
#define RS_SETTINGS 2
static RsSetting rs_settings[RS_SETTINGS] = {
    { 0x00bf, 0x00bd, GW_ANGLE_HOME, RS_ANGLE_MIN, RS_ANGLE_MAX, angle_decode, angle_encode, angle_step, 0x710, GW_ANGLE_HOME, false, false, RS_NONE, 0, 0, 0 },
    { 0x1b48, 0x1b4a, GW_POS_HOME, RS_POS_MIN, RS_POS_MAX, pos_decode, pos_encode, pos_step, 0x718, GW_POS_HOME, false, false, RS_NONE, 0, 0, 0 },
};

// ---- LED flash

struct RsLed {
    bool requested;  // a flash is waiting for its reads (palette, stage)
    bool active;     // the flash colour is in the slot(s)
    uint32_t colour;
    uint32_t shown;
    uint32_t until;
    int8_t stage;  // slot flashed, -1 = every stage (current stage unknown)
    bool palette_done;  // read (or given up) since the mouse was plugged in
    bool palette_read;  // the palette came from the mouse
    uint8_t palette_next;
    bool stage_done;  // stage read (or given up) for this flash
    uint32_t palette[RS_DPI_STAGES];
    bool restore_pending;  // a flash was cut off by an unplug: put the slot(s) back on the next mount
    int8_t restore_stage;
    uint32_t restore_rgb[RS_DPI_STAGES];
    uint32_t restore_at;  // 0 = waiting for a mount
    int32_t flashes;
    int32_t restored;
};
static RsLed led = {};
static int rs_cur_stage = -1;  // 0-based; from a read or the firmware's own stage write; -1 = unknown

// DPI table: what the firmware last read or wrote (for the QT Py page); bit n of the mask = stage n known
static uint16_t rs_dpi_x[RS_DPI_STAGES];
static uint16_t rs_dpi_y[RS_DPI_STAGES];
static uint8_t rs_dpi_known = 0;

static void led_palette_reset() {
    memcpy(led.palette, gw_palette_default, sizeof(led.palette));
    led.palette_done = false;
    led.palette_read = false;
    led.palette_next = 0;
}

static void led_write(int8_t stage, const uint32_t* rgb_per_stage, uint32_t rgb_all) {
    for (uint8_t s = 0; s < RS_DPI_STAGES; s++) {
        if ((stage < 0) || (stage == s)) {
            rs_write_colour(s, (rgb_per_stage != NULL) ? rgb_per_stage[s] : rgb_all);
        }
    }
}

static void led_restore_now() {
    if (!led.active) {
        return;
    }
    led_write(led.stage, led.palette, 0);
    led.active = false;
}

static void rs_led_flash(uint32_t colour, uint32_t now) {
    led.flashes++;
    led.colour = colour;
    led.until = now + tune.flash_ms * 1000u;
    if (led.active) {
        if (colour != led.shown) {
            led_write(led.stage, NULL, colour);
            led.shown = colour;
            diag(0x743, (int32_t) colour);
        }
        return;
    }
    if (!led.requested) {
        led.requested = true;
        led.stage_done = false;
    }
}

static void led_apply(uint32_t now) {
    led.requested = false;
    led.stage = (rs_cur_stage >= 0) && (rs_cur_stage < RS_DPI_STAGES) ? rs_cur_stage : -1;
    led_write(led.stage, NULL, led.colour);
    led.shown = led.colour;
    led.active = true;
    led.until = now + tune.flash_ms * 1000u;
    diag(0x740, led.flashes);
    diag(0x741, led.stage + 1);
    diag(0x742, led.palette_read);
    diag(0x743, (int32_t) led.colour);
}

// ---- reads

enum class RsRead : uint8_t {
    NONE,
    DPI_STAGE,
    DPI_COUNT,
    SETTING,
    LED_SLOT,
    LED_STAGE,
    REFRESH,
    INFO,  // the status query (command 04), last item of a refresh
    RAW,   // a probe from the QT Py (LINK_RAW)
};

// The QT Py page's "refresh": stage, angle, position, the 5 colours, the 5 DPI table entries.
// Then the five option bytes (rs_opt_addr order), then the status query.
#define RS_OPTS 5
#define RS_REFRESH_ITEMS (3 + 2 * RS_DPI_STAGES + RS_OPTS + 1)
static const uint16_t rs_opt_addr[RS_OPTS] = { RS_ADDR_POLL, RS_ADDR_LOD, RS_ADDR_SNAP, RS_ADDR_SYNC, RS_ADDR_SPDT };
static uint8_t rs_opt_val[RS_OPTS];
static uint8_t rs_opt_known = 0;  // bit n = rs_opt_val[n] read from or written to the mouse


// only the values the web driver itself writes; reading, SPDT 0 (both off) is accepted too, since the
// web driver may write it although the capture never had it
static bool rs_opt_ok(uint8_t k, uint8_t v, bool write = true) {
    switch (k) {
        case 0: return (v == 0x01) || (v == 0x02) || (v == 0x04) || (v == 0x08) || (v == 0x10) || (v == 0x20) || (v == 0x40);
        case 1: return (v >= 1) && (v <= 5);
        case 2:
        case 3: return v <= 1;
        case 4: return write ? ((v >= 1) && (v <= 3)) : (v <= 3);
        default: return false;
    }
}

// The web driver's status query (command 04, no address, no data), sent on its "Refresh"; the battery
// level is somewhere in the reply (warg-wired-battery-check-by-clicking-refresh.txt, 2026-10-09: the
// replies weren't captured). The reply goes to the QT Py as it is, to be decoded there.
#define RS_INFO_CMD 0x04
#define RS_RAW_MAX 24
#define RS_RAW_TIMEOUT_US 250000
static uint8_t rs_info[RS_RAW_MAX];  // last reply to 04, report ID first
static uint8_t rs_info_len = 0;      // 0 = none since the mouse was plugged in
static uint8_t rs_info_seq = 0;
// A probe from the QT Py: one read (08) or status query (04), never a write; the reply is passed back.
// The Feather rebuilds the report itself from the command, address and length.
static uint8_t rs_raw_req[RS_LEN];
static bool rs_raw_pending = false;
static uint8_t rs_raw[RS_RAW_MAX];
static uint8_t rs_raw_len = 0;
static uint8_t rs_raw_seq = 0;
static uint8_t rs_raw_state = 0;  // 0 none, 1 waiting, 2 answered, 3 no answer (refusals don't change it)
static uint8_t rs_raw_refused = 0;           // probes refused (malformed, not allowed, or one already running)
static uint8_t rs_raw_cmd = 0, rs_raw_rlen = 0;
static uint16_t rs_raw_addr = 0;             // the probe rs_raw* describes
static uint8_t rs_cap_rejects = 0;           // report 8s echoing the command but failing the checks
// what the reply to an INFO/RAW exchange must echo in its first data byte, and where it's kept meanwhile
static uint8_t rs_cap_cmd = 0;
static uint8_t rs_cap[RS_RAW_MAX];
static uint8_t rs_cap_len = 0;

static RsRead rs_read = RsRead::NONE;
static uint8_t rs_read_setting = 0;
static uint8_t rs_read_dev = 0;
static uint16_t rs_read_addr = 0;
static uint8_t rs_read_len = 2;
static uint32_t rs_deadline = 0;
// A read from this device timed out (a wireless mouse that's asleep doesn't answer), so reads to it
// are skipped. Cleared when it's replugged, when it sends input again at least RS_NOREAD_HOLD_US after
// the timeout (it's awake), and by the QT Py's "read settings" (REFRESH).
static uint8_t rs_noread_dev = 0;
static uint32_t rs_noread_at = 0;
static uint8_t rs_reply[6];
static bool rs_reply_ready = false;
static int32_t rs_inputs = 0;
static int32_t rs_fail = 0;
static bool rs_reads_worked = false;  // a read was answered since the mouse was plugged in
static int8_t rs_refresh_next = -1;   // next refresh item; -1 = no refresh running
static int8_t rs_refresh_item = -1;   // the item whose read is in flight; -1 = none (or cancelled)
// A write queued while a refresh read is in flight reaches the mouse after the read, so that reply holds
// the old value: it is dropped and the item read again.
static bool rs_refresh_stale = false;
// a REFRESH asked for while a read was in flight: that read timing out doesn't pause reads
static bool rs_refresh_asked = false;

static bool rs_dpi_wanted = false;
static int rs_dpi_stage = -1;  // 0-based, as read; -1 = not read
static int rs_dpi_last = -1;   // 0-based, last written; -1 = none since the mouse was plugged in

static void rs_forget(uint8_t dev_addr, bool mounted) {
    if (rs_noread_dev == dev_addr) {
        rs_noread_dev = 0;
    }
    for (RsSetting& s : rs_settings) {
        s.known = false;
        s.sent = RS_NONE;
    }
    rs_dpi_last = -1;
    rs_cur_stage = -1;
    rs_dpi_known = 0;
    rs_opt_known = 0;
    rs_info_len = 0;
    rs_reads_worked = false;
    rs_refresh_next = -1;
    rs_refresh_item = -1;  // a reply still on its way is ignored
    if (led.active && !mounted) {
        // the temporary colour may still be in the mouse: put it back once a mouse is there again
        led.restore_pending = true;
        led.restore_stage = led.stage;
        memcpy(led.restore_rgb, led.palette, sizeof(led.restore_rgb));
        led.restore_at = 0;
        led.active = false;
    }
    if (led.restore_pending && mounted) {
        led.restore_at = (time_us_32() + GW_LED_RESTORE_DELAY_US) | 1;
    }
    led.requested = false;
    led_palette_reset();
}

// Input report 8 from the device being read: is it the reply (address and length echoed, data
// bytes summing to 0x55 like every stored value, valid checksum when the report is complete)?
// The data (rs_read_len bytes) goes to out.
static bool rs_parse_reply(const uint8_t* r, uint16_t len, uint8_t* out) {
    if ((len < 6 + rs_read_len) || (r[0] != RS_REPORT_ID) || (r[3] != (rs_read_addr >> 8)) || (r[4] != (rs_read_addr & 0xFF)) || (r[5] != rs_read_len)) {
        return false;
    }
    uint8_t data_sum = 0;
    for (int i = 0; i < rs_read_len; i++) {
        data_sum += r[6 + i];
    }
    if (data_sum != 0x55) {
        return false;
    }
    if (len >= 1 + RS_LEN) {
        uint8_t sum = 0;
        for (int i = 0; i < 1 + RS_LEN; i++) {
            sum += r[i];
        }
        if (sum != 0x55) {
            return false;
        }
    }
    memcpy(out, r + 6, rs_read_len);
    return true;
}

void gw_on_input_report(uint8_t dev_addr, uint8_t instance, const uint8_t* report, uint16_t len) {
    if ((rs_noread_dev != 0) && (dev_addr == rs_noread_dev) && ((int32_t) (time_us_32() - rs_noread_at) >= RS_NOREAD_HOLD_US)) {
        rs_noread_dev = 0;  // sending again, so awake: reads are worth trying
    }
    if ((rs_read == RsRead::NONE) || (dev_addr != rs_read_dev) || (len < 8) || (report[0] != RS_REPORT_ID)) {
        return;
    }
    if ((rs_read == RsRead::INFO) || (rs_read == RsRead::RAW)) {
        // kept as it is: the first full report 8 that echoes the command (for a read, also the address
        // and length) and whose bytes sum to 0x55
        if (rs_reply_ready || (report[1] != rs_cap_cmd)) {
            return;
        }
        uint8_t sum = 0;
        for (int i = 0; (i < 1 + RS_LEN) && (i < len); i++) {
            sum += report[i];
        }
        bool echo = (rs_cap_cmd != RS_READ) || ((report[3] == (rs_read_addr >> 8)) && (report[4] == (rs_read_addr & 0xFF)) && (report[5] == rs_read_len));
        if ((len < 1 + RS_LEN) || (sum != 0x55) || !echo) {
            rs_cap_rejects++;
            return;
        }
        rs_cap_len = (len < RS_RAW_MAX) ? len : RS_RAW_MAX;
        memcpy(rs_cap, report, rs_cap_len);
        rs_reply_ready = true;
        return;
    }
    if (rs_inputs < RS_INPUT_LOG) {
        diag(0x720 + rs_inputs, (int32_t) ((uint32_t) report[1] << 24 | report[2] << 16 | report[3] << 8 | report[4]));
        diag(0x730 + rs_inputs, (int32_t) ((uint32_t) report[5] << 24 | report[6] << 16 | report[7] << 8 | (len & 0xFF)));
    }
    rs_inputs++;
    if (!rs_reply_ready && rs_parse_reply(report, len, rs_reply)) {
        rs_reply_ready = true;
    }
}

static void rs_start_read(RsRead what, uint16_t addr, uint8_t len, uint32_t now) {
    rs_refresh_asked = false;
    rs_read = what;
    rs_read_addr = addr;
    rs_read_len = len;
    rs_reply_ready = false;
    rs_inputs = 0;
    rs_deadline = now + RS_READ_TIMEOUT_US;
    std::map<uint8_t, uint16_t> targets = rs_targets();
    uint8_t r[RS_LEN];
    rs_build(r, RS_READ, addr, NULL, len);
    if (targets.count(rs_read_dev)) {
        queue_out_report(targets[rs_read_dev], RS_REPORT_ID, r, RS_LEN);
    }
}

// a status query or probe whose reply is kept as it is (r = the 16 bytes after the report ID)
static void rs_start_exchange(RsRead what, const uint8_t* r, uint32_t now) {
    rs_refresh_asked = false;
    rs_read = what;
    rs_read_dev = rs_read_target();
    rs_cap_cmd = r[0];
    rs_read_addr = (uint16_t) (r[2] << 8 | r[3]);
    rs_read_len = r[4];
    rs_reply_ready = false;
    rs_deadline = now + RS_RAW_TIMEOUT_US;
    std::map<uint8_t, uint16_t> targets = rs_targets();
    if (targets.count(rs_read_dev)) {
        queue_out_report(targets[rs_read_dev], RS_REPORT_ID, r, RS_LEN);
    }
}

// a read can go out now: none in flight, writes flushed first, room in the out-report queue
static bool rs_can_read() {
    return (rs_read == RsRead::NONE) && (rs_wq_n == 0) && (out_report_free_slots() >= 1);
}

static bool rs_reads_work() {
    uint8_t dev = rs_read_target();
    return (dev != 0) && (dev != rs_noread_dev);
}

// true if a read was started; otherwise the caller goes on without one
static bool rs_try_read(RsRead what, uint16_t addr, uint8_t len, uint32_t now) {
    rs_read_dev = rs_read_target();
    if ((rs_read_dev == 0) || (rs_read_dev == rs_noread_dev)) {
        rs_fail |= 16;
        return false;
    }
    rs_start_read(what, addr, len, now);
    return true;
}

static void rs_dpi_finish() {
    int n = RS_DPI_STAGES;
    int cur = ((rs_dpi_stage >= 0) && (rs_dpi_stage < n)) ? rs_dpi_stage
              : ((rs_dpi_last >= 0) && (rs_dpi_last < n)) ? rs_dpi_last
                                                           : 0;
    int next = (cur + 1) % n;
    rs_write_value(RS_ADDR_DPI_STAGE, next);
    rs_dpi_last = next;
    rs_cur_stage = next;
    diag(0x700, rs_dpi_stage + 1);
    diag(0x701, next + 1);
    diag(0x703, (int32_t) rs_targets().size());
    diag(0x704, rs_fail);
    diag(0x705, rs_read_dev);
    diag(0x706, rs_inputs);
}

// absolute, so it needs no read
static void rs_setting_reset(RsSetting& s, uint32_t now) {
    s.ops++;
    s.value = s.home;
    s.known = true;
    s.pending = 0;
    s.wanted = false;
    s.changed_at = now;
}

// an exact value from the QT Py page: written at once (no settle wait)
static void rs_setting_set(RsSetting& s, int v, uint32_t now) {
    s.ops++;
    s.value = clamp(v, s.lo, s.hi);
    s.known = true;
    s.pending = 0;
    s.wanted = false;
    s.changed_at = now - RS_SETTLE_US;
}

// 1 = changed, 0 = already at the end of the range, -1 = not known yet (read pending)
static int rs_setting_step(RsSetting& s, int dir, uint32_t now) {
    s.ops++;
    if (!s.known) {
        s.pending += dir;
        s.wanted = true;
        return -1;
    }
    int v = s.step(s.value, dir);
    if (v == s.value) {
        return 0;
    }
    s.value = v;
    s.changed_at = now;
    return 1;
}

// raw: the byte read, -1 = no reply
static void rs_setting_read_done(RsSetting& s, int32_t raw, uint32_t now) {
    if (s.known) {
        return;  // a reset came in while reading; it wins
    }
    int v = (raw >= 0) ? s.decode(raw) : RS_NONE;
    bool ok = (v != RS_NONE);
    s.value = ok ? v : s.home;
    s.sent = ok ? v : RS_NONE;
    if (!ok) {
        rs_fail |= 4;
    }
    s.known = true;
    diag(s.diag, ok ? v : RS_NONE);
    for (; s.pending > 0; s.pending--) {
        s.value = s.step(s.value, 1);
    }
    for (; s.pending < 0; s.pending++) {
        s.value = s.step(s.value, -1);
    }
    s.changed_at = now;
}

static bool rs_refresh_reading() {
    return ((rs_read == RsRead::REFRESH) || (rs_read == RsRead::INFO)) && (rs_refresh_item >= 0);
}

static void rs_mark_stale() {
    rs_refresh_stale = true;
}

static void rs_refresh_start(int8_t item, uint32_t now) {
    rs_refresh_item = item;
    rs_refresh_stale = false;
    if (item < 3) {
        static const uint16_t addr[3] = { RS_ADDR_DPI_STAGE, 0x00bd, 0x1b4a };
        rs_try_read(RsRead::REFRESH, addr[item], 2, now);
    } else if (item < 3 + RS_DPI_STAGES) {
        rs_try_read(RsRead::REFRESH, RS_ADDR_LED_SLOT + 4 * (item - 3), 4, now);
    } else if (item < 3 + 2 * RS_DPI_STAGES) {
        rs_try_read(RsRead::REFRESH, RS_ADDR_DPI_TABLE + 6 * (item - 3 - RS_DPI_STAGES), 6, now);
    } else if (item < 3 + 2 * RS_DPI_STAGES + RS_OPTS) {
        rs_try_read(RsRead::REFRESH, rs_opt_addr[item - 3 - 2 * RS_DPI_STAGES], 2, now);
    } else {
        uint8_t r[RS_LEN];
        rs_build(r, RS_INFO_CMD, 0, NULL, 0);
        rs_start_exchange(RsRead::INFO, r, now);
    }
}

// The reply is decoded as the item that was read, whatever rs_refresh_next says now: a REFRESH that
// came in meanwhile restarted it (next = 0), and an unplug cancelled it (item = -1, reply ignored).
static void rs_refresh_done(bool ok, const uint8_t* d) {
    int8_t item = rs_refresh_item;
    rs_refresh_item = -1;
    if (item < 0) {
        return;
    }
    bool current = (rs_refresh_next == item);  // not restarted or cancelled meanwhile
    if (rs_refresh_stale) {
        rs_refresh_stale = false;
        if (ok && current) {
            rs_refresh_next = item;  // something was written meanwhile: read this item again
        } else if (!ok && current) {
            rs_refresh_next = -1;
        }
        return;
    }
    if (!ok) {
        if (current) {
            rs_refresh_next = -1;  // the mouse doesn't answer: stop here
        }
        return;
    }
    if (item == 0) {
        if (d[0] < RS_DPI_STAGES) {
            rs_cur_stage = d[0];
            rs_dpi_last = d[0];
        }
    } else if (item < 3) {
        RsSetting& s = rs_settings[item - 1];
        int v = s.decode(d[0]);
        // a change still waiting to be written wins over what's in the mouse
        if ((v != RS_NONE) && !(s.known && (s.value != s.sent))) {
            s.value = v;
            s.sent = v;
            s.known = true;
            s.pending = 0;
            s.wanted = false;
        }
    } else if (item < 3 + RS_DPI_STAGES) {
        led.palette[item - 3] = (uint32_t) d[0] << 16 | d[1] << 8 | d[2];
        if (item == 2 + RS_DPI_STAGES) {
            led.palette_done = true;
            led.palette_read = true;
        }
    } else if (item < 3 + 2 * RS_DPI_STAGES) {
        int st = item - 3 - RS_DPI_STAGES;
        rs_dpi_x[st] = (uint16_t) (d[0] | d[1] << 8) + 1;
        rs_dpi_y[st] = (uint16_t) (d[2] | d[3] << 8) + 1;
        rs_dpi_known |= 1 << st;
    } else {
        int k = item - 3 - 2 * RS_DPI_STAGES;
        if (rs_opt_ok(k, d[0], false)) {  // anything else stays unknown
            rs_opt_val[k] = d[0];
            rs_opt_known |= 1 << k;
        }
    }
    if (current) {
        rs_refresh_next = (item + 1 < RS_REFRESH_ITEMS) ? item + 1 : -1;
    }
}

static void rs_read_done(bool ok, uint32_t now) {
    RsRead what = rs_read;
    rs_read = RsRead::NONE;
    if (ok) {
        rs_reads_worked = true;
    }
    int32_t byte = ok ? rs_reply[0] : -1;
    switch (what) {
        case RsRead::DPI_STAGE:
            if (!ok) {
                rs_fail |= 1;
            }
            rs_dpi_stage = byte;  // range-checked in rs_dpi_finish
            rs_dpi_finish();
            if (ok) {
                // reads work: also show the byte at the guessed stage-count address
                rs_start_read(RsRead::DPI_COUNT, RS_ADDR_DPI_COUNT, 2, now);
            }
            break;
        case RsRead::DPI_COUNT:
            diag(0x702, byte);
            break;
        case RsRead::SETTING:
            rs_setting_read_done(rs_settings[rs_read_setting], byte, now);
            break;
        case RsRead::LED_SLOT:
            if (ok) {
                led.palette[led.palette_next] = (uint32_t) rs_reply[0] << 16 | rs_reply[1] << 8 | rs_reply[2];
                if (++led.palette_next == RS_DPI_STAGES) {
                    led.palette_done = true;
                    led.palette_read = true;
                }
            } else {
                led_palette_reset();  // keep the built-in colours, all of them
                led.palette_done = true;
            }
            break;
        case RsRead::LED_STAGE:
            led.stage_done = true;
            if (ok && (byte < RS_DPI_STAGES)) {
                rs_cur_stage = byte;
            }
            break;
        case RsRead::REFRESH:
            rs_refresh_done(ok, rs_reply);
            break;
        case RsRead::INFO: {
            int8_t item = rs_refresh_item;
            rs_refresh_item = -1;
            if (ok && (item >= 0)) {
                memcpy(rs_info, rs_cap, rs_cap_len);
                rs_info_len = rs_cap_len;
                rs_info_seq++;
            }
            if ((item >= 0) && (rs_refresh_next == item)) {
                rs_refresh_next = -1;  // the last refresh item
            }
            break;
        }
        case RsRead::RAW:
            if (ok) {
                memcpy(rs_raw, rs_cap, rs_cap_len);
                rs_raw_len = rs_cap_len;
            } else {
                rs_raw_len = 0;
            }
            rs_raw_state = ok ? 2 : 3;
            rs_raw_seq++;
            break;
        default:
            break;
    }
}

static void rs_tick(uint32_t now) {
    if (rs_read != RsRead::NONE) {
        if (rs_reply_ready) {
            rs_reply_ready = false;
            rs_read_done(true, now);
        } else if ((int32_t) (now - rs_deadline) >= 0) {
            // a status query or probe going unanswered says nothing about whether reads work
            if ((rs_read != RsRead::INFO) && (rs_read != RsRead::RAW) && !rs_refresh_asked) {
                rs_fail |= 8;
                rs_noread_dev = rs_read_dev;
                rs_noread_at = now;
            }
            rs_read_done(false, now);
        }
    }
    if (rs_can_read()) {
        if (rs_raw_pending) {
            rs_raw_pending = false;
            if (rs_read_target() != 0) {
                rs_start_exchange(RsRead::RAW, rs_raw_req, now);
            } else {
                rs_raw_state = 3;
                rs_raw_len = 0;
                rs_raw_seq++;
            }
        } else if (rs_dpi_wanted) {
            rs_dpi_wanted = false;
            rs_dpi_stage = -1;
            rs_fail = 0;
            if (!rs_try_read(RsRead::DPI_STAGE, RS_ADDR_DPI_STAGE, 2, now)) {
                rs_dpi_finish();
            }
        } else {
            bool started = false;
            for (uint8_t i = 0; (i < RS_SETTINGS) && !started; i++) {
                RsSetting& s = rs_settings[i];
                if (!s.wanted) {
                    continue;
                }
                s.wanted = false;
                // steps that came in while this setting was being read were applied with it
                if (!s.known) {
                    rs_fail = 0;
                    rs_read_setting = i;
                    if (!rs_try_read(RsRead::SETTING, s.addr, 2, now)) {
                        rs_setting_read_done(s, -1, now);
                    }
                }
                started = true;  // one read at a time
            }
            if (!started && led.requested) {
                // the stage colours once per plug-in, the current stage before each flash
                if (!led.palette_done && rs_reads_work()) {
                    rs_try_read(RsRead::LED_SLOT, RS_ADDR_LED_SLOT + 4 * led.palette_next, 4, now);
                } else if (!led.stage_done && rs_reads_work()) {
                    rs_try_read(RsRead::LED_STAGE, RS_ADDR_DPI_STAGE, 2, now);
                } else {
                    led_apply(now);
                }
                started = true;
            }
            if (!started && (rs_refresh_next >= 0)) {
                if (rs_reads_work()) {
                    rs_refresh_start(rs_refresh_next, now);
                } else {
                    rs_refresh_next = -1;
                }
            }
        }
    }
    if (led.active && ((int32_t) (now - led.until) >= 0)) {
        led_restore_now();
    }
    if (led.restore_pending && (led.restore_at != 0) && ((int32_t) (now - led.restore_at) >= 0) && !rs_targets().empty()) {
        led_write(led.restore_stage, led.restore_rgb, 0);
        led.restore_pending = false;
        led.restored++;
        diag(0x745, led.restored);
    }
    // one setting per tick
    for (RsSetting& s : rs_settings) {
        if (s.known && (s.value != s.sent) && ((now - s.changed_at) >= RS_SETTLE_US)) {
            rs_write_value(s.on_addr, 0x01);
            rs_write_value(s.addr, s.encode(s.value));
            s.sent = s.value;
            diag(s.diag + 1, s.value);
            diag(s.diag + 3, s.ops);
            diag(0x703, (int32_t) rs_targets().size());
            diag(0x704, rs_fail);
            break;
        }
    }
    rs_pump();
}

// ---- QT Py settings link (gw_link.h) ----------------------------------------------------------
// Status block the QT Py reads (GW_LINK_STATUS_LEN bytes, little-endian):
//   0 'G'   1 protocol (1)   2 link flags: bit 0 tuning set since boot, bit 1 built-in palette set
//   since boot, bit 2 refresh running, bit 3 reads paused (last one timed out: mouse asleep?)
//   3 mouse flags: bit 0 Warg, 1 VUK, 2 reads answered,
//   3 stage known, 4 angle known, 5 position known, 6 colours read from the mouse, 7 DPI table known
//   4 stage (0-based, 0xFF unknown)   5 angle (int8)   6 position (int8)   7 Mid+Right hold count
//   8-22 stage colours (R,G,B x5)   23-42 DPI table (X,Y uint16 x5)   43-54 tuning (GwTune order:
//   angle step, position step, angle home, position home, flash ms, position-reset hold ms, wheel
//   hold ms, radio hold ms)   55 last command seq applied   56 firmware version
//   57-61 option bytes as the mouse stores them (rs_opt_addr order: polling, lift-off, Angle Snap,
//   Motion Sync, SPDT)   62 bit n = option n known   63 crc8 of 0-62
// Bytes 64-127 (v14+; a QT Py reading only 64 bytes sees the block above unchanged):
//   64 'X'   65 status query reply count   66 its length (0 = none)   67-90 the reply (report ID first)
//   91 probe count   92 probe state (0 none, 1 waiting, 2 answered, 3 no answer)   93 reply length
//   94-117 the reply (report ID first)   118 probes refused   119-122 the probe described (cmd, addr
//   hi, addr lo, len)   123 replies rejected (echoed the command, failed the checks)   124-126 0
//   127 crc8 of 64-126
// Commands (cmd, seq, len, payload, crc8):
#define GW_FW_VERSION 14
#define LINK_SET_STAGE 0x01        // [stage 0-4]
#define LINK_SET_ANGLE 0x02        // [int8]
#define LINK_SET_POS 0x03          // [int8]
#define LINK_SET_PALETTE 0x04      // [R,G,B x5] written to the mouse and kept as the built-in palette
#define LINK_SET_DPI 0x05          // [X,Y uint16 x5]
#define LINK_SET_TUNING 0x06       // [12 bytes, GwTune order]
#define LINK_RADIO_STATE 0x07      // [0 off, 1 on] the QT Py switched its radio: LED confirmation
#define LINK_REFRESH 0x08          // [] read stage, angle, position, colours, DPI table from the mouse
#define LINK_FLASH 0x09            // [R,G,B] test flash
#define LINK_PALETTE_DEFAULT 0x0A  // [R,G,B x5] built-in palette only (QT Py restoring it after a reboot)
#define LINK_SET_OPTION 0x0B       // [option 0-4 (rs_opt_addr order), value as the mouse stores it]
#define LINK_RAW 0x0C              // [16 bytes of report 8: a read (08) or the status query (04) only]

static uint8_t link_seq = 0;
static uint8_t radio_toggles = 0;
static bool tune_set = false;
static bool palette_default_set = false;

static void put16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}
static uint16_t get16(const uint8_t* p) {
    return p[0] | p[1] << 8;
}

void gw_link_status(uint8_t* s) {
    memset(s, 0, GW_LINK_STATUS_LEN);
    s[0] = 'G';
    s[1] = 1;
    s[2] = (tune_set ? 1 : 0) | (palette_default_set ? 2 : 0) | ((rs_refresh_next >= 0) ? 4 : 0) | ((rs_noread_dev != 0) ? 8 : 0);
    const RsSetting& a = rs_settings[RS_ANGLE];
    const RsSetting& p = rs_settings[RS_POS];
    s[3] = (family_present(Family::RS) ? 1 : 0) | (family_present(Family::VUK) ? 2 : 0) | (rs_reads_worked ? 4 : 0) |
           ((rs_cur_stage >= 0) ? 8 : 0) | (a.known ? 16 : 0) | (p.known ? 32 : 0) | (led.palette_read ? 64 : 0) |
           ((rs_dpi_known == (1 << RS_DPI_STAGES) - 1) ? 128 : 0);
    s[4] = (rs_cur_stage >= 0) ? rs_cur_stage : 0xFF;
    s[5] = (uint8_t) (int8_t) a.value;
    s[6] = (uint8_t) (int8_t) p.value;
    s[7] = radio_toggles;
    for (int i = 0; i < RS_DPI_STAGES; i++) {
        uint32_t c = led.palette[i];
        s[8 + 3 * i] = c >> 16;
        s[9 + 3 * i] = c >> 8;
        s[10 + 3 * i] = c;
        put16(s + 23 + 4 * i, rs_dpi_x[i]);
        put16(s + 25 + 4 * i, rs_dpi_y[i]);
    }
    s[43] = tune.angle_step;
    s[44] = tune.pos_step;
    s[45] = (uint8_t) tune.angle_home;
    s[46] = (uint8_t) tune.pos_home;
    put16(s + 47, tune.flash_ms);
    put16(s + 49, tune.pos_hold_ms);
    put16(s + 51, tune.wheel_hold_ms);
    put16(s + 53, tune.radio_hold_ms);
    s[55] = link_seq;
    s[56] = GW_FW_VERSION;
    for (int k = 0; k < RS_OPTS; k++) {
        s[57 + k] = rs_opt_val[k];
    }
    s[62] = rs_opt_known;
    s[63] = gw_crc8(s, 63);
    s[64] = 'X';
    s[65] = rs_info_seq;
    s[66] = rs_info_len;
    memcpy(s + 67, rs_info, rs_info_len);
    s[91] = rs_raw_seq;
    s[92] = rs_raw_state;
    s[93] = rs_raw_len;
    memcpy(s + 94, rs_raw, rs_raw_len);
    s[118] = rs_raw_refused;
    s[119] = rs_raw_cmd;
    s[120] = rs_raw_addr >> 8;
    s[121] = rs_raw_addr & 0xFF;
    s[122] = rs_raw_rlen;
    s[123] = rs_cap_rejects;
    s[127] = gw_crc8(s + 64, 63);
}

static bool tuning_ok(const uint8_t* d) {
    int8_t ah = (int8_t) d[2], ph = (int8_t) d[3];
    uint16_t fl = get16(d + 4), poh = get16(d + 6), wh = get16(d + 8), rh = get16(d + 10);
    return (d[0] >= 1) && (d[0] <= 10) && (d[1] >= 1) && (d[1] <= 25) && (ah >= RS_ANGLE_MIN) && (ah <= RS_ANGLE_MAX) &&
           (ph >= RS_POS_MIN) && (ph <= RS_POS_MAX) && (fl >= 100) && (fl <= 5000) && (poh >= 150) && (poh <= 3000) &&
           (wh <= 3000) && (rh >= 200) && (rh <= 3000);
}

// one command frame from the QT Py; true if it was valid (it's then acknowledged in status byte 55)
bool gw_link_apply(const uint8_t* f, uint8_t n, uint32_t now) {
    if ((n < 4) || (f[2] + 4 != n) || (gw_crc8(f, n - 1) != f[n - 1])) {
        diag(0x751, n);
        return false;
    }
    uint8_t cmd = f[0], seq = f[1], len = f[2];
    const uint8_t* d = f + 3;
    if (seq == link_seq) {
        return true;  // a repeat of the last command (its ack was missed)
    }
    bool rs = family_present(Family::RS);
    switch (cmd) {
        case LINK_SET_STAGE:
            if ((len == 1) && (d[0] < RS_DPI_STAGES) && rs) {
                led_restore_now();
                led.requested = false;
                rs_write_value(RS_ADDR_DPI_STAGE, d[0]);
                rs_cur_stage = d[0];
                rs_dpi_last = d[0];
            }
            break;
        case LINK_SET_ANGLE:
        case LINK_SET_POS:
            if ((len == 1) && rs) {
                rs_setting_set(rs_settings[(cmd == LINK_SET_ANGLE) ? RS_ANGLE : RS_POS], (int8_t) d[0], now);
            }
            break;
        case LINK_SET_PALETTE:
        case LINK_PALETTE_DEFAULT:
            if (len == 3 * RS_DPI_STAGES) {
                if (cmd == LINK_SET_PALETTE) {
                    led_restore_now();
                }
                for (int i = 0; i < RS_DPI_STAGES; i++) {
                    uint32_t c = (uint32_t) d[3 * i] << 16 | d[3 * i + 1] << 8 | d[3 * i + 2];
                    gw_palette_default[i] = c;
                    if (cmd == LINK_SET_PALETTE) {
                        led.palette[i] = c;
                        if (rs) {
                            rs_write_colour(i, c);
                        }
                    } else if (!led.palette_read) {
                        led.palette[i] = c;  // nothing better yet
                    }
                }
                palette_default_set = true;
            }
            break;
        case LINK_SET_DPI:
            if ((len == 4 * RS_DPI_STAGES) && rs) {
                bool ok = true;
                for (int i = 0; i < 2 * RS_DPI_STAGES; i++) {
                    uint16_t v = get16(d + 2 * i);
                    ok = ok && (v >= RS_DPI_MIN) && (v <= RS_DPI_MAX);
                }
                if (ok) {
                    for (int i = 0; i < RS_DPI_STAGES; i++) {
                        rs_dpi_x[i] = get16(d + 4 * i);
                        rs_dpi_y[i] = get16(d + 4 * i + 2);
                        rs_write_dpi(i, rs_dpi_x[i], rs_dpi_y[i]);
                    }
                    rs_dpi_known = (1 << RS_DPI_STAGES) - 1;
                    if (rs_cur_stage >= 0) {
                        rs_write_value(RS_ADDR_DPI_STAGE, rs_cur_stage);  // re-select, so a changed current stage applies
                    }
                }
            }
            break;
        case LINK_SET_TUNING:
            if ((len == 12) && tuning_ok(d)) {
                tune.angle_step = d[0];
                tune.pos_step = d[1];
                tune.angle_home = (int8_t) d[2];
                tune.pos_home = (int8_t) d[3];
                tune.flash_ms = get16(d + 4);
                tune.pos_hold_ms = get16(d + 6);
                tune.wheel_hold_ms = get16(d + 8);
                tune.radio_hold_ms = get16(d + 10);
                rs_settings[RS_ANGLE].home = tune.angle_home;
                rs_settings[RS_POS].home = tune.pos_home;
                tune_set = true;
            }
            break;
        case LINK_RADIO_STATE:
            if ((len == 1) && rs) {
                rs_led_flash(d[0] ? GW_LED_RADIO_ON : GW_LED_LIMIT, now);
            }
            break;
        case LINK_REFRESH:
            if (rs) {
                rs_noread_dev = 0;  // asked for by hand: try again even if the last read timed out
                rs_refresh_asked = (rs_read != RsRead::NONE);
                rs_refresh_next = 0;
            }
            break;
        case LINK_FLASH:
            if ((len == 3) && rs) {
                rs_led_flash((uint32_t) d[0] << 16 | d[1] << 8 | d[2], now);
            }
            break;
        case LINK_RAW: {
            // reads and the status query only, with a valid check byte, and a read of at most 10 bytes
            uint8_t sum = RS_REPORT_ID;
            for (int i = 0; i < len; i++) {
                sum += d[i];
            }
            bool ok = (len == RS_LEN) && (sum == 0x55) && (d[1] == 0);
            for (int i = 5; ok && (i < RS_LEN - 1); i++) {
                ok = (d[i] == 0);  // no data bytes, as the web driver sends them
            }
            uint8_t cmd = ok ? d[0] : 0, rlen = ok ? d[4] : 0;
            uint16_t addr = ok ? (uint16_t) (d[2] << 8 | d[3]) : 0;
            // reads only inside the areas the web driver reads (0000-01ff, 1b00-1bff), at most 10 bytes
            bool read_ok = (cmd == RS_READ) && (rlen >= 1) && (rlen <= 10) &&
                           ((addr + rlen <= 0x0200) || ((addr >= 0x1b00) && (addr + rlen <= 0x1c00)));
            bool query_ok = (cmd == RS_INFO_CMD) && (addr == 0) && (rlen == 0);
            if ((read_ok || query_ok) && rs && !rs_raw_pending && (rs_read != RsRead::RAW)) {
                rs_build(rs_raw_req, cmd, addr, NULL, rlen);
                rs_raw_cmd = cmd;
                rs_raw_addr = addr;
                rs_raw_rlen = rlen;
                rs_raw_pending = true;
                rs_raw_state = 1;
                rs_raw_len = 0;
            } else {
                rs_raw_refused++;
            }
            break;
        }
        case LINK_SET_OPTION:
            if ((len == 2) && rs && (d[0] < RS_OPTS) && rs_opt_ok(d[0], d[1])) {
                rs_write_value(rs_opt_addr[d[0]], d[1]);
                rs_opt_val[d[0]] = d[1];
                rs_opt_known |= 1 << d[0];

            }
            break;
        default:
            diag(0x752, cmd);
            break;
    }
    link_seq = seq;
    return true;
}

// ---------------------------------------------------------------------------------------------

static int32_t prev_regs[8] = { 0 };
static uint32_t sensor_down_at = 0;
static bool sensor_hold_fired = false;
static uint32_t dpi_down_at = 0;
static bool dpi_hold_fired = false;
static uint32_t middle_down_at = 0;

static bool rose(const int32_t* regs, int reg) {
    return (regs[reg - 1] != 0) && (prev_regs[reg - 1] == 0);
}

static bool fell(const int32_t* regs, int reg) {
    return (regs[reg - 1] == 0) && (prev_regs[reg - 1] != 0);
}

// a setting step, with its LED flash: the setting's colour, or off when nothing could change
static void rs_step_with_flash(int setting, int dir, uint32_t colour, uint32_t now) {
    int r = rs_setting_step(rs_settings[setting], dir, now);
    rs_led_flash((r == 0) ? GW_LED_LIMIT : colour, now);
}

static void link_tick(uint32_t now) {
    static bool up = false;
    static uint32_t status_at = 0;
    if (!up) {
        gw_link_init();
        up = true;
    }
    uint8_t f[GW_LINK_FRAME_MAX];
    uint8_t n;
    if (gw_link_take_frame(f, &n)) {
        gw_link_apply(f, n, now);
        status_at = now - 20000;  // show the ack right away
    }
    if ((now - status_at) >= 20000) {
        status_at = now;
        uint8_t s[GW_LINK_STATUS_LEN];
        gw_link_status(s);
        gw_link_set_status(s);
    }
}

void gw_tick(const int32_t* regs) {
    uint32_t now = time_us_32();
    bool rs = family_present(Family::RS);
    // Mid+Left. VUK: sensor toggle on the press. Warg: a tap resets the angle (on release), a
    // hold resets the position (as soon as it has lasted the position-reset hold).
    int32_t sensor = regs[GW_SENSOR_REGISTER - 1];
    if (rose(regs, GW_SENSOR_REGISTER)) {
        trigger_count++;
        completions = 0;
        sensor_down_at = now;
        sensor_hold_fired = false;
        if (family_present(Family::VUK)) {
            front_selected = !front_selected;
            if (front_selected) {
                send_all(VUK_SELECT_FRONT, sizeof(VUK_SELECT_FRONT) / sizeof(VUK_SELECT_FRONT[0]));
            } else {
                send_all(VUK_SELECT_REAR, sizeof(VUK_SELECT_REAR) / sizeof(VUK_SELECT_REAR[0]));
            }
        }
        if (rs) {
            diag(1, trigger_count);
        }
    }
    if ((sensor != 0) && !sensor_hold_fired && ((now - sensor_down_at) >= tune.pos_hold_ms * 1000u)) {
        sensor_hold_fired = true;
        if (rs) {
            rs_setting_reset(rs_settings[RS_POS], now);
            rs_led_flash(GW_LED_RESET, now);
        }
    }
    if (fell(regs, GW_SENSOR_REGISTER) && !sensor_hold_fired && rs) {
        rs_setting_reset(rs_settings[RS_ANGLE], now);
        rs_led_flash(GW_LED_RESET, now);
    }
    // Mid+Right: a tap steps the DPI stage (on release), a hold switches the QT Py's radio (the QT
    // Py sees the count go up in the status block).
    if (rose(regs, GW_DPI_REGISTER)) {
        dpi_down_at = now;
        dpi_hold_fired = false;
    }
    if ((regs[GW_DPI_REGISTER - 1] != 0) && !dpi_hold_fired && ((now - dpi_down_at) >= tune.radio_hold_ms * 1000u)) {
        dpi_hold_fired = true;
        radio_toggles++;
        diag(0x750, radio_toggles);
    }
    if (fell(regs, GW_DPI_REGISTER) && !dpi_hold_fired) {
        dpi_trigger_count++;
        diag(3, dpi_trigger_count);
        if ((dpi_phase == DpiPhase::IDLE) && family_present(Family::VUK)) {
            dpi_start(now);
        }
        if (rs) {
            led_restore_now();  // the new stage's own colour is the feedback
            led.requested = false;
            rs_dpi_wanted = true;
        }
    }
    if (rs) {
        if (rose(regs, GW_POS_DOWN_REGISTER)) {
            rs_step_with_flash(RS_POS, -1, GW_LED_POS, now);
        }
        if (rose(regs, GW_POS_UP_REGISTER)) {
            rs_step_with_flash(RS_POS, +1, GW_LED_POS, now);
        }
        if (rose(regs, GW_ANGLE_DOWN_REGISTER)) {
            rs_step_with_flash(RS_ANGLE, -1, GW_LED_ANGLE, now);
        }
        if (rose(regs, GW_ANGLE_UP_REGISTER)) {
            rs_step_with_flash(RS_ANGLE, +1, GW_LED_ANGLE, now);
        }
    }
    if (rose(regs, GW_MIDDLE_REGISTER)) {
        middle_down_at = now;
    }
    // one step per frame with wheel movement (notches arrive in separate reports; the size isn't
    // trusted, a high-resolution wheel would report 120 per notch)
    int32_t wheel = regs[GW_WHEEL_REGISTER - 1];
    if ((wheel != 0) && (regs[GW_MIDDLE_REGISTER - 1] != 0) && ((now - middle_down_at) >= tune.wheel_hold_ms * 1000u) && rs) {
        rs_step_with_flash(RS_ANGLE, (wheel > 0) ? 1 : -1, GW_LED_ANGLE, now);
    }
    memcpy(prev_regs, regs, sizeof(prev_regs));
    dpi_tick(now);
    rs_tick(now);
    link_tick(now);
    // Also dump everything every 3 s while the Monitor tab is open, so a device that
    // never mounts (no Mid+Left possible) still shows its enumeration trace.
    static uint32_t last_dump_us = 0;
    if (monitor_enabled && (diag_count == 0) && (now - last_dump_us > 3000000)) {
        last_dump_us = now;
        diag_all();
    }
    diag_flush();
}

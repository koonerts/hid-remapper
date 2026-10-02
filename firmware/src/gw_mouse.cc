#include "gw_mouse.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include <tusb.h>

#include "globals.h"
#include "gw_payloads.h"
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
//   0xFFF40704  Warg: failures, bit 0 stage read, bit 2 angle read, bit 3 a read timed out
//               (reads skipped for that device until it's replugged), bit 4 read skipped
//   0xFFF40705  Warg: device read from (dev_addr)
//   0xFFF40706  Warg: input reports with ID 8 seen during the last read
//   0xFFF40710  Warg angle: read from the mouse (0x7FFF = not read)
//   0xFFF40711  Warg angle: written
//   0xFFF40713  Warg angle: chord count since boot (steps + resets)
//   0xFFF4072n, 0xFFF4073n  Warg: input report n with ID 8 during a read: bytes 1..4, bytes 5..7 << 8 | length
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

static void rs_forget(uint8_t dev_addr);

void gw_on_hid_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num, uint16_t desc_len) {
    if (mount_log_n < MOUNT_LOG) {
        mount_log[mount_log_n++] = (int32_t) ((uint32_t) (dev_addr & 0x7) << 28 | (uint32_t) (instance & 0xF) << 20 | (uint32_t) (itf_num & 0xF) << 16 | desc_len);
    }
    if (family_of_dev(dev_addr) == Family::RS) {
        rs_forget(dev_addr);  // the mouse may have been changed in the web app in between
    }
}

static uint16_t dpi_noread_itf = 0;  // VUK DPI: interface whose replies never finished; reads skipped

void gw_on_hid_umount(uint8_t dev_addr, uint8_t instance) {
    if (dpi_noread_itf == ((uint16_t) (dev_addr << 8 | instance))) {
        dpi_noread_itf = 0;  // the address can come back as a different device
    }
    rs_forget(dev_addr);  // VID/PID may already be gone here, so for any device
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

static int32_t prev_trigger = 0;
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
// duplicate is harmless). Reads go to one device, the cable when it's attached. The reply is
// expected as input report 8 echoing the address with the (v, 0x55 - v) pair; that is a guess
// (no reply was captured yet), so a read that doesn't come back in time just falls back to what
// the firmware last wrote: after power-up or a replug the first DPI step then assumes stage 1,
// and the angle assumes GW_ANGLE_HOME.
#define RS_REPORT_ID 0x08
#define RS_LEN 16
#define RS_WRITE 0x07
#define RS_READ 0x08
#define RS_ADDR_DPI_STAGE 0x0004
#define RS_ADDR_DPI_COUNT 0x0002  // guess; only shown in the Monitor
#define RS_ADDR_ANGLE_ON 0x00bf
#define RS_ADDR_ANGLE 0x00bd
#define RS_DPI_STAGES 5  // as configured on the Warg (1600/5000/10000/20000/40000); not read yet
// Sensor angle in degrees. Mid+Left goes back to GW_ANGLE_HOME, the angle the Warg is set to in
// the web app (2026-10-01); it's also assumed after a replug when the mouse doesn't answer a read.
#define GW_ANGLE_HOME (-13)
#define RS_ANGLE_STEP 1
#define RS_ANGLE_MIN (-30)  // the web app's range on G-Wolves' 8K mice
#define RS_ANGLE_MAX 30
#define RS_ANGLE_NONE 0x7FFF
#define RS_READ_TIMEOUT_US 150000
#define RS_ANGLE_SETTLE_US 40000  // one write for a quick run of presses
#define RS_INPUT_LOG 8

static void rs_build(uint8_t* r, uint8_t cmd, uint16_t addr, uint8_t value) {
    memset(r, 0, RS_LEN);
    r[0] = cmd;
    r[2] = addr >> 8;
    r[3] = addr & 0xFF;
    r[4] = 2;
    if (cmd == RS_WRITE) {
        r[5] = value;
        r[6] = 0x55 - value;
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

// only_dev 0 = every RS device
static int rs_send(uint8_t cmd, uint16_t addr, uint8_t value, uint8_t only_dev = 0) {
    uint8_t r[RS_LEN];
    rs_build(r, cmd, addr, value);
    int n = 0;
    for (auto const& [dev_addr, itf] : rs_targets()) {
        if ((only_dev != 0) && (dev_addr != only_dev)) {
            continue;
        }
        queue_out_report(itf, RS_REPORT_ID, r, RS_LEN);
        n++;
    }
    return n;
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

enum class RsRead : uint8_t {
    NONE,
    DPI_STAGE,
    DPI_COUNT,
    ANGLE,
};

static RsRead rs_read = RsRead::NONE;
static uint8_t rs_read_dev = 0;
static uint16_t rs_read_addr = 0;
static uint32_t rs_deadline = 0;
static uint8_t rs_noread_dev = 0;  // a read from it timed out; skipped until it's replugged
static int rs_reply_value = -1;
static bool rs_reply_ready = false;
static int32_t rs_inputs = 0;
static int32_t rs_fail = 0;

static bool rs_dpi_wanted = false;
static int rs_dpi_stage = -1;  // 0-based, as read; -1 = not read
static int rs_dpi_last = -1;   // 0-based, last written; -1 = none since the mouse was plugged in

static int rs_angle = GW_ANGLE_HOME;
static bool rs_angle_known = false;   // read from the mouse, set by a reset, or assumed after a failed read
static bool rs_angle_wanted = false;  // a read is needed before the pending steps can apply
static int rs_angle_sent = RS_ANGLE_NONE;
static int rs_angle_pending_steps = 0;
static uint32_t rs_angle_changed_at = 0;
static int32_t rs_angle_ops = 0;

static void rs_forget(uint8_t dev_addr) {
    if (rs_noread_dev == dev_addr) {
        rs_noread_dev = 0;
    }
    rs_angle_known = false;
    rs_angle_sent = RS_ANGLE_NONE;
    rs_dpi_last = -1;
}

// Input report 8 from the device being read: is it the reply (address echoed, valid value
// pair, valid checksum when the report is complete)? -1 if not.
static int rs_parse_reply(const uint8_t* r, uint16_t len) {
    if ((len < 8) || (r[0] != RS_REPORT_ID) || (r[3] != (rs_read_addr >> 8)) || (r[4] != (rs_read_addr & 0xFF)) || (r[5] != 2)) {
        return -1;
    }
    if ((uint8_t) (r[6] + r[7]) != 0x55) {
        return -1;
    }
    if (len >= 1 + RS_LEN) {
        uint8_t sum = 0;
        for (int i = 0; i < 1 + RS_LEN; i++) {
            sum += r[i];
        }
        if (sum != 0x55) {
            return -1;
        }
    }
    return r[6];
}

void gw_on_input_report(uint8_t dev_addr, uint8_t instance, const uint8_t* report, uint16_t len) {
    if ((rs_read == RsRead::NONE) || (dev_addr != rs_read_dev) || (len < 8) || (report[0] != RS_REPORT_ID)) {
        return;
    }
    if (rs_inputs < RS_INPUT_LOG) {
        diag(0x720 + rs_inputs, (int32_t) ((uint32_t) report[1] << 24 | report[2] << 16 | report[3] << 8 | report[4]));
        diag(0x730 + rs_inputs, (int32_t) ((uint32_t) report[5] << 24 | report[6] << 16 | report[7] << 8 | (len & 0xFF)));
    }
    rs_inputs++;
    int v = rs_parse_reply(report, len);
    if ((v >= 0) && !rs_reply_ready) {
        rs_reply_value = v;
        rs_reply_ready = true;
    }
}

static void rs_start_read(RsRead what, uint16_t addr, uint32_t now) {
    rs_read = what;
    rs_read_addr = addr;
    rs_reply_ready = false;
    rs_inputs = 0;
    rs_deadline = now + RS_READ_TIMEOUT_US;
    rs_send(RS_READ, addr, 0, rs_read_dev);
}

static void rs_dpi_finish() {
    int n = RS_DPI_STAGES;
    int cur = ((rs_dpi_stage >= 0) && (rs_dpi_stage < n)) ? rs_dpi_stage
              : ((rs_dpi_last >= 0) && (rs_dpi_last < n)) ? rs_dpi_last
                                                           : 0;
    int next = (cur + 1) % n;
    int sent = rs_send(RS_WRITE, RS_ADDR_DPI_STAGE, next);
    rs_dpi_last = next;
    diag(0x700, rs_dpi_stage + 1);
    diag(0x701, next + 1);
    diag(0x703, sent);
    diag(0x704, rs_fail);
    diag(0x705, rs_read_dev);
    diag(0x706, rs_inputs);
}

static int rs_angle_clamp(int a) {
    return (a < RS_ANGLE_MIN) ? RS_ANGLE_MIN : (a > RS_ANGLE_MAX) ? RS_ANGLE_MAX : a;
}

// Mid+Left: absolute, so it needs no read
static void rs_angle_reset(uint32_t now) {
    rs_angle_ops++;
    rs_angle = GW_ANGLE_HOME;
    rs_angle_known = true;
    rs_angle_pending_steps = 0;
    rs_angle_wanted = false;
    rs_angle_changed_at = now;
}

// Mid+Back / Mid+Fwd: dir -1 / +1
static void rs_angle_step(int dir, uint32_t now) {
    rs_angle_ops++;
    if (!rs_angle_known) {
        rs_angle_pending_steps += dir;
        rs_angle_wanted = true;
        return;
    }
    rs_angle = rs_angle_clamp(rs_angle + dir * RS_ANGLE_STEP);
    rs_angle_changed_at = now;
}

// value: the byte read, -1 = no reply
static void rs_angle_read_done(int value, uint32_t now) {
    if (rs_angle_known) {
        return;  // a reset came in while reading; it wins
    }
    int a = (int8_t) value;  // stored as a signed byte
    bool ok = (value >= 0) && (a >= RS_ANGLE_MIN) && (a <= RS_ANGLE_MAX);
    rs_angle = ok ? a : GW_ANGLE_HOME;
    rs_angle_sent = ok ? rs_angle : RS_ANGLE_NONE;
    if (!ok) {
        rs_fail |= 4;
    }
    rs_angle_known = true;
    diag(0x710, ok ? rs_angle : RS_ANGLE_NONE);
    rs_angle = rs_angle_clamp(rs_angle + rs_angle_pending_steps * RS_ANGLE_STEP);
    rs_angle_pending_steps = 0;
    rs_angle_changed_at = now;
}

// value: the byte read, -1 = no reply
static void rs_read_done(int value, uint32_t now) {
    RsRead what = rs_read;
    rs_read = RsRead::NONE;
    switch (what) {
        case RsRead::DPI_STAGE:
            if (value < 0) {
                rs_fail |= 1;
            }
            rs_dpi_stage = value;  // range-checked in rs_dpi_finish
            rs_dpi_finish();
            if (value >= 0) {
                // reads work: also show the byte at the guessed stage-count address
                rs_start_read(RsRead::DPI_COUNT, RS_ADDR_DPI_COUNT, now);
            }
            break;
        case RsRead::DPI_COUNT:
            diag(0x702, value);
            break;
        case RsRead::ANGLE:
            rs_angle_read_done(value, now);
            break;
        default:
            break;
    }
}

// true if a read was started; otherwise the caller goes on without one
static bool rs_try_read(RsRead what, uint16_t addr, uint32_t now) {
    rs_read_dev = rs_read_target();
    if ((rs_read_dev == 0) || (rs_read_dev == rs_noread_dev)) {
        rs_fail |= 16;
        return false;
    }
    rs_start_read(what, addr, now);
    return true;
}

static void rs_tick(uint32_t now) {
    if (rs_read != RsRead::NONE) {
        if (rs_reply_ready) {
            rs_reply_ready = false;
            rs_read_done(rs_reply_value, now);
        } else if ((int32_t) (now - rs_deadline) >= 0) {
            rs_fail |= 8;
            rs_noread_dev = rs_read_dev;
            rs_read_done(-1, now);
        }
    }
    if (rs_read == RsRead::NONE) {
        if (rs_dpi_wanted) {
            rs_dpi_wanted = false;
            rs_dpi_stage = -1;
            rs_fail = 0;
            if (!rs_try_read(RsRead::DPI_STAGE, RS_ADDR_DPI_STAGE, now)) {
                rs_dpi_finish();
            }
        } else if (rs_angle_wanted) {
            rs_angle_wanted = false;
            // steps that came in while an angle read was running were applied with it
            if (!rs_angle_known) {
                rs_fail = 0;
                if (!rs_try_read(RsRead::ANGLE, RS_ADDR_ANGLE, now)) {
                    rs_angle_read_done(-1, now);
                }
            }
        }
    }
    if (rs_angle_known && (rs_angle != rs_angle_sent) && ((now - rs_angle_changed_at) >= RS_ANGLE_SETTLE_US)) {
        // the web driver always writes 0x00bf = 1 first
        int sent = rs_send(RS_WRITE, RS_ADDR_ANGLE_ON, 0x01);
        rs_send(RS_WRITE, RS_ADDR_ANGLE, (uint8_t) (int8_t) rs_angle);
        rs_angle_sent = rs_angle;
        diag(0x711, rs_angle);
        diag(0x713, rs_angle_ops);
        diag(0x703, sent);
        diag(0x704, rs_fail);
    }
}

// ---------------------------------------------------------------------------------------------

static int32_t angle_prev_down = 0;
static int32_t angle_prev_up = 0;
static int32_t middle_prev = 0;
static uint32_t middle_down_at = 0;

void gw_tick(int32_t trigger, int32_t dpi_trigger, int32_t angle_down, int32_t angle_up, int32_t wheel, int32_t middle) {
    uint32_t now = time_us_32();
    if ((trigger != 0) && (prev_trigger == 0)) {
        trigger_count++;
        completions = 0;
        if (family_present(Family::VUK)) {
            front_selected = !front_selected;
            if (front_selected) {
                send_all(VUK_SELECT_FRONT, sizeof(VUK_SELECT_FRONT) / sizeof(VUK_SELECT_FRONT[0]));
            } else {
                send_all(VUK_SELECT_REAR, sizeof(VUK_SELECT_REAR) / sizeof(VUK_SELECT_REAR[0]));
            }
        }
        if (family_present(Family::RS)) {
            rs_angle_reset(now);
            diag(1, trigger_count);
        }
    }
    prev_trigger = trigger;
    if ((dpi_trigger != 0) && (dpi_prev_trigger == 0)) {
        dpi_trigger_count++;
        diag(3, dpi_trigger_count);
        if ((dpi_phase == DpiPhase::IDLE) && family_present(Family::VUK)) {
            dpi_start(now);
        }
        if (family_present(Family::RS)) {
            rs_dpi_wanted = true;
        }
    }
    dpi_prev_trigger = dpi_trigger;
    if ((angle_down != 0) && (angle_prev_down == 0) && family_present(Family::RS)) {
        rs_angle_step(-1, now);
    }
    if ((angle_up != 0) && (angle_prev_up == 0) && family_present(Family::RS)) {
        rs_angle_step(+1, now);
    }
    angle_prev_down = angle_down;
    angle_prev_up = angle_up;
    if ((middle != 0) && (middle_prev == 0)) {
        middle_down_at = now;
    }
    middle_prev = middle;
    // one step per frame with wheel movement (notches arrive in separate reports; the size isn't
    // trusted, a high-resolution wheel would report 120 per notch)
    if ((wheel != 0) && (middle != 0) && ((now - middle_down_at) >= GW_WHEEL_HOLD_US) && family_present(Family::RS)) {
        rs_angle_step((wheel > 0) ? 1 : -1, now);
    }
    dpi_tick(now);
    rs_tick(now);
    // Also dump everything every 3 s while the Monitor tab is open, so a device that
    // never mounts (no Mid+Left possible) still shows its enumeration trace.
    static uint32_t last_dump_us = 0;
    if (monitor_enabled && (diag_count == 0) && (now - last_dump_us > 3000000)) {
        last_dump_us = now;
        diag_all();
    }
    diag_flush();
}

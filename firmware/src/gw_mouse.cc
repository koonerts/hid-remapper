#include "gw_mouse.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include <tusb.h>

#include "globals.h"
#include "gw_chords.h"
#include "gw_dirgain.h"
#include "gw_turn.h"
#include "gw_curve.h"
#include "gw_hostcap.h"
#include "gw_link.h"
#include "gw_ota.h"
#include "gw_payloads.h"
#include "out_report.h"
#include "remapper.h"
#include "tick.h"
#include "pico/time.h"
#include "pico.h"  // v70: the board header, for the USB-A power pin
#include "hardware/gpio.h"

#if defined(FEATHER_HOST_BOARD) && !defined(PICO_DEFAULT_PIO_USB_VBUSEN_PIN)
#error "feather_host.h must define PICO_DEFAULT_PIO_USB_VBUSEN_PIN"
#endif
#if defined(__NEWLIB__)
#include <malloc.h>  // v51: mallinfo, the heap in use (profile block)
#endif

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
//   0xFFF40712  Warg angle: last write read back (v20; 0x7FFF = no answer)
//   0xFFF40713  Warg angle: chord count since boot (steps + resets)
//   0xFFF40718  Warg position: read from the mouse (0x7FFF = not read)
//   0xFFF40719  Warg position: written
//   0xFFF4071A  Warg position: last write read back (v20; 0x7FFF = no answer)
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
//   0xFFF40753  QT Py link: malformed chord table frame (length shown)
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

static uint32_t dev_mounts = 0, dev_umounts = 0;

// Every HID interface the remapper parsed (some declare only inputs, some only outputs).
// v52: kept, and built again only when what it comes from may have changed: the three maps' sizes or a device mounted or
// unmounted since (and once a second from gw_tick, known_itf_refresh). It was built (a std::set, so heap allocations) on
// every call: family_present from idle_tick each frame cost ~37 us. Callers iterate it: nothing changes those maps meanwhile
static std::set<uint16_t> known_itfs;
static size_t known_k1 = SIZE_MAX, known_k2 = SIZE_MAX, known_k3 = SIZE_MAX;
static uint32_t known_km = UINT32_MAX;

static void known_itf_build() {
    known_itfs.clear();
    for (auto const& [itf, reports] : their_usages) {
        known_itfs.insert(itf);
    }
    for (auto const& [itf, reports] : their_feature_usages) {
        known_itfs.insert(itf);
    }
    for (auto const& [key, size] : out_report_sizes) {
        known_itfs.insert(key >> 16);
    }
    known_itfs.erase(OUR_OUT_INTERFACE);
    known_k1 = their_usages.size();
    known_k2 = their_feature_usages.size();
    known_k3 = out_report_sizes.size();
    known_km = dev_mounts + dev_umounts;
}

static const std::set<uint16_t>& known_interfaces() {
    if ((their_usages.size() != known_k1) || (their_feature_usages.size() != known_k2) || (out_report_sizes.size() != known_k3) ||
        (dev_mounts + dev_umounts != known_km)) {
        known_itf_build();
    }
    return known_itfs;
}

static void known_itf_refresh(uint32_t now) {  // the once-a-second rebuild, outside any caller's loop
    static uint32_t at = 0;
    if (now - at >= 1000000) {
        at = now;
        known_itf_build();
    }
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
// dev_mounts, dev_umounts: before known_interfaces (v52)

static void rs_forget(uint8_t dev_addr, bool mounted);

// v62: the frame timing's still / moving blocks leave out frames while the Feather starts (its first 3 s) or a USB change
// settles (2 s after a device on the Feather's USB host port mounts or goes, or the host side resets: an ATEN switch, a PC
// starting): the USB start-up pauses them ~0.5 s, which is no fault. Their skipped frames are counted apart (blocks 6-7)
static uint64_t settle_until_us = 3000000ull;
static void settle_now(void) {
    settle_until_us = time_us_64() + 2000000ull;
}

static uint8_t ua_mounts = 0;  // v70: HID interfaces mounted since the start (block 8): shows that a USB-A restart really reconnected

void gw_on_hid_mount(uint8_t dev_addr, uint8_t instance, uint8_t itf_num, uint16_t desc_len) {
    ua_mounts++;
    if (mount_log_n < MOUNT_LOG) {
        mount_log[mount_log_n++] = (int32_t) ((uint32_t) (dev_addr & 0x7) << 28 | (uint32_t) (instance & 0xF) << 20 | (uint32_t) (itf_num & 0xF) << 16 | desc_len);
    }
    if (family_of_dev(dev_addr) == Family::RS) {
        rs_forget(dev_addr, true);  // the mouse may have been changed in the web app in between
    }
    settle_now();  // v62
}

static uint16_t dpi_noread_itf = 0;  // VUK DPI: interface whose replies never finished; reads skipped

void gw_on_hid_umount(uint8_t dev_addr, uint8_t instance) {
    if (dpi_noread_itf == ((uint16_t) (dev_addr << 8 | instance))) {
        dpi_noread_itf = 0;  // the address can come back as a different device
    }
    rs_forget(dev_addr, false);  // VID/PID may already be gone here, so for any device
    settle_now();  // v62
    if (umount_log_n < MOUNT_LOG) {
        umount_log[umount_log_n++] = (int32_t) dev_addr << 8 | instance;
    }
}

extern "C" void tuh_mount_cb(uint8_t dev_addr) {
    dev_mounts++;
    settle_now();  // v62
}

extern "C" void tuh_umount_cb(uint8_t dev_addr) {
    dev_umounts++;
    settle_now();  // v62
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
#define RS_ADDR_SLEEP 0x00ad  // v58: sleep time in 10 s steps, 3-60 (30-600 s; warg-dongle-sleep-timer-80-125--30-600-90.txt, 11-05)
#define RS_ADDR_COMP 0x00e1   // v58: competitive mode 0 off, 1 on (warg-competitive-mode-on-off-on.txt, 11-05)
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
#define GW_LED_ON 0x00ff00u        // green: a chord switched Motion Sync or Angle Snap on
#define GW_LED_OFF 0xff0000u       // red: ... or off
#define GW_LED_LOD 0x8000ffu       // violet: a chord stepped the lift-off distance
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
#define RS_VERIFY_DELAY_US 150000  // v20: a write is read back this long after it went out
#define RS_VERIFY_TRIES 3  // v20: writes again (read back different, or unanswered then awake) per value set
#define RS_INPUT_LOG 8

struct GwTune {
    uint8_t angle_step;
    uint8_t pos_step;
    int8_t angle_home;
    int8_t pos_home;
    uint16_t flash_ms;
    // the three holds as the old chords used them; since v15 each chord rule has its own hold time,
    // and these are only kept and shown so a QT Py that still sends them stays in step
    uint16_t pos_hold_ms;
    uint16_t wheel_hold_ms;
    uint16_t radio_hold_ms;
};
static GwTune tune = { RS_ANGLE_STEP, RS_POS_STEP, GW_ANGLE_HOME, GW_POS_HOME, GW_LED_FLASH_MS, GW_POS_RESET_HOLD_MS, 300, 600 };

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
    // v20: what the mouse has, as far as the Feather knows. A write is read back (verify_*); one the mouse
    // didn't answer is written again once it sends input (it was asleep), see rs_verify_tick()
    bool confirmed;        // the mouse's value is value: read from it, or a write read back
    bool verify_wanted;    // read back at verify_at
    bool rewrite_on_wake;  // the read-back went unanswered: write again when the mouse is awake
    uint8_t verify_tries;  // writes again after a read-back that differed
    uint32_t verify_at;
};

#define RS_ANGLE 0
#define RS_POS 1
#define RS_SETTINGS 2
static RsSetting rs_settings[RS_SETTINGS] = {
    { 0x00bf, 0x00bd, GW_ANGLE_HOME, RS_ANGLE_MIN, RS_ANGLE_MAX, angle_decode, angle_encode, angle_step, 0x710, GW_ANGLE_HOME, false, false, RS_NONE, 0, 0, 0 },
    { 0x1b48, 0x1b4a, GW_POS_HOME, RS_POS_MIN, RS_POS_MAX, pos_decode, pos_encode, pos_step, 0x718, GW_POS_HOME, false, false, RS_NONE, 0, 0, 0 },
};

// ---- recent actions (v21): what changed what, newest first in the status block's last 64 bytes
#define GW_EVENTS 8
#define GW_EVENTS_SENT 7  // v58: the status block has room for the newest 7 (bytes 236-237: options 5-6)
#define EV_VALUE_UNKNOWN 0x7F
#define GW_SRC_PAGE 6     // buttons byte = the link command, value = what it set
#define GW_SRC_ASSUMED 7  // a setting assumed after an unanswered read (buttons byte = 1 angle, 2 position)
#define GW_SRC_ADOPTED 8  // writes that didn't take: the mouse's own value is used from now on (1/2)
#define GW_SRC_WOKE 9     // written again once the mouse woke (1/2)
struct GwEvent {
    uint32_t sec;     // ev_secs when it happened
    uint8_t src;      // GW_SRC_*
    uint8_t buttons;  // a chord's modifier << 4 | trigger
    uint8_t action;   // GA_* (0 for the GW_SRC_* past 5)
    int8_t value;     // the angle/position (DPI stage for a page stage) after it, EV_VALUE_UNKNOWN if none
};
static GwEvent ev[GW_EVENTS];
static uint8_t ev_n = 0, ev_head = 0, ev_seq = 0;
static int8_t ev_wait[RS_SETTINGS] = { -1, -1 };  // the event whose value comes once that setting is read
static uint32_t ev_secs = 0, ev_secs_at = 0;     // seconds since boot (time_us_32 wraps every 71 min)

static void ev_tick(uint32_t now) {
    while ((now - ev_secs_at) >= 1000000) {
        ev_secs++;
        ev_secs_at += 1000000;
    }
}

static int8_t ev_add(uint8_t src, uint8_t buttons, uint8_t action, int value) {
    int8_t slot = (int8_t) ev_head;
    GwEvent& e = ev[slot];
    e.sec = ev_secs;
    e.src = src;
    e.buttons = buttons;
    e.action = action;
    e.value = ((value == RS_NONE) || (value < -126) || (value > 126)) ? EV_VALUE_UNKNOWN : (int8_t) value;
    ev_head = (ev_head + 1) % GW_EVENTS;
    if (ev_n < GW_EVENTS) {
        ev_n++;
    }
    for (int8_t& w : ev_wait) {
        if (w == slot) {
            w = -1;  // overwritten before its value came
        }
    }
    ev_seq++;
    return slot;
}

static void ev_fill(int setting, int value) {
    int8_t& w = ev_wait[setting];
    if (w >= 0) {
        ev[w].value = (int8_t) value;
        w = -1;
        ev_seq++;
    }
}
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

// v37: idle stage. At 40,000 DPI a resting Warg's sensor noise kept it from ever sleeping (10-31: still answering after
// 8.75 idle min with an 80 s sleep timer; on the 800 DPI stage asleep within 3 min), so after GW_IDLE_US with no real use
// the mouse is put on its lowest-DPI stage, and the first button, wheel or movement puts the stage back (then read back
// until the mouse shows it). Meanwhile everything here keeps the real stage: rs_cur_stage, the status the QT Py shows (no
// DPI pop-up), and reads that see the idle stage are taken as the real one.
#define GW_IDLE_US 120000000ull
static uint64_t idle_used_us = 0;   // the last real use: a button, the wheel, or movement at the curve's still threshold
static bool idle_low = false;       // the mouse is on idle_stage
static uint64_t idle_low_us = 0;    // when it was put there: movement in the next GW_IDLE_SETTLE_US is the old stage's noise
#define GW_IDLE_SETTLE_US 250000ull
static int idle_stage = -1, idle_back = -1;
static bool idle_checking = false;  // the stage was put back: read it back (idle_check_*)
static bool idle_check_busy = false;
static uint32_t idle_check_at = 0;
static uint8_t idle_check_tries = 0;
static int32_t idle_drops = 0, idle_check_fails = 0;
static bool idle_read_asked = false;  // the settings read once per plug-in when the stage or DPI table isn't known

// v46: while the mouse is on the idle stage, it and the stage to put back are kept in gw_keep (through the Feather's own
// restart: 11-01 an update restarted it on the idle stage, the stage to put back was lost and the mouse stayed on 800 DPI)
#define GW_IDLE_KEEP 0x49440000u  // 'ID', back << 8, idle stage
static void idle_keep() {
    gw_keep_set((idle_low || idle_checking) ? (GW_IDLE_KEEP | ((uint32_t) idle_back << 8) | (uint32_t) idle_stage) : 0);
}
static void idle_keep_load() {  // once at start: the idle stage a restart came in on is still owed
    uint32_t v = gw_keep_get();
    int back = (v >> 8) & 0xFF, low = v & 0xFF;
    if (((v & 0xFFFF0000u) == GW_IDLE_KEEP) && (back < RS_DPI_STAGES) && (low < RS_DPI_STAGES) && (back != low)) {
        idle_back = back;
        idle_stage = low;
        idle_low = true;
        idle_low_us = time_us_64();
        rs_cur_stage = back;
    } else {
        gw_keep_set(0);
    }
}

static int rs_stage_seen(int s) {  // a stage read from the mouse, as the rest of the firmware should take it
    return ((idle_low || idle_checking) && (s == idle_stage)) ? idle_back : s;
}

static void idle_cancel() {  // a stage chosen by hand (chord, page): that is the stage now
    idle_low = false;
    idle_checking = false;
    idle_keep();  // v46
    idle_used_us = time_us_64();
}

// DPI table: what the firmware last read or wrote (for the QT Py page); bit n of the mask = stage n known
static uint16_t rs_dpi_x[RS_DPI_STAGES];
static uint16_t rs_dpi_y[RS_DPI_STAGES];
static uint8_t rs_dpi_known = 0;

uint16_t gw_mouse_dpi(void) {  // v30: the X DPI of the stage in use, 0 = not known
    return ((rs_cur_stage >= 0) && (rs_cur_stage < RS_DPI_STAGES) && (rs_dpi_known & (1 << rs_cur_stage))) ? rs_dpi_x[rs_cur_stage] : 0;
}

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
    // audit 11-01: on the idle stage the mouse shows idle_stage's slot (rs_cur_stage keeps the real one), so the flash goes
    // there; led_restore_now puts back the same slot, and the DPI stays as it is
    int s = (idle_low && (rs_cur_stage == idle_back)) ? idle_stage : rs_cur_stage;
    led.stage = (s >= 0) && (s < RS_DPI_STAGES) ? s : -1;
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
    VERIFY,  // v20: an angle/position write read back (rs_read_setting)
    IDLE_CHECK,  // v37: the stage put back after the idle stage, read back
};

// The QT Py page's "refresh": stage, angle, position, the 5 colours, the 5 DPI table entries.
// Then the option bytes (rs_opt_addr order: v58 added sleep time and competitive mode), then the status query.
#define RS_OPTS 7
#define RS_REFRESH_ITEMS (3 + 2 * RS_DPI_STAGES + RS_OPTS + 1)
static const uint16_t rs_opt_addr[RS_OPTS] = { RS_ADDR_POLL, RS_ADDR_LOD, RS_ADDR_SNAP, RS_ADDR_SYNC, RS_ADDR_SPDT, RS_ADDR_SLEEP, RS_ADDR_COMP };
static uint8_t rs_opt_val[RS_OPTS];
static uint8_t rs_opt_known = 0;  // bit n = rs_opt_val[n] read from or written to the mouse
static uint8_t rs_opt_xseq = 0;   // v58: changes to options 5-6 (their bytes are in the 192-255 block: byte 117's counter)
static void rs_opt_set(uint8_t k, uint8_t v) {
    if ((k >= 5) && (!(rs_opt_known & (1 << k)) || (rs_opt_val[k] != v))) {
        rs_opt_xseq++;
    }
    rs_opt_val[k] = v;
    rs_opt_known |= 1 << k;
}


// only the values the web driver itself writes; reading, SPDT 0 (both off) is accepted too, since the
// web driver may write it although the capture never had it
static bool rs_opt_ok(uint8_t k, uint8_t v, bool write = true) {
    switch (k) {
        case 0: return (v == 0x01) || (v == 0x02) || (v == 0x04) || (v == 0x08) || (v == 0x10) || (v == 0x20) || (v == 0x40);
        case 1: return (v >= 1) && (v <= 5);
        case 2:
        case 3: return v <= 1;
        case 4: return write ? ((v >= 1) && (v <= 3)) : (v <= 3);
        case 5: return (v >= 3) && (v <= 60);  // v58: sleep time, 10 s steps
        case 6: return v <= 1;                 // v58: competitive mode
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
static int rs_dpi_dir = 1;     // the step rs_dpi_wanted asks for: +1 next stage, -1 previous

static void rs_forget(uint8_t dev_addr, bool mounted) {
    if (rs_noread_dev == dev_addr) {
        rs_noread_dev = 0;
    }
    for (RsSetting& s : rs_settings) {
        s.known = false;
        s.sent = RS_NONE;
        s.confirmed = s.verify_wanted = s.rewrite_on_wake = false;
        s.verify_tries = 0;
    }
    rs_dpi_last = -1;
    rs_cur_stage = -1;
    rs_dpi_known = 0;
    rs_opt_known = 0;
    rs_info_len = 0;
    rs_reads_worked = false;
    rs_refresh_next = -1;
    rs_refresh_item = -1;  // a reply still on its way is ignored
    idle_check_busy = false;  // v37: an idle stage stays owed: the next use puts idle_back on whatever mouse is there
    idle_read_asked = false;
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
    // audit 11-01: unsigned, so input more than 35.8 min after the timeout still clears it (the signed test went negative)
    if ((rs_noread_dev != 0) && (dev_addr == rs_noread_dev) && ((uint32_t) (time_us_32() - rs_noread_at) >= (uint32_t) RS_NOREAD_HOLD_US)) {
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
    int next = (cur + rs_dpi_dir + n) % n;
    idle_cancel();
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
// v20: an exact value from a reset or the page is always written: what the Feather last sent may never have reached
// the mouse (it was asleep), and the web driver can change it behind the Feather's back. Before v20 a value the Feather
// thought was already there wasn't sent, so the page could show 0 while the mouse stayed at 15.
static void rs_setting_force(RsSetting& s, int v) {
    s.sent = RS_NONE;
    s.verify_tries = 0;
    s.value = v;
}

static void rs_setting_reset(RsSetting& s, uint32_t now) {
    s.ops++;
    rs_setting_force(s, s.home);
    s.known = true;
    s.pending = 0;
    s.wanted = false;
    s.changed_at = now;
}

// an exact value from the QT Py page: written at once (no settle wait)
static void rs_setting_set(RsSetting& s, int v, uint32_t now) {
    s.ops++;
    rs_setting_force(s, clamp(v, s.lo, s.hi));
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
    s.verify_tries = 0;
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
    s.confirmed = ok;  // a value assumed for an unanswered read is only a guess
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
    if (!ok) {
        ev_add(GW_SRC_ASSUMED, (uint8_t) (&s - rs_settings + 1), 0, s.home);
    }
    ev_fill((int) (&s - rs_settings), s.value);
}

// v20: the read-back of a write (raw = the byte read, -1 = no reply)
static void rs_verify_done(RsSetting& s, int32_t raw) {
    int v = (raw >= 0) ? s.decode(raw) : RS_NONE;
    diag(s.diag + 2, v);
    if (s.value != s.sent) {
        return;  // changed again meanwhile: that write gets its own read-back
    }
    if (raw < 0) {
        s.rewrite_on_wake = true;  // no answer, so asleep: written again once it sends input
    } else if (v == s.value) {
        s.confirmed = true;
    } else if (s.verify_tries < RS_VERIFY_TRIES) {
        s.verify_tries++;
        s.sent = RS_NONE;  // it didn't take: write it again
    } else if (v != RS_NONE) {
        s.value = s.sent = v;  // the mouse keeps another value: show that one rather than pretend
        s.confirmed = true;
        ev_add(GW_SRC_ADOPTED, (uint8_t) (&s - rs_settings + 1), 0, v);
    }
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
        } else if (!ok && current && !rs_refresh_asked) {
            rs_refresh_next = -1;
        }
        return;
    }
    if (!ok) {
        // the mouse doesn't answer: stop here, unless a REFRESH asked for meanwhile restarted it at
        // this same item (item 0, the first read of any refresh)
        if (current && !rs_refresh_asked) {
            rs_refresh_next = -1;
        }
        return;
    }
    if (item == 0) {
        if (d[0] < RS_DPI_STAGES) {
            rs_cur_stage = rs_stage_seen(d[0]);
            rs_dpi_last = rs_cur_stage;
        }
    } else if (item < 3) {
        RsSetting& s = rs_settings[item - 1];
        int v = s.decode(d[0]);
        if ((v != RS_NONE) && !s.known) {
            // chord steps pressed while it was unknown apply on top of what the mouse has (as in
            // rs_setting_read_done); the setting's write follows once they've settled
            s.value = v;
            s.sent = v;
            s.known = true;
            s.wanted = false;
            s.confirmed = true;
            for (; s.pending > 0; s.pending--) {
                s.value = s.step(s.value, 1);
            }
            for (; s.pending < 0; s.pending++) {
                s.value = s.step(s.value, -1);
            }
            s.changed_at = time_us_32();
            ev_fill(item - 1, s.value);
        } else if ((v != RS_NONE) && (s.value == s.sent)) {
            // known, nothing waiting to be written: take the mouse's value (a change waiting wins)
            s.value = v;
            s.sent = v;
            s.pending = 0;
            s.wanted = false;
            s.confirmed = true;
            s.verify_wanted = s.rewrite_on_wake = false;
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
            rs_opt_set(k, d[0]);
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
            rs_dpi_stage = ok ? rs_stage_seen(byte) : byte;  // range-checked in rs_dpi_finish
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
        case RsRead::VERIFY:
            rs_verify_done(rs_settings[rs_read_setting], byte);
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
                rs_cur_stage = rs_stage_seen(byte);
            }
            break;
        case RsRead::IDLE_CHECK:
            idle_check_busy = false;
            if (!idle_checking) {
                break;  // a stage chosen by hand meanwhile
            }
            if (ok && (byte == idle_back)) {
                idle_checking = false;
                idle_keep();  // v46: back
            } else if (++idle_check_tries >= 6) {
                idle_checking = false;
                idle_check_fails++;
                if (ok && (byte < RS_DPI_STAGES)) {
                    rs_cur_stage = byte;  // v42: given up: the stage the mouse says it's on (its DPI is what the speed curve needs)
                    idle_low = false;
                }
                idle_keep();  // v46
                diag(0x708, idle_check_fails);
            } else {
                rs_write_value(RS_ADDR_DPI_STAGE, idle_back);
                idle_check_at = now + 400000;
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
            for (uint8_t i = 0; (i < RS_SETTINGS) && !started; i++) {
                RsSetting& s = rs_settings[i];
                if (!s.verify_wanted || ((int32_t) (now - s.verify_at) < 0)) {
                    continue;
                }
                s.verify_wanted = false;
                rs_read_setting = i;
                if (rs_try_read(RsRead::VERIFY, s.addr, 2, now)) {
                    started = true;
                } else {
                    s.rewrite_on_wake = true;  // the mouse isn't answering reads: asleep, most likely
                }
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
            // a refresh waits while a light flash is showing: a colour read now would be the flash's,
            // and it would be put back as the stage's colour when the flash ends
            if (!started && (rs_refresh_next >= 0) && !led.active && !led.restore_pending) {
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
    // v20: a write the mouse didn't answer the read-back of goes out again once it's awake (sending input)
    for (RsSetting& s : rs_settings) {
        if (s.rewrite_on_wake && s.known && rs_reads_work()) {
            s.rewrite_on_wake = false;
            if (s.verify_tries < RS_VERIFY_TRIES) {
                s.verify_tries++;
                s.sent = RS_NONE;
                ev_add(GW_SRC_WOKE, (uint8_t) (&s - rs_settings + 1), 0, s.value);
            }
        }
    }
    // one setting per tick
    for (RsSetting& s : rs_settings) {
        if (s.known && (s.value != s.sent) && ((now - s.changed_at) >= RS_SETTLE_US)) {
            rs_write_value(s.on_addr, 0x01);
            rs_write_value(s.addr, s.encode(s.value));
            s.sent = s.value;
            s.confirmed = false;
            s.verify_wanted = true;
            s.rewrite_on_wake = false;
            s.verify_at = now + RS_VERIFY_DELAY_US;
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
//   since boot, bit 2 refresh running, bit 3 reads paused (last one timed out: mouse asleep?), bit 4 (v48) the last
//   restart was a freeze (block 4, gw_ota.h)
//   3 mouse flags: bit 0 Warg, 1 VUK, 2 reads answered,
//   3 stage known, 4 angle known, 5 position known, 6 colours read from the mouse, 7 DPI table known
//   4 stage (0-based, 0xFF unknown)   5 angle (int8)   6 position (int8)   7 Mid+Right hold count
//   8-22 stage colours (R,G,B x5)   23-42 DPI table (X,Y uint16 x5)   43-54 tuning (GwTune order:
//   angle step, position step, angle home, position home, flash ms, position-reset hold ms, wheel
//   hold ms, radio hold ms)   55 last command seq applied   56 firmware version
//   57-61 option bytes as the mouse stores them (rs_opt_addr order: polling, lift-off, Angle Snap,
//   Motion Sync, SPDT)   62 bit n = option n known (v58: bits 5-6 = sleep time, competitive mode at 236-237)   63 crc8 of 0-62
// Bytes 64-127 (v14+; a QT Py reading only 64 bytes sees the block above unchanged):
//   64 'X'   65 status query reply count   66 its length (0 = none)   67-83 the reply (report ID first,
//   17 bytes at most)   84-85 average processing time per frame over the last second (0.1 us)
//   86-87 the longest frame in it (us)   88 the chord filter's average (0.1 us, at most 255)   89 its longest
//   (us, at most 255)   90 ticks merged in the last second (v19+: frames the main loop never processed, at most 255)   91 probe count   92 probe state (0 none, 1 waiting, 2 answered, 3 no answer)
//   93 reply length   94-110 the reply (report ID first, 17 bytes at most)   111-117 (v19+) frame timing, all in 4 us steps
//   at most 255 except 111-112: 111-112 ticks merged since boot (at most 65535)   113 how long after its tick each frame's
//   processing was done, average over the last second   114 the longest   115 the 1 ms SOF interrupt (USB host frame:
//   SOF + the transfers to the mouse), average   116 its longest   117 angle/position confirmed (v20, see below)   118 probes refused   119-122 the probe described (cmd, addr
//   hi, addr lo, len)   123 replies rejected (echoed the command, failed the checks)   124 chord table
//   number (changes with every new table; 1 at power-up)   125 chord flags (GW_CH_* in gw_chords.h)
//   126 chord count   127 crc8 of 64-126
//   (byte 117, v20+) bit 0 angle confirmed by the mouse (read, or a write read back), bit 1 position confirmed, bit 2 angle
//   write waiting for the mouse to wake, bit 3 position write waiting
// Bytes 128-191 (v15+; read only when byte 124 changes): 128 'C'   129 chord table number   130 count
//   131-190 the rules, 5 bytes each (gw_rule_t), unused ones 0   191 crc8 of 128-190
// Bytes 192-255 (v21+; read when byte 117's high nibble, a counter of changes, moves): 192 'E'   193 count (at most 8)
//   194-241 the recent actions, newest first, 6 bytes each (v58: at most 7, 194-235; 236 sleep time in 10 s steps, 237
//   competitive mode, both as the mouse stores them and valid with byte 62 bits 5-6; 238-241 0): age in s (uint16, at most 65535), source (GW_SRC_*: 1 chord tap,
//   2 hold, 3 double tap, 4 wheel up, 5 wheel down, 6 page, 7 assumed after an unanswered read, 8 the mouse's value taken
//   after writes didn't take, 9 written again once the mouse woke), buttons (chord: modifier << 4 | trigger; page: the
//   command; 7-9: 1 angle, 2 position), action (GA_*), value after it (int8: angle/position, the stage for a page stage;
//   0x7F none)   242 (v24) direction gains: bit 0 applying, bit 1 got a table   243 crc8 of the gains as received   244-254 0
//   255 crc8 of 192-254
// Commands (cmd, seq, len, payload, crc8):
#define GW_FW_VERSION 72  // v72: review fixes for v70: the quiet-time check goes to every G-Wolves device on the port (with the cable also attached it only reached the cable) and is the status query (a late reply to a stage read could pass for a real one); no hang declared while the USB side settles; block 8's restarting flag held until published; v71: report pauses counted at every polling rate: a report at least 3 ms later than it was due (at 1000 Hz and up the same as v63-v70), the 15 cm/s line scaled to the counts a report holds; v70: the USB-A port restarted by its power switch (GPIO 18) when sends to the receiver hang for 3 s (ua_tick; the 11-10 hang: the receiver stayed mounted, no input, every read queued forever, only a replug helped), at most 3 in a row; LINK_USB_RESET from the page; while the mouse is quiet a harmless read every 10 s so a hang shows up with nothing else being sent; block 8 layout 3 (48 bytes: 42-46 the USB-A port); v69: review fix for v68: the slow-down's per-stroke "computed" bits in a uint16_t (a uint8_t lost segments 8-10: recomputed every frame); v68: slow-down gains up to 10 points (GW_DECEL_N; 6 before); v67: the 1 ms SOF interrupt's interval jitter per second in block 8 (30-31 worst us, 40-41 count of 50 us or more), to see whether the host side delays the mouse's USB frames; v66: review fixes for the pause count: the mouse's own DPI only, 1000 Hz polling or faster, empty frames as processed and none while the Feather itself lost frames; block 8 layout 2 (44 bytes: frames checked since the start); v65: a mouse plugged in (an ATEN switch, wired/wireless) has its settings read once it settles, not after 2 min idle (the speed curve used its saved DPI meanwhile: 40000 on a 7500 DPI stage); the pause count falls back to the curve's DPI too; v64: frames lost while the USB host task blocks 20 ms or more (a device plugged in: TinyUSB's 50 + 450 ms enumeration waits, before tuh_mount_cb) count as settling, not skipped (v62 counted 450 per plug-in); v63: pauses in the mouse's reports mid-movement (3+ frames with no report between two at 15 cm/s or faster: the mouse or its receiver held them back) counted by length, link block 8; v62: the still / moving frame timing leaves out settling (the first 3 s, 2 s after a USB device change or a host reset), its skipped frames counted apart (blocks 6-7 124-126); v61: the speed histogram's bin edges written out (computed on the first moving frame after each start: 25 powf calls, ~490 us in one frame); v60: review fixes for v59: the still / moving SOF numbers count every interrupt (tick.cc, by the kind of the frame before it), and with no DPI known (no speed) frames are told apart by raw motion, counted in blocks 6-7 bytes 122-123; v59: where each frame's time goes, kept apart for still and moving frames (link blocks 6 and 7, layout 4: also the remapping, chord check, frame finished and SOF interrupt per kind); block 5 is the two together; v58: the mouse's sleep time (0x00ad, 10 s steps) and competitive mode (0x00e1) as options 5-6: refresh reads them, LINK_SET_OPTION writes them, status 236-237 (recent actions sent: 7, not 8); v57: the speed curve's carried fraction in Q32, exact (v54-v56's Q16 rounded ties up: +3 counts per million frames toward +x/+y at 50 counts a frame; gwtest curve_bias); a changed curve or slow-down table keeps the speed, the carry and the stroke (only a new tau or turning on/off starts over); v56: the mouse's reports parse faster (get_bits whole bytes at once, do_handle_received_report from RAM; the same values); v55: the profile also times the main loop pass that takes the frame's tick (USB host task, report parsing, the G-Wolves hook, the wait before it); v54: the speed curve with fewer soft-float calls: cached 1000/dt and 2.54/dpi, the segments' slopes, slow-down thresholds per stroke, the carried fraction in Q16 (output within 1 count of v53 a frame: gwtest curve_round; its rounded-up ties drift, fixed in v57); v53: the mappings loop walks a flat list of the plain mappings' inputs; the speed curve runs from RAM and caches its expf, motion length and table steps (output bit for bit the same: gwtest maptest53, curve_diff, curve_probe); v52: process_mapping skips plain mappings whose inputs are all 0 (with unmapped passthrough, every key of our descriptor: ~160 us a frame); the HID interface list is kept, not built on each call (~37 us a frame); v51: process_mapping runs from RAM (it ran from flash, as all the fork's own code does); the profile block also times gw_tick's parts and shows the heap; v50: where each frame's time goes (link block 5: process_mapping's sections, the link interrupt's time); v49: flash writes run boot2 with interrupts off (an interrupt mid-command left XIP broken: the update freezes); v48: what a freeze left (where the main loop was, the PC a top-priority timer or the hard-fault handler caught) kept through the watchdog's restart, for the QT Py (block 4); v47: slow-down gain shares in 0.1 % steps (LINK_SET_DECEL byte 0 bit 1 = 4-byte points; the 3-byte whole-% points still taken), a 4 s watchdog (main.cc); v46: the idle stage kept through the Feather's own restart (an update on the idle stage left the mouse on 800 DPI); v45: audit 11-01 fixes (link writes cut by a late IRQ, idle-stage flash slot, reads-paused hold past 35.8 min, no flash erase after APPLY); v44: no change from v43 but the number (the first update with 240-byte frames); v43: faster updates (240-byte frames, the staging slot erased ahead in 64 KB blocks)
#define GW_STR2(x) #x
#define GW_STR(x) GW_STR2(x)
// v40: the version as text in the image: the QT Py reads it from an uploaded update (gw_ota.cc reports it, which keeps
// it in the image)
extern const char gw_fw_tag[] = "GWFW:v" GW_STR(GW_FW_VERSION) ";";
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
#define LINK_SET_OPTION 0x0B       // [option 0-6 (rs_opt_addr order; v58: 5 sleep time 3-60, 6 competitive 0/1), value as the mouse stores it]
#define LINK_RAW 0x0C              // [16 bytes of report 8: a read (08) or the status query (04) only]
#define LINK_SET_CHORDS 0x0D       // [part, total, rules] (gw_chords_link): the chord table, kept in flash
#define LINK_SET_DIRGAIN 0x0E      // v24: [on, 8 gains uint16 per mille] sensitivity per direction (gw_dirgain.cc), RAM only
#define LINK_SET_CURVE 0x10        // v30: [on, tau ms, dpi fallback uint16, n x (speed 0.1 cm/s uint16, gain per mille uint16)] the speed curve (gw_curve.cc), RAM only
#define LINK_CURVE_HIST_RESET 0x11  // v32: [] the speed histogram starts over (gw_curve.cc)
#define LINK_SET_DECEL 0x12        // v39: [on, n x (share of the peak speed %, gain per mille uint16)] the slow-down gain (gw_curve.cc), RAM only; v47: byte 0 bit 0 = on, bit 1 = points [share 0.1 % uint16, gain uint16] (len 1 + 4n), else the 3-byte whole-% points
#define LINK_USB_RESET 0x18        // v70: [] the USB-A port off and on (ua_tick), as replugging what's in it
#define LINK_TURN 0x0F            // v27: [total int32, counts a second uint32, hand counts uint32] a measured turn (gw_turn.cc); [] stops it; v28: [1] counts the real X; v34: [2] counts it after the speed curve

// per-frame processing time (gw_frame_time), over a 1 s window; since v19 also how long after its tick each frame was
// done (td_*), the SOF interrupt's time and the ticks merged (frames never processed), from tick.cc
static uint32_t ft_sum = 0, ft_n = 0, ft_max = 0, fc_sum = 0, fc_max = 0, ft_at = 0, td_sum = 0, td_max = 0, merged_at = 0;
static uint16_t ft_avg10 = 0, ft_peak = 0, merged_total = 0;
static uint8_t fc_avg10 = 0, fc_peak = 0, td_avg4 = 0, td_peak4 = 0, sof_avg4 = 0, sof_peak4 = 0, merged_1s = 0;

static uint8_t in_4us(uint32_t us) {
    uint32_t q = (us + 2) / 4;
    return (q > 255) ? 255 : q;
}

// v50: gw_prof_* (gw_mouse.h)
// v59: kept apart by whether the mouse was moving in that frame ([0] still, [1] moving: gw_curve_moving, the smoothed
// speed at the curve's still threshold or over within 50 ms of motion; tracked with the curve off too), published as link
// blocks 6 and 7 (layout 4, prof_publish); block 5 (all frames, layout 3) is the two together
static uint32_t pf_last = 0, pf_sub_last = 0, pf_cur[GW_PROF_N];
static uint32_t pf_n[2], pf_sum[2][GW_PROF_N], pf_max[2][GW_PROF_N], pf_worst[2][GW_PROF_N], pf_worst_t[2];
static uint32_t pc_ft_sum[2], pc_ft_max[2], pc_fc_sum[2], pc_fc_max[2], pc_td_sum[2], pc_td_max[2];
static uint32_t pc_nodpi[2];         // frames told apart by raw motion (no DPI known: no speed)
static uint32_t pf_mlast = 0, merged_run = 0, merged_settle = 0;  // v62: frames skipped, apart for settling (settle_now)
// v70: the USB-A port's power switch (GPIO 18, the 5 V boost's enable), for a hang that only a replug cured: the receiver stays
// mounted, no input comes and whatever is sent to it never finishes. Not the same as a sleeping mouse: its receiver takes the
// request and only the reply is missing (a read then times out in 150 ms), so the queue keeps moving. A send stuck for
// UA_STALL_MS turns the port off for UA_OFF_US; the next try waits UA_WAIT_US; UA_MAX_TRIES in a row, then only the page's
// button until the queue has been clean for UA_CLEAN_US (a replug, for one). While the mouse is quiet (and nothing else is
// being sent) a status query goes to every G-Wolves device every UA_BEAT_US, its reply ignored, or a hang would go unseen
#define UA_STALL_MS 3000
#define UA_OFF_US 600000ull
#define UA_WAIT_US 8000000ull
#define UA_MAX_TRIES 3
#define UA_CLEAN_US 60000000ull
#define UA_QUIET_US 2000000ull
#define UA_BEAT_US 10000000ull
static uint8_t ua_resets = 0, ua_tries = 0;
static uint16_t ua_worst_ms = 0;  // the longest stall seen since the start
static uint64_t ua_on_at = 0, ua_wait_until = 0, ua_beat_at = 0, ua_clean_from = 0;
static bool ua_manual = false, ua_gave_up = false, ua_off_pub = false;  // ua_off_pub: went off since block 8 was last published
bool gw_ua_beats = true;
static uint64_t raw_move_us = 0;     // the last frame with raw motion (gw_idle_frame)
static uint8_t pf_blk[GW_LINK_PROF_LEN];
static uint8_t pc_blk[2][GW_LINK_PROFC_LEN];
static bool pf_dirty = false, pc_dirty[2] = { false, false };
// v63: the report pauses (gap_frame): counts by length since the Feather started, paused ms (3-49 ms ones) since then,
// the longest 3-49 ms one and the frames the check covered this second; published as block 8
#define GW_GAP_BUCKETS 5  // 3-4, 5-9, 10-19, 20-49, 50 ms and longer (that long may be a lift-off)
static uint32_t gp_cnt[GW_GAP_BUCKETS], gp_ms = 0, gp_long = 0, gp_frames = 0;
static uint8_t gp_blk[GW_LINK_GAP_LEN];
static bool gp_dirty = false;
static void put16(uint8_t* p, uint16_t v);
static void put32(uint8_t* p, uint32_t v) {
    for (uint8_t i = 0; i < 4; i++) {
        p[i] = (v >> (8 * i)) & 0xFF;
    }
}
static uint16_t pf_seq = 0;  // v59: seconds published (blocks 6 and 7 carry it, so a reader can tell they're the same second)

void gw_prof_start(void) {
    uint32_t now = time_us_32();
    pf_cur[0] = now - tick_taken_time();
    pf_last = now;
}

void gw_prof_mark(uint8_t k) {
    uint32_t now = time_us_32();
    if (k < GW_PROF_N) {
        pf_cur[k] = now - pf_last;
    }
    pf_last = now;
}

void gw_prof_set(uint8_t k, uint32_t us) {  // v55
    if (k < GW_PROF_N) {
        pf_cur[k] = us;
    }
}

void gw_prof_add(uint8_t k, uint32_t us) {  // v55
    if (k < GW_PROF_N) {
        pf_cur[k] += us;
    }
}

static void prof_sub(uint8_t k) {  // v51: a part of gw_tick, from the last prof_sub (or gw_tick's start)
    uint32_t now = time_us_32();
    if (k < GW_PROF_N) {
        pf_cur[k] = now - pf_sub_last;
    }
    pf_sub_last = now;
}

static void prof_frame(uint8_t c) {  // the frame's sections into the second's sums (v59: c 0 still, 1 moving)
    uint32_t t = 0;
    uint32_t* sum = pf_sum[c];
    uint32_t* mx = pf_max[c];
    for (uint8_t k = 0; k < GW_PROF_N; k++) {
        sum[k] += pf_cur[k];
        if (pf_cur[k] > mx[k]) {
            mx[k] = pf_cur[k];
        }
        if (k < GW_PROF_MAIN) {
            t += pf_cur[k];  // v51: gw_tick's parts are inside section 7 already
        }
    }
    if (t > pf_worst_t[c]) {
        pf_worst_t[c] = t;
        memcpy(pf_worst[c], pf_cur, sizeof(pf_worst[c]));
    }
    memset(pf_cur, 0, sizeof(pf_cur));
    pf_n[c]++;
}

static void put_sections(uint8_t* p, const uint32_t* sum, uint32_t n, const uint32_t* mx, const uint32_t* worst) {
    for (uint8_t k = 0; k < GW_PROF_N; k++) {
        uint32_t a = n ? sum[k] * 10 / n : 0;
        uint16_t a16 = (a > 65535) ? 65535 : a;
        p[4 * k] = a16 & 0xFF;
        p[4 * k + 1] = a16 >> 8;
        p[4 * k + 2] = in_4us(mx[k]);
        p[4 * k + 3] = in_4us(worst[k]);
    }
}

static void prof_publish(void) {  // once a second: the block, then the sums start again
    uint8_t* b = pf_blk;
    memset(b, 0, GW_LINK_PROF_LEN);
    uint32_t ius, in;
    gw_link_irq_take(&ius, &in);
    b[0] = 'P';
    b[1] = 3;  // v55: 24 sections (2: 20, v51-v54)
    b[2] = GW_PROF_N;
    for (uint8_t i = 0; i < 4; i++) {
        b[5 + i] = (ius >> (8 * i)) & 0xFF;
    }
    uint16_t in16 = (in > 65535) ? 65535 : in;
    b[9] = in16 & 0xFF;
    b[10] = in16 >> 8;
    uint32_t used = 0, arena = 0;
#if defined(__NEWLIB__)
    struct mallinfo mi = mallinfo();
    used = mi.uordblks / 16;
    arena = mi.arena / 16;
#endif
    used = (used > 65535) ? 65535 : used;
    arena = (arena > 65535) ? 65535 : arena;
    b[11] = used & 0xFF;
    b[12] = used >> 8;
    b[13] = arena & 0xFF;
    b[14] = arena >> 8;
    // block 5: still and moving frames together (the slowest frame of either)
    uint32_t n = pf_n[0] + pf_n[1], sum[GW_PROF_N], mx[GW_PROF_N];
    for (uint8_t k = 0; k < GW_PROF_N; k++) {
        sum[k] = pf_sum[0][k] + pf_sum[1][k];
        mx[k] = (pf_max[0][k] > pf_max[1][k]) ? pf_max[0][k] : pf_max[1][k];
    }
    b[3] = (n > 65535) ? 255 : n & 0xFF;
    b[4] = (n > 65535) ? 255 : n >> 8;
    put_sections(b + 15, sum, n, mx, pf_worst[(pf_worst_t[1] > pf_worst_t[0]) ? 1 : 0]);
    b[GW_LINK_PROF_LEN - 1] = gw_crc8(b, GW_LINK_PROF_LEN - 1);
    pf_dirty = true;
    // v59: blocks 6 and 7, layout 4: 'P', 4, GW_PROF_N, frames uint16, the second's number uint16 (the same in both), 0 still /
    // 1 moving, remapping average (0.1 us, uint16) and worst (us, uint16), the chord check's average (0.1 us) and worst (us),
    // frame finished after its tick average and worst (4 us), the 1 ms SOF interrupt average and worst (4 us), then the
    // sections as layout 3 (18-113), 114-117 frames skipped since the Feather started (uint32, not capped: the status's
    // 111-112 stop at 65535; v62: outside settling, those while settling at 124-126, uint24), 118-121 seconds since it started (uint32: smaller than before = it restarted), 122-123 of the
    // frames, those told apart by raw motion (no DPI known, so no speed), 124-126 (v62) frames skipped while settling, crc8 last; the SOF numbers count every
    // interrupt, by the kind of the frame that ended before it
    pf_seq++;
    for (uint8_t c = 0; c < 2; c++) {
        uint8_t* q = pc_blk[c];
        uint32_t fn = pf_n[c];
        memset(q, 0, GW_LINK_PROFC_LEN);
        q[0] = 'P';
        q[1] = 4;
        q[2] = GW_PROF_N;
        put16(q + 3, (fn > 65535) ? 65535 : fn);
        put16(q + 5, pf_seq);
        q[7] = c;
        uint32_t fa = fn ? pc_ft_sum[c] * 10 / fn : 0, ca = fn ? pc_fc_sum[c] * 10 / fn : 0;
        put16(q + 8, (fa > 65535) ? 65535 : fa);
        put16(q + 10, (pc_ft_max[c] > 65535) ? 65535 : pc_ft_max[c]);
        q[12] = (ca > 255) ? 255 : ca;
        q[13] = (pc_fc_max[c] > 255) ? 255 : pc_fc_max[c];
        q[14] = fn ? in_4us(pc_td_sum[c] / fn) : 0;
        q[15] = in_4us(pc_td_max[c]);
        uint32_t ss, sn, sm;
        tick_sof_take_class(c, &ss, &sn, &sm);  // every SOF interrupt, by the kind of the frame before it
        q[16] = sn ? in_4us(ss / sn) : 0;
        q[17] = in_4us(sm);
        put_sections(q + 18, pf_sum[c], fn, pf_max[c], pf_worst[c]);
        put32(q + 114, merged_run);  // v62: outside settling (before: all)
        put32(q + 118, ev_secs);
        put16(q + 122, (pc_nodpi[c] > 65535) ? 65535 : pc_nodpi[c]);
        uint32_t ms = (merged_settle > 0xFFFFFF) ? 0xFFFFFF : merged_settle;  // v62: 124-126 skipped while settling (uint24)
        q[124] = ms & 0xFF;
        q[125] = (ms >> 8) & 0xFF;
        q[126] = (ms >> 16) & 0xFF;
        q[GW_LINK_PROFC_LEN - 1] = gw_crc8(q, GW_LINK_PROFC_LEN - 1);
        pc_dirty[c] = true;
    }
    // v63: block 8, layout 1 (v66: layout 2, 44 bytes: 30-31 unused, 36-39 frames checked since the start, crc at 43): 'R', 1, the second's number (uint16, as blocks 6-7), 4-23 report pauses since the Feather
    // started by length (uint32 x GW_GAP_BUCKETS: 3-4, 5-9, 10-19, 20-49, 50+ ms), 24-27 paused ms of the 3-49 ms ones since
    // then (uint32), 28-29 the longest 3-49 ms one this second (ms), 30-31 frames the check covered this second (moving,
    // DPI known, not settling), 32-35 seconds since the Feather started (as blocks 6-7), crc8 last
    {
        uint8_t* g = gp_blk;
        memset(g, 0, GW_LINK_GAP_LEN);
        g[0] = 'R';  // not 'G', 1: that is the status block's start (the QT Py's read_sel takes it as one)
        g[1] = 3;  // v70: layout 3 (48 bytes, 42-46 the USB-A port); v66: layout 2 (frames checked since the start at 36-39)
        put16(g + 2, pf_seq);
        for (uint8_t k = 0; k < GW_GAP_BUCKETS; k++) {
            put32(g + 4 + 4 * k, gp_cnt[k]);
        }
        put32(g + 24, gp_ms);
        put16(g + 28, (gp_long > 65535) ? 65535 : gp_long);
        put32(g + 32, ev_secs);
        put32(g + 36, gp_frames);  // v66: since the start, as the counts (layout 1: this second's, uint16 at 30-31)
        {
            uint32_t jm, jn;  // v67: the 1 ms SOF interrupt's worst distance from 1000 us this second (us), and how many 50 us or more
            tick_sof_jit_take(&jm, &jn);
            put16(g + 30, (jm > 65535) ? 65535 : jm);
            put16(g + 40, (jn > 65535) ? 65535 : jn);
        }
        g[42] = ua_resets;  // v70: times the USB-A port was restarted since the Feather started
        g[43] = ((ua_on_at || ua_off_pub) ? 1 : 0) | (ua_gave_up ? 2 : 0) | ((out_report_stall_ms() >= UA_STALL_MS) ? 4 : 0);
        ua_off_pub = false;
        put16(g + 44, ua_worst_ms);
        g[46] = ua_mounts;
        g[GW_LINK_GAP_LEN - 1] = gw_crc8(g, GW_LINK_GAP_LEN - 1);
        gp_long = 0;
        gp_dirty = true;
    }
    memset(pf_n, 0, sizeof(pf_n));
    memset(pf_worst_t, 0, sizeof(pf_worst_t));
    memset(pf_sum, 0, sizeof(pf_sum));
    memset(pf_max, 0, sizeof(pf_max));
    memset(pf_worst, 0, sizeof(pf_worst));
    memset(pc_ft_sum, 0, sizeof(pc_ft_sum));
    memset(pc_ft_max, 0, sizeof(pc_ft_max));
    memset(pc_fc_sum, 0, sizeof(pc_fc_sum));
    memset(pc_fc_max, 0, sizeof(pc_fc_max));
    memset(pc_td_sum, 0, sizeof(pc_td_sum));
    memset(pc_td_max, 0, sizeof(pc_td_max));
    memset(pc_nodpi, 0, sizeof(pc_nodpi));
}

// v64: main.cc, after a main loop pass whose USB host task blocked 20 ms or more (a device plugged in: TinyUSB's
// enumeration waits 50 + 450 ms before tuh_mount_cb, so settle_now came after the frames were lost and v62 counted them
// as faults). Those frames go to settling, and settling starts from here
void gw_usb_stall(uint32_t merged) {
    merged_settle += merged;
    pf_mlast += merged;  // the next frame's delta leaves them out
    settle_now();
}

void gw_frame_time(uint32_t total_us, uint32_t chords_us) {
    static bool started = false;
    // v59: moving by the curve's speed; with no DPI known (no speed) by raw motion in the last 50 ms instead, counted
    bool sk = gw_curve_speed_known();
    uint8_t c = sk ? (gw_curve_moving() ? 1 : 0) : ((time_us_64() - raw_move_us < 50000) ? 1 : 0);
    // v62: settling (the start, a USB change: settle_now; a new host capture is the host side resetting) is left out
    {
        static uint8_t cap_seen = 0;
        static bool cap_known = false;
        uint8_t cid = gw_hostcap_id();
        if (cap_known && (cid != cap_seen)) {
            settle_now();
        }
        cap_seen = cid;
        cap_known = true;
    }
    bool settling = time_us_64() < settle_until_us;
    {
        uint32_t m = ticks_merged();
        if (settling) {
            merged_settle += m - pf_mlast;
        } else {
            merged_run += m - pf_mlast;
        }
        pf_mlast = m;
    }
    if (!sk && !settling) {
        pc_nodpi[c]++;
    }
    tick_sof_class(settling ? 2 : c);
    if (settling) {
        memset(pf_cur, 0, sizeof(pf_cur));  // its parts aren't kept
    } else {
        prof_frame(c);  // v50
    }
    if (pf_dirty) {
        pf_dirty = !gw_link_set_block(5, pf_blk);  // refused while that buffer was being read: again next frame
    }
    for (uint8_t k = 0; k < 2; k++) {  // v59
        if (pc_dirty[k]) {
            pc_dirty[k] = !gw_link_set_block(6 + k, pc_blk[k]);
        }
    }
    if (gp_dirty) {  // v63
        gp_dirty = !gw_link_set_block(8, gp_blk);
    }
    if (!started) {  // v25: the first window starts here, not at boot (it counted the ~0.5 s USB enumeration as one second: 255 skipped)
        started = true;
        ft_at = time_us_32();
        merged_at = ticks_merged();
    }
    ft_sum += total_us;
    fc_sum += chords_us;
    ft_n++;
    if (total_us > ft_max) {
        ft_max = total_us;
    }
    if (chords_us > fc_max) {
        fc_max = chords_us;
    }
    uint32_t now = time_us_32();
    uint32_t td = now - tick_taken_time();
    td_sum += td;
    if (td > td_max) {
        td_max = td;
    }
    if (!settling) {  // v59: the same per still / moving frame (the SOF interrupts: tick.cc); v62: not while settling
        pc_ft_sum[c] += total_us;
        pc_fc_sum[c] += chords_us;
        pc_td_sum[c] += td;
        pc_ft_max[c] = (total_us > pc_ft_max[c]) ? total_us : pc_ft_max[c];
        pc_fc_max[c] = (chords_us > pc_fc_max[c]) ? chords_us : pc_fc_max[c];
        pc_td_max[c] = (td > pc_td_max[c]) ? td : pc_td_max[c];
    }
    if ((now - ft_at) >= 1000000) {
        uint32_t a = ft_sum * 10 / ft_n, c = fc_sum * 10 / ft_n;
        ft_avg10 = (a > 65535) ? 65535 : a;
        ft_peak = (ft_max > 65535) ? 65535 : ft_max;
        fc_avg10 = (c > 255) ? 255 : c;
        fc_peak = (fc_max > 255) ? 255 : fc_max;
        td_avg4 = in_4us(td_sum / ft_n);
        td_peak4 = in_4us(td_max);
        uint32_t s_sum, s_n, s_max;
        tick_sof_take(&s_sum, &s_n, &s_max);
        sof_avg4 = s_n ? in_4us(s_sum / s_n) : 0;
        sof_peak4 = in_4us(s_max);
        uint32_t m = ticks_merged();
        merged_1s = ((m - merged_at) > 255) ? 255 : (m - merged_at);
        merged_total = (m > 65535) ? 65535 : m;
        merged_at = m;
        prof_publish();  // v50
        ft_sum = ft_n = ft_max = fc_sum = fc_max = td_sum = td_max = 0;
        ft_at = now;
    }
}

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
    s[2] = (tune_set ? 1 : 0) | (palette_default_set ? 2 : 0) | ((rs_refresh_next >= 0) ? 4 : 0) | ((rs_noread_dev != 0) ? 8 : 0) |
           (gw_freeze_kept() ? 16 : 0);  // v48
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
    for (int k = 0; k < 5; k++) {  // v58: options 5-6 are at 236-237
        s[57 + k] = rs_opt_val[k];
    }
    s[62] = rs_opt_known;  // v58: bits 5-6 too
    s[63] = gw_crc8(s, 63);
    s[64] = 'X';
    s[65] = rs_info_seq;
    s[66] = (rs_info_len > 17) ? 17 : rs_info_len;  // a report 8 is 17 bytes with its ID
    memcpy(s + 67, rs_info, s[66]);
    put16(s + 84, ft_avg10);
    put16(s + 86, ft_peak);
    s[88] = fc_avg10;
    s[89] = fc_peak;
    s[90] = merged_1s;
    put16(s + 111, merged_total);
    s[113] = td_avg4;
    s[114] = td_peak4;
    s[115] = sof_avg4;
    s[116] = sof_peak4;
    s[117] = (rs_settings[RS_ANGLE].confirmed ? 1 : 0) | (rs_settings[RS_POS].confirmed ? 2 : 0) |
             (rs_settings[RS_ANGLE].rewrite_on_wake ? 4 : 0) | (rs_settings[RS_POS].rewrite_on_wake ? 8 : 0) |
             (uint8_t) (((ev_seq + gw_hostcap_seq() + gw_turn_seq() + rs_opt_xseq) & 0x0F) << 4);  // v58: + options 5-6  // v21: changes to the recent actions (192-255), v23 also to the host capture (256-511); before the crc at 127
    s[91] = rs_raw_seq;
    s[92] = rs_raw_state;
    s[93] = (rs_raw_len > 17) ? 17 : rs_raw_len;
    memcpy(s + 94, rs_raw, s[93]);
    s[118] = rs_raw_refused;
    s[119] = rs_raw_cmd;
    s[120] = rs_raw_addr >> 8;
    s[121] = rs_raw_addr & 0xFF;
    s[122] = rs_raw_rlen;
    s[123] = rs_cap_rejects;
    s[124] = gw_chords_seq();
    s[125] = gw_chords_flags();
    s[126] = gw_chords_count();
    s[127] = gw_crc8(s + 64, 63);
    s[128] = 'C';
    s[129] = s[124];
    s[130] = s[126];
    memcpy(s + 131, gw_chords_rules(), s[126] * GW_CH_RULE_BYTES);
    s[191] = gw_crc8(s + 128, 63);
    // v21: the recent actions, newest first; the QT Py reads this part when byte 117's high nibble changes
    s[192] = 'E';
    s[193] = (ev_n > GW_EVENTS_SENT) ? GW_EVENTS_SENT : ev_n;  // v58: the newest 7 (236-241 hold options 5-6)
    for (uint8_t k = 0; k < s[193]; k++) {
        const GwEvent& e = ev[(ev_head + GW_EVENTS - 1 - k) % GW_EVENTS];
        uint8_t* q = s + 194 + 6 * k;
        uint32_t age = ev_secs - e.sec;
        put16(q, (age > 65535) ? 65535 : age);
        q[2] = e.src;
        q[3] = e.buttons;
        q[4] = e.action;
        q[5] = (uint8_t) e.value;
    }
    s[236] = rs_opt_val[5];  // v58: sleep time (10 s steps), valid with byte 62 bit 5
    s[237] = rs_opt_val[6];  // v58: competitive mode, valid with byte 62 bit 6
    memset(s + 238, 0, 4);
    gw_dirgain_status(s + 242);  // v24: 242 bit 0 applying, bit 1 got a table; 243 crc8 of the gains as received
    s[244] = gw_hostcap_upd();  // v26: the capture block's change counter and capture number (= its bytes 258 and 257)
    s[245] = gw_hostcap_id();
    gw_turn_status(s + 246);  // v27: 246 state, 247 run number, 248-251 counts sent
    gw_curve_status(s + 252);  // v30: 252 bit 0 applying, bit 1 got a table; 253 crc8 of the curve as received; v39: 252 bits 2-3 and 254 the same for the slow-down gain
    s[255] = gw_crc8(s + 192, 63);
    prof_sub(17);  // v52: the status up to here (link_tick)
    gw_hostcap_fill(s + 256);  // v23
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
                idle_cancel();
                rs_write_value(RS_ADDR_DPI_STAGE, d[0]);
                rs_cur_stage = d[0];
                rs_dpi_last = d[0];
                ev_add(GW_SRC_PAGE, cmd, 0, d[0]);
            }
            break;
        case LINK_SET_ANGLE:
        case LINK_SET_POS:
            if ((len == 1) && rs) {
                RsSetting& s = rs_settings[(cmd == LINK_SET_ANGLE) ? RS_ANGLE : RS_POS];
                rs_setting_set(s, (int8_t) d[0], now);
                ev_add(GW_SRC_PAGE, cmd, 0, s.value);
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
        case LINK_SET_DIRGAIN:
            if (len == 1 + 2 * GW_DIRGAIN_N) {
                uint16_t g[GW_DIRGAIN_N];
                for (int k = 0; k < GW_DIRGAIN_N; k++) {
                    g[k] = get16(d + 1 + 2 * k);
                }
                gw_dirgain_set(d[0] != 0, g);
            }
            break;
        case LINK_CURVE_HIST_RESET:
            gw_curve_hist_reset();
            break;
        case LINK_SET_CURVE:
            gw_curve_link(d, len);
            break;
        case LINK_SET_DECEL:
            gw_curve_decel_link(d, len);
            break;
        case GW_OTA_BEGIN:  // v40: an update through the QT Py (gw_ota.cc)
        case GW_OTA_DATA:
        case GW_OTA_END:
        case GW_OTA_APPLY:
        case GW_OTA_ABORT:
            gw_ota_link(cmd, d, len, now);
            break;
        case LINK_TURN:
            if (len == 12) {
                gw_turn_start((int32_t) ((uint32_t) d[0] | (uint32_t) d[1] << 8 | (uint32_t) d[2] << 16 | (uint32_t) d[3] << 24),
                              (uint32_t) d[4] | (uint32_t) d[5] << 8 | (uint32_t) d[6] << 16 | (uint32_t) d[7] << 24,
                              (uint32_t) d[8] | (uint32_t) d[9] << 8 | (uint32_t) d[10] << 16 | (uint32_t) d[11] << 24);
            } else if (len == 0) {
                gw_turn_stop();
            } else if ((len == 1) && ((d[0] == 1) || (d[0] == 2))) {
                gw_turn_count(d[0] == 2);  // v28: count mode; v34: [2] counts after the speed curve
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
        case LINK_USB_RESET:  // v70
            if (len == 0) {
                ua_manual = true;
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
        case LINK_SET_CHORDS:
            if (!gw_chords_link(d, len, seq)) {
                diag(0x753, len);
            }
            break;
        case LINK_SET_OPTION:
            if ((len == 2) && rs && (d[0] < RS_OPTS) && rs_opt_ok(d[0], d[1])) {
                rs_write_value(rs_opt_addr[d[0]], d[1]);
                rs_opt_set(d[0], d[1]);

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
        gw_where_cmd(f[0]);  // v48
        uint8_t was = gw_where(GW_AT_LINK);
        gw_link_apply(f, n, now);
        gw_where(was);
        if (f[0] != GW_OTA_DATA) {
            status_at = now - 20000;  // show the ack right away (v40: an update's data frames are followed by block 3 instead)
        }
    }
    gw_ota_tick(now);  // v40
    prof_sub(16);  // v52: the link's frame and the update
    if ((now - status_at) >= 20000) {
        status_at = now;
        uint8_t s[GW_LINK_STATUS_LEN];
        gw_link_status(s);  // v52: its bytes 0-255 are section 17 (prof_sub before gw_hostcap_fill)
        gw_link_set_status(s);
    }
    prof_sub(18);  // v52: the host capture's half of the status, and handing the block over
    // v32: the live view's blocks (read only when the QT Py selects them)
    static uint32_t live_at = 0, hist_at = 0;
    if ((now - live_at) >= 10000) {
        live_at = now;
        uint8_t b[GW_LINK_LIVE_LEN];
        gw_curve_live(b);
        gw_link_set_block(1, b);
    }
    if ((now - hist_at) >= 100000) {
        hist_at = now;
        uint8_t b[GW_LINK_HIST_LEN];
        gw_curve_hist(b);
        gw_link_set_block(2, b);
    }
}

bool gw_action_supported(uint8_t a) {
    if (a == GA_RADIO) {
        return true;  // the QT Py's radio, whatever the mouse
    }
    if (family_present(Family::RS)) {
        return a != GA_NONE;
    }
    if (family_present(Family::VUK)) {
        return (a == GA_DPI_NEXT) || (a == GA_ANGLE_RESET);  // the VUK: next stage, sensor switch
    }
    return false;
}

static void gw_action_do(uint8_t a, uint32_t now);

// A chord's action (gw_chords.h), noted in the recent-actions list with where it came from (v21)
void gw_action_src(uint8_t action, uint8_t src, uint8_t mod, uint8_t trig, uint32_t now) {
    uint8_t a = GA_ACTION(action);
    if (a == GA_NONE) {
        return;
    }
    gw_action_do(a, now);
    bool rs = family_present(Family::RS);
    int st = ((a == GA_ANGLE_UP) || (a == GA_ANGLE_DOWN) || (a == GA_ANGLE_RESET)) ? RS_ANGLE
             : ((a == GA_POS_UP) || (a == GA_POS_DOWN) || (a == GA_POS_RESET))     ? RS_POS
                                                                                 : -1;
    int value = ((st >= 0) && rs && rs_settings[st].known) ? rs_settings[st].value : RS_NONE;
    int8_t slot = ev_add(src, (uint8_t) (mod << 4 | trig), a, value);
    if ((st >= 0) && rs && !rs_settings[st].known) {
        ev_wait[st] = slot;  // a step waiting for its read: the value comes with it
    }
}

// Each action does what the mouse at hand supports; the rest do nothing.
static void gw_action_do(uint8_t a, uint32_t now) {
    bool rs = family_present(Family::RS);
    bool vuk = family_present(Family::VUK);
    switch (a) {
        case GA_DPI_NEXT:
        case GA_DPI_PREV:
            dpi_trigger_count++;
            diag(3, dpi_trigger_count);
            if ((a == GA_DPI_NEXT) && (dpi_phase == DpiPhase::IDLE) && vuk) {
                dpi_start(now);
            }
            if (rs) {
                led_restore_now();  // the new stage's own colour is the feedback
                led.requested = false;
                rs_dpi_dir = (a == GA_DPI_NEXT) ? 1 : -1;
                rs_dpi_wanted = true;
            }
            break;
        case GA_ANGLE_UP:
        case GA_ANGLE_DOWN:
            if (rs) {
                rs_step_with_flash(RS_ANGLE, (a == GA_ANGLE_UP) ? 1 : -1, GW_LED_ANGLE, now);
            }
            break;
        case GA_POS_UP:
        case GA_POS_DOWN:
            if (rs) {
                rs_step_with_flash(RS_POS, (a == GA_POS_UP) ? 1 : -1, GW_LED_POS, now);
            }
            break;
        case GA_ANGLE_RESET:
            // VUK: front/rear sensor toggle (what Middle + Left always did there)
            trigger_count++;
            completions = 0;
            if (vuk) {
                front_selected = !front_selected;
                if (front_selected) {
                    send_all(VUK_SELECT_FRONT, sizeof(VUK_SELECT_FRONT) / sizeof(VUK_SELECT_FRONT[0]));
                } else {
                    send_all(VUK_SELECT_REAR, sizeof(VUK_SELECT_REAR) / sizeof(VUK_SELECT_REAR[0]));
                }
            }
            if (rs) {
                diag(1, trigger_count);
                rs_setting_reset(rs_settings[RS_ANGLE], now);
                rs_led_flash(GW_LED_RESET, now);
            }
            break;
        case GA_POS_RESET:
            if (rs) {
                rs_setting_reset(rs_settings[RS_POS], now);
                rs_led_flash(GW_LED_RESET, now);
            }
            break;
        case GA_RADIO:
            // the QT Py sees the count go up in the status block and switches its radio
            radio_toggles++;
            diag(0x750, radio_toggles);
            break;
        case GA_SYNC:
        case GA_SNAP:
        case GA_LOD: {
            if (!rs) {
                break;
            }
            uint8_t k = (a == GA_LOD) ? 1 : (a == GA_SNAP) ? 2 : 3;  // rs_opt_addr order
            if (!(rs_opt_known & (1 << k))) {
                // not read since the mouse was plugged in: read everything now, so the next press works
                if (rs_refresh_next < 0) {
                    rs_refresh_next = 0;
                }
                rs_led_flash(GW_LED_LIMIT, now);
                break;
            }
            uint8_t v = (a == GA_LOD) ? (uint8_t) (rs_opt_val[k] % 5 + 1) : (uint8_t) !rs_opt_val[k];
            rs_write_value(rs_opt_addr[k], v);
            rs_opt_val[k] = v;
            rs_led_flash((a == GA_LOD) ? GW_LED_LOD : (v ? GW_LED_ON : GW_LED_OFF), now);
            break;
        }
        default:
            if ((a >= GA_DPI_1) && (a < GA_DPI_1 + RS_DPI_STAGES) && rs) {
                led_restore_now();
                led.requested = false;
                idle_cancel();
                rs_write_value(RS_ADDR_DPI_STAGE, a - GA_DPI_1);
                rs_cur_stage = a - GA_DPI_1;
                rs_dpi_last = rs_cur_stage;
            }
            break;
    }
}

// regs: the expression registers; unused since v15 (chords come from gw_chords_filter)
// v35: a keyboard tap once the host has configured the Feather, when it enumerated like the XIM (its first device
// descriptor request asks 8 bytes; Windows asks 64): the XIM only counts a keyboard as there after its first key, so
// the first key after an ATEN switch was eaten. F24 (no XIM mapping uses it), about 30 ms, 0.3 s after
// SET_CONFIGURATION, once per capture (each switch powers the Feather up again)
static void wake_tick(uint32_t now) {
    static uint8_t done_for = 0;
    const uint8_t id = gw_hostcap_id();
    if (!id || (id == done_for) || !tud_mounted()) {
        return;
    }
    uint32_t ago = 0;
    if (!gw_hostcap_cfg_ago(now, &ago) || (ago < 300000)) {
        return;
    }
    done_for = id;
    if (gw_hostcap_first_desc_len() == 8) {
        gw_macro_tap(0x00070073, 30);  // keyboard page, F24
    }
}

// v37: the idle stage (see GW_IDLE_US). Put back at once from the frame that shows use (a queued write: reads wait for
// it, so a DPI chord in the same frame reads the stage put back), then read back from idle_tick.
static void idle_restore() {
    idle_low = false;
    led_restore_now();
    led.requested = false;
    rs_write_value(RS_ADDR_DPI_STAGE, idle_back);
    rs_cur_stage = idle_back;
    rs_dpi_last = idle_back;
    idle_checking = true;
    idle_check_busy = false;
    idle_check_tries = 0;
    idle_check_at = time_us_32() + 300000;
}

// v63: pauses in the mouse's reports mid-movement. Two frames with motion at GW_GAP_CMS or faster (the counts of that
// one frame: at 1000 Hz a report a millisecond, so at any DPI over 800 a frame of a hand at that speed never holds 0)
// with 3 or more frames between them that held none: no hand stops dead from that speed and starts again at it, so the
// mouse or its receiver held its reports back (the 11-06 recording: ~10 ms with nothing, then the motion in a burst).
// Counted by the empty frames, cumulative since the Feather started (outside settling, DPI known, not on the idle
// stage), and published once a second as link block 8 (prof_publish)
// v71: at any polling rate. Below 1000 Hz a report holds several frames' motion and the frames between are normally empty, so
// what's counted is how late the report is: the empty frames beyond the rate's own (period - 1), 3 or more. Above 1000 Hz the
// Feather still takes a report a frame. One lost report at 500 Hz (2 ms late) isn't a pause, as one at 1000 Hz isn't
#define GW_GAP_CMS 15
static uint64_t gp_last_us = 0;
static bool gp_last_fast = false;
static uint16_t gp_dpi = 0;
static int64_t gp_thr2 = 0;  // (GW_GAP_CMS cm/s in counts a millisecond)^2 at gp_dpi
static uint32_t gp_quiet = 0;      // v66: frames processed since the last one with motion, that held none
static uint32_t gp_merged_at = 0;  // v66: ticks_merged() at the last frame with motion

static uint8_t gp_period_ms() {  // v71: ms between the mouse's reports as the Feather takes them, 0 = polling not known
    if (!(rs_opt_known & 1)) {
        return 0;
    }
    uint8_t v = rs_opt_val[0];  // RS_ADDR_POLL: 08 125 Hz, 04 250, 02 500, 01 1000, 10-40 2-8 kHz
    return ((v == 1) || (v == 2) || (v == 4) || (v == 8)) ? v : ((v == 0x10) || (v == 0x20) || (v == 0x40)) ? 1 : 0;
}

static void gap_frame(const int32_t* x, const int32_t* y, bool moved) {
    // v66 (review 11-06): only the mouse's own DPI as read (the curve's saved one can be far off: the 15 cm/s line then
    // sits anywhere); v71: any polling rate that's known
    uint16_t dpi = gw_mouse_dpi();
    uint32_t per = gp_period_ms();
    bool poll_ok = per != 0;
    uint64_t now = time_us_64();
    bool ok = dpi && poll_ok && !idle_low && !idle_checking && (now >= settle_until_us);
    if (ok && gw_curve_moving()) {
        gp_frames++;  // the frames the check covered: moving, DPI known, not settling (v66: since the start, published)
    }
    if (!moved) {
        gp_quiet++;
        return;
    }
    if (dpi != gp_dpi) {
        gp_dpi = dpi;
        gp_thr2 = (int64_t) GW_GAP_CMS * GW_GAP_CMS * dpi * dpi / (2540LL * 2540LL);
    }
    int64_t dx = (x != NULL) ? *x : 0, dy = (y != NULL) ? *y : 0;
    bool fast = ok && (dx * dx + dy * dy >= gp_thr2 * per * per) && (gp_thr2 > 0);  // v71: a report holds per ms of motion
    if (fast && gp_last_fast) {
        // v66 (review 11-06): the empty frames as processed, and only when the Feather ran every frame in between (no tick
        // merged, no time missing from the count: a flash write with interrupts off stops the ticks too). Otherwise the
        // Feather's own stall would read as the mouse's pause: the mouse is polled from the same loop
        uint32_t empty = gp_quiet, el = (uint32_t) ((now - gp_last_us + 500) / 1000);
        uint32_t late = (empty >= per - 1) ? empty - (per - 1) : 0;  // v71: the frames beyond the polling rate's own empty ones
        if ((late >= 3) && (ticks_merged() == gp_merged_at) && (el <= empty + 2)) {
            uint8_t k = (late < 5) ? 0 : (late < 10) ? 1 : (late < 20) ? 2 : (late < 50) ? 3 : 4;
            gp_cnt[k]++;
            if (k < 4) {
                gp_ms += late;
                if (late > gp_long) {
                    gp_long = late;
                }
            }
        }
    }
    gp_last_us = now;
    gp_last_fast = fast;
    gp_quiet = 0;
    gp_merged_at = ticks_merged();
}

void gw_gap_totals(uint32_t* cnt, uint32_t* ms, uint32_t* frames) {  // v66: for the host tests (block 8 holds the same)
    memcpy(cnt, gp_cnt, sizeof(gp_cnt));
    *ms = gp_ms;
    *frames = gp_frames;
}

void gw_idle_frame(int32_t* const* ch, const int32_t* x, const int32_t* y) {
    bool moved = ((x != NULL) && (*x != 0)) || ((y != NULL) && (*y != 0));
    if (moved) {
        raw_move_us = time_us_64();  // v59: the still / moving split's fallback with no DPI known
    }
    gap_frame(x, y, moved);  // v63
    bool used = false;
    for (int i = 0; (i < GW_CH_IN) && !used; i++) {
        used = (ch[i] != NULL) && (*ch[i] != 0);
    }
    if (!used) {
        // on the idle stage any movement is the hand (800 DPI showed no noise: the mouse slept), once the switch has
        // landed (the high stage's noise, a count every few ms, can still arrive meanwhile); otherwise only the curve's
        // real movement counts, since at high DPI the noise moves it a count every few ms
        used = idle_low ? ((time_us_64() - idle_low_us >= GW_IDLE_SETTLE_US) && (((x != NULL) && (*x != 0)) || ((y != NULL) && (*y != 0))))
                        : gw_curve_moving();
    }
    if (used) {
        idle_used_us = time_us_64();
        if (idle_low) {
            idle_restore();
        }
    }
}

static void idle_tick(uint32_t now) {
    if (idle_checking) {
        if (!idle_check_busy && ((int32_t) (now - idle_check_at) >= 0) && rs_can_read()) {
            idle_check_busy = rs_try_read(RsRead::IDLE_CHECK, RS_ADDR_DPI_STAGE, 2, now);
            if (!idle_check_busy) {
                idle_check_at = now + 1000000;  // reads paused (asleep): again in a second
            }
        }
        return;
    }
    int cur = rs_cur_stage, low = -1;
    bool unknown = (cur < 0) || (cur >= RS_DPI_STAGES) || !(rs_dpi_known & (1 << cur)) || (rs_dpi_known != (1 << RS_DPI_STAGES) - 1);
    // v65: read them as soon as a mouse plugged in has settled, not only after GW_IDLE_US idle: until then the speed curve
    // takes its saved DPI (11-06: 40000 with the mouse on 7500 DPI, so every speed read 5x too low) and the pause count too
    // (once it moves: the curve only needs the DPI then, and a mouse that just moved is awake to answer)
    if (unknown && !idle_low && family_present(Family::RS) && (time_us_64() >= settle_until_us) && (time_us_64() - raw_move_us < 1000000) &&
        !idle_read_asked && (rs_refresh_next < 0) && rs_reads_work()) {
        idle_read_asked = true;
        rs_refresh_next = 0;
    }
    if (idle_low || !family_present(Family::RS) || (time_us_64() - idle_used_us < GW_IDLE_US)) {
        return;
    }
    if (unknown) {
        // the stage or the DPI table isn't known: after an ATEN switch nothing reads them unless the QT Py page is
        // opened, so read the mouse's settings once (it's awake: the noise that keeps it up answers too)
        if (!idle_read_asked && (rs_refresh_next < 0) && rs_reads_work()) {
            idle_read_asked = true;
            rs_refresh_next = 0;
        }
        return;
    }
    for (int s = 0; s < RS_DPI_STAGES; s++) {
        if ((rs_dpi_known & (1 << s)) && ((low < 0) || (rs_dpi_x[s] < rs_dpi_x[low]))) {
            low = s;
        }
    }
    if ((low < 0) || (rs_dpi_x[low] >= rs_dpi_x[cur])) {
        return;  // already the lowest
    }
    idle_back = cur;
    idle_stage = low;
    idle_low = true;
    idle_low_us = time_us_64();
    led_restore_now();
    led.requested = false;
    rs_write_value(RS_ADDR_DPI_STAGE, low);
    idle_keep();  // v46
    idle_drops++;
    diag(0x707, idle_drops);
}

static void ua_power(bool on) {
#ifdef PICO_DEFAULT_PIO_USB_VBUSEN_PIN
    gpio_put(PICO_DEFAULT_PIO_USB_VBUSEN_PIN, on ? PICO_DEFAULT_PIO_USB_VBUSEN_STATE : !PICO_DEFAULT_PIO_USB_VBUSEN_STATE);
#else
    (void) on;
#endif
}

static void ua_tick() {
    uint64_t t = time_us_64();
    uint32_t stall = out_report_stall_ms();
    if (stall > ua_worst_ms) {
        ua_worst_ms = (stall > 65535) ? 65535 : (uint16_t) stall;
    }
    if (ua_on_at) {  // the port is off
        ua_manual = false;
        if (t >= ua_on_at) {
            ua_on_at = 0;
            ua_power(true);
            settle_now();
            ua_wait_until = t + UA_WAIT_US;
            ua_clean_from = t;
        }
        return;
    }
    if (stall >= UA_STALL_MS) {
        ua_clean_from = t;
    } else if (ua_tries && (t - ua_clean_from >= UA_CLEAN_US)) {
        ua_tries = 0;  // clean for a good while: that was a separate hang, the next one starts over
        ua_gave_up = false;
    }
    bool hung = (stall >= UA_STALL_MS) && (t >= ua_wait_until) && (t >= settle_until_us);  // v72: not while devices mount (the control pipe is shared)
    if (hung && (ua_tries >= UA_MAX_TRIES)) {
        ua_gave_up = true;
    }
    if (ua_manual || (hung && !ua_gave_up)) {
        if (ua_manual) {
            ua_tries = 0;
            ua_gave_up = false;
        } else {
            ua_tries++;
        }
        ua_manual = false;
        if (ua_resets < 255) {
            ua_resets++;
        }
        ua_power(false);
        ua_off_pub = true;
        ua_on_at = t + UA_OFF_US;
        settle_now();
        return;
    }
    if (gw_ua_beats && (stall == 0) && (t >= ua_wait_until) && (t >= settle_until_us) && (t - raw_move_us >= UA_QUIET_US) && (t - ua_beat_at >= UA_BEAT_US) && rs_can_read()) {
        ua_beat_at = t;
        std::map<uint8_t, uint16_t> targets = rs_targets();
        if (out_report_free_slots() >= targets.size()) {
            uint8_t r[RS_LEN];
            rs_build(r, RS_INFO_CMD, 0, NULL, 0);  // v72: the status query: no address, so no reply to it can pass for a read's
            for (auto const& [dev_addr, itf] : targets) {
                (void) dev_addr;
                queue_out_report(itf, RS_REPORT_ID, r, RS_LEN);  // rs_read stays NONE: the reply is dropped in gw_on_input_report
            }
        }
    }
}

void gw_ua_state(uint8_t* resets, uint8_t* tries, bool* off, bool* gave_up, uint16_t* worst_ms) {
    *resets = ua_resets;
    *tries = ua_tries;
    *off = ua_on_at != 0;
    *gave_up = ua_gave_up;
    *worst_ms = ua_worst_ms;
}

void gw_tick(const int32_t* regs) {
    uint32_t now = time_us_32();
    static bool started = false;
    if (!started) {
        started = true;
        idle_keep_load();  // v46
    }
    known_itf_refresh(now);  // v52
    pf_sub_last = now;  // v51: gw_tick's parts (profile sections 13-19)
    wake_tick(now);
    ev_tick(now);
    gw_chords_service();
    dpi_tick(now);
    prof_sub(13);  // v52: 13 wake, events, chords, DPI; 14 mouse settings; 15 idle stage; 16-19 in link_tick
    rs_tick(now);
    ua_tick();  // v70
    prof_sub(14);
    idle_tick(now);
    prof_sub(15);
    link_tick(now);
    // Also dump everything every 3 s while the Monitor tab is open, so a device that
    // never mounts (no Mid+Left possible) still shows its enumeration trace.
    static uint32_t last_dump_us = 0;
    if (monitor_enabled && (diag_count == 0) && (now - last_dump_us > 3000000)) {
        last_dump_us = now;
        diag_all();
    }
    diag_flush();
    prof_sub(19);
}

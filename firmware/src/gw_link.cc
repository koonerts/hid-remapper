#include "gw_link.h"

#include <cstring>

// pico.h brings in the board header, which is where GW_LINK_I2C comes from. Without it the test
// below sees nothing and the no-op versions get built (v11 shipped that way).
#include "pico.h"

#if defined(FEATHER_HOST_BOARD) && !defined(GW_LINK_I2C)
#error "feather_host.h must define GW_LINK_I2C, GW_LINK_SDA and GW_LINK_SCL"
#endif

// v25: table-driven (same result as the bit-by-bit loop, poly 0x07): the status block's ~500 bytes of CRC every 20 ms ran
// inside process_mapping and cost ~0.2-0.3 ms of that frame (audit 10-21); about 8 cycles a byte now
static const uint8_t CRC8_07[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3,
};

uint8_t gw_crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc = CRC8_07[crc ^ data[i]];
    }
    return crc;
}


#ifdef GW_LINK_I2C

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "pico/i2c_slave.h"

// Everything the interrupt touches. One frame mailbox: a frame that arrives while the previous
// one hasn't been taken yet is dropped (the QT Py sends one command at a time and checks the ack).
static uint8_t rx[GW_LINK_FRAME_MAX];
static volatile uint8_t rx_n = 0;
static volatile bool rx_overflow = false;
static uint8_t frame[GW_LINK_FRAME_MAX];
static volatile uint8_t frame_n = 0;  // 0 = mailbox empty
// Two status buffers: a read latches one at its first byte, the main loop fills the other.
static uint8_t status_buf[2][GW_LINK_STATUS_LEN];
static volatile uint8_t status_cur = 0;
static volatile uint8_t status_reading = 0;
static volatile uint16_t tx_i = 0;  // the block is 256 bytes since v21
// v32: the live and histogram blocks, double-buffered the same way; sel = what the next read returns
static uint8_t live_buf[2][GW_LINK_LIVE_LEN];
static uint8_t hist_buf[2][GW_LINK_HIST_LEN];
static volatile uint8_t live_cur = 0, hist_cur = 0;
static volatile uint8_t sel = 0;
static const uint8_t* volatile tx_buf = nullptr;
static volatile uint16_t tx_len = 0;

// In RAM: firmware/src code runs from flash (remapper_single.ld), and this runs from the I2C
// interrupt, which stays on while HID Remapper's config or the chord table is written to flash
// (XIP is off then; a handler fetched from flash would fault and hang the Feather).
static void __not_in_flash_func(link_irq)(i2c_inst_t* i2c, i2c_slave_event_t event) {
    switch (event) {
        case I2C_SLAVE_RECEIVE: {
            uint8_t b = i2c_read_byte_raw(i2c);
            if (rx_n < GW_LINK_FRAME_MAX) {
                rx[rx_n++] = b;
            } else {
                rx_overflow = true;
            }
            break;
        }
        case I2C_SLAVE_REQUEST:
            if (tx_i == 0) {
                if (sel == 1) {
                    tx_buf = live_buf[live_cur];
                    tx_len = GW_LINK_LIVE_LEN;
                } else if (sel == 2) {
                    tx_buf = hist_buf[hist_cur];
                    tx_len = GW_LINK_HIST_LEN;
                } else {
                    status_reading = status_cur;
                    tx_buf = status_buf[status_reading];
                    tx_len = GW_LINK_STATUS_LEN;
                }
            }
            i2c_write_byte_raw(i2c, (tx_i < tx_len) ? tx_buf[tx_i] : 0xFF);
            tx_i++;
            break;
        case I2C_SLAVE_FINISH:
            if ((rx_n == 2) && !rx_overflow && (rx[0] == GW_LINK_SEL)) {
                sel = rx[1];  // v32: not a frame (those are 4 bytes at least)
            } else {
                if ((rx_n > 0) && !rx_overflow && (frame_n == 0)) {
                    memcpy(frame, rx, rx_n);
                    frame_n = rx_n;
                }
                if (tx_i > 0) {
                    sel = 0;
                }
            }
            rx_n = 0;
            rx_overflow = false;
            tx_i = 0;
            break;
        default:
            break;
    }
}

void gw_link_init() {
    i2c_init(GW_LINK_I2C, 100 * 1000);
    gpio_set_function(GW_LINK_SDA, GPIO_FUNC_I2C);
    gpio_set_function(GW_LINK_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(GW_LINK_SDA);
    gpio_pull_up(GW_LINK_SCL);
    i2c_slave_init(GW_LINK_I2C, GW_LINK_ADDR, &link_irq);
    // below the USB frame timer, so a link byte can never delay it
    irq_set_priority((i2c_get_index(GW_LINK_I2C) == 0) ? I2C0_IRQ : I2C1_IRQ, PICO_LOWEST_IRQ_PRIORITY);
}

bool gw_link_take_frame(uint8_t* buf, uint8_t* len) {
    uint8_t n = frame_n;
    if (n == 0) {
        return false;
    }
    memcpy(buf, frame, n);
    *len = n;
    frame_n = 0;  // the interrupt only fills the mailbox while it's empty
    return true;
}

void gw_link_set_status(const uint8_t* status) {
    uint8_t next = status_cur ^ 1;
    if ((tx_i != 0) && (status_reading == next)) {
        return;  // that buffer is being read right now; the next update goes through
    }
    memcpy(status_buf[next], status, GW_LINK_STATUS_LEN);
    status_cur = next;
}

void gw_link_set_block(uint8_t which, const uint8_t* data) {
    uint8_t* b0 = (which == 1) ? live_buf[0] : hist_buf[0];
    uint8_t* b1 = (which == 1) ? live_buf[1] : hist_buf[1];
    volatile uint8_t& cur = (which == 1) ? live_cur : hist_cur;
    uint8_t next = cur ^ 1;
    if ((tx_i != 0) && (tx_buf == (next ? b1 : b0))) {
        return;  // being read right now
    }
    memcpy(next ? b1 : b0, data, (which == 1) ? GW_LINK_LIVE_LEN : GW_LINK_HIST_LEN);
    cur = next;
}

#else

void gw_link_init() {
}

bool gw_link_take_frame(uint8_t* buf, uint8_t* len) {
    return false;
}

void gw_link_set_status(const uint8_t* status) {
}

void gw_link_set_block(uint8_t which, const uint8_t* data) {
}

#endif

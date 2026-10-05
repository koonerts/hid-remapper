#include "gw_link.h"

#include <cstring>

uint8_t gw_crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t) ((crc << 1) ^ 0x07) : (uint8_t) (crc << 1);
        }
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
static volatile uint8_t tx_i = 0;

static void link_irq(i2c_inst_t* i2c, i2c_slave_event_t event) {
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
                status_reading = status_cur;
            }
            i2c_write_byte_raw(i2c, (tx_i < GW_LINK_STATUS_LEN) ? status_buf[status_reading][tx_i] : 0xFF);
            tx_i++;
            break;
        case I2C_SLAVE_FINISH:
            if ((rx_n > 0) && !rx_overflow && (frame_n == 0)) {
                memcpy(frame, rx, rx_n);
                frame_n = rx_n;
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

#else

void gw_link_init() {
}

bool gw_link_take_frame(uint8_t* buf, uint8_t* len) {
    return false;
}

void gw_link_set_status(const uint8_t* status) {
}

#endif

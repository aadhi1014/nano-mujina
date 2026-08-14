#ifndef UART_TRANSPORT_H
#define UART_TRANSPORT_H

#include <stdint.h>
#include <sys/ioctl.h>

#define UART_DEFAULT_BAUD_RATE  115200
#define UART_HIGH_BAUD_RATE     4800000

#define IOC_SET_BAUDRATE  _IOW('U', 0x40, int)
struct uart_configure
{
        uint32_t baud_rate;
        uint32_t data_bits    :4;
        uint32_t stop_bits    :2;
        uint32_t parity       :2;
        uint32_t fifo_lenth   :2;
        uint32_t auto_flow    :1;
        uint32_t reserved     :21;
};

typedef enum {
        UART_PARITY_NONE,
        UART_PARITY_ODD,
        UART_PARITY_EVEN
} uart_parity_t;

typedef enum {
        UART_RECEIVE_FIFO_1,
        UART_RECEIVE_FIFO_8,
        UART_RECEIVE_FIFO_16,
        UART_RECEIVE_FIFO_30,
} uart_receive_trigger_t;

void uart_init(uint8_t channel, uint32_t baud);
void uart_config_baud(uint8_t channel, uint32_t baud);
void uart_clear_rxfifo(uint8_t channel);
int uart_read(uint8_t channel, uint8_t *buf, uint16_t len);
int uart_write(uint8_t channel, uint8_t *buf, uint16_t len);
/* Raw fd for a channel, for callers (the ASIC frame-protocol layer in
 * chip_link.c) that need to select() on it directly. -1 if not open. */
int uart_get_fd(uint8_t channel);

#endif

/*
 * Raw UART transport for this board's chip-communication channels
 * (/dev/uart1..4): open/configure/read/write.
 */

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/select.h>
#include "uart_transport.h"

#define UART_CHANNEL_COUNT 4

typedef struct {
        const char *device_path;
        int fd;
} uart_channel_t;

static uart_channel_t channels[UART_CHANNEL_COUNT] = {
        { "/dev/uart1", -1 },
        { "/dev/uart2", -1 },
        { "/dev/uart3", -1 },
        { "/dev/uart4", -1 },
};

static void apply_baud_config(int fd, uint32_t baud)
{
        struct uart_configure config;

        memset(&config, 0, sizeof(config));
        config.baud_rate = baud;
        config.data_bits = 8;
        config.stop_bits = 1;
        config.parity = UART_PARITY_NONE;
        config.fifo_lenth = UART_RECEIVE_FIFO_16;
        config.auto_flow = 0;

        ioctl(fd, IOC_SET_BAUDRATE, &config);
}

void uart_init(uint8_t channel, uint32_t baud)
{
        uart_channel_t *ch = &channels[channel % UART_CHANNEL_COUNT];

        ch->fd = open(ch->device_path, O_RDWR);
        if (ch->fd < 0)
                return;

        apply_baud_config(ch->fd, baud);
}

void uart_config_baud(uint8_t channel, uint32_t baud)
{
        uart_channel_t *ch = &channels[channel % UART_CHANNEL_COUNT];

        apply_baud_config(ch->fd, baud);
}

int uart_read(uint8_t channel, uint8_t *buf, uint16_t len)
{
        uart_channel_t *ch = &channels[channel % UART_CHANNEL_COUNT];

        return read(ch->fd, buf, len);
}

int uart_write(uint8_t channel, uint8_t *buf, uint16_t len)
{
        uart_channel_t *ch = &channels[channel % UART_CHANNEL_COUNT];

        /* Writes raw bytes to the UART channel. */
        return write(ch->fd, buf, len);
}

int uart_get_fd(uint8_t channel)
{
        return channels[channel % UART_CHANNEL_COUNT].fd;
}

void uart_clear_rxfifo(uint8_t channel)
{
        /* Drains the receive FIFO: select() with a short timeout before
         * each read, stopping once no data is ready within it. */
        uart_channel_t *ch = &channels[channel % UART_CHANNEL_COUNT];
        uint8_t buf[256];
        int ret;
        fd_set read_fds;
        struct timeval timeout;

        for (;;) {
                FD_ZERO(&read_fds);
                FD_SET(ch->fd, &read_fds);
                timeout.tv_sec = 0;
                timeout.tv_usec = 2000;

                ret = select(ch->fd + 1, &read_fds, NULL, NULL, &timeout);
                if (ret <= 0)
                        break;

                ret = uart_read(channel, buf, sizeof(buf));
                if (ret <= 0)
                        break;
        }
}

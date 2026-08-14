#ifndef CHIP_LINK_H
#define CHIP_LINK_H

#include <stdint.h>
#include <stdio.h>
#include "uart_transport.h"

#define UART_TX_BUFFER_SIZE		256
#define UART_RX_BUFFER_SIZE		256

enum uart_mode {
	UART_READ_MODE = 0,
	UART_WRITE_MODE = 1,
	UART_BAUD_MODE = 2,
	UART_INIT_MODE = 3,
};

void a3197s_send_baud_cmd(uint8_t channel, uint32_t baud);
void a3197s_set_active_chip(uint8_t channel, uint16_t chip_addr);
void a3197s_send_chiplast_cmd(uint8_t channel, uint8_t value);
uint16_t a3197s_get_active_chip(void);
int a3197s_recv_frame(uint8_t channel, uint32_t reg_addr, uint32_t *data_recv, uint32_t len);
int a3197s_send_frame(uint8_t channel, uint32_t reg_addr, uint32_t *data_send, uint32_t len, enum uart_mode mode);
int a3197s_recv_frame_ex(uint8_t channel, uint32_t *chipid, uint32_t *reg_addr, uint32_t *data_recv, uint32_t *plen, int check_len);
/* Optional read/write wire trace. Off by default (pass NULL to
 * disable); every a3197s_send_frame() call and a3197s_recv_frame_ex()
 * outcome is logged to fp when set. */
void uart_set_wiretrace_log(FILE *fp);

#endif

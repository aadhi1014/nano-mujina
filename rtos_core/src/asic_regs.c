/*
 * A3197S register-access primitives: the thin wrappers over chip_link.c's
 * wire-frame layer (a3197s_write_fifo/a3197s_read_fifo), single-register
 * read/write helpers, chip-select tracking, error-block clearing, and
 * the UART baud-change sequence. Every other asic_*.c file builds on
 * top of these.
 */

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "chip_link.h"
#include "asic_engine.h"
#include "asic_internal.h"

static uint16_t g_selected_chip[HBOARD_COUNT];
uint32_t g_error_count[HBOARD_COUNT];
uint32_t g_regread_err[HBOARD_COUNT];

int a3197s_write_fifo(uint8_t channel, uint32_t reg_addr, uint32_t *data_send, uint32_t len, uint8_t mode)
{
	return a3197s_send_frame(channel, reg_addr, data_send, len, mode);
}

/* Writes `count` words to addr in a single UART_WRITE_MODE burst. */
void asic_raw_write_words(uint8_t channel, uint32_t addr, const uint32_t *words, uint32_t count)
{
	a3197s_write_fifo(channel, addr, (uint32_t *)words, count, UART_WRITE_MODE);
}

uint32_t a3197s_soft_reset(uint8_t channel)
{
	a3197s_set_reg(channel, REG_UART_CFG3, DEFAULT_UART_CFG3);
	uart_clear_rxfifo(channel);

	a3197s_set_reg(channel, REG_NONCE_UPDATE, 0x80000000);
	a3197s_set_reg(channel, REG_ASIC_CFG, DEFAULT_ASIC_CFG);
	g_error_count[channel]++;
	return 0;
}

int a3197s_read_fifo(uint8_t channel, uint32_t reg_addr, uint32_t *data_recv, uint32_t len)
{
	int ret = 0;

	ret = a3197s_recv_frame(channel, reg_addr, data_recv, len);
	/* ret==1: a frame started arriving but failed CRC/framing/addressing --
	 * counted as a failure and triggers a soft reset. ret==2 (nothing came
	 * back at all) is not treated as an error here. */
	if(ret == 1)
	{
		g_regread_err[channel]++;
		if (g_debug_fp) {
			fprintf(g_debug_fp, "regread_fail channel=%u chip=%u addr=0x%x len=%u\n",
				channel, g_selected_chip[channel], reg_addr, len);
			fflush(g_debug_fp);
		}
		/* Skip the soft reset for telemetry-only registers (SmartSpeed/
		 * PLLCNT1, PLL0-3L1 readback, PVT temp sensor) so a failed
		 * telemetry read doesn't discard nonces already queued in the
		 * UART RX FIFO. Core registers still soft-reset on failure. */
		if (reg_addr != REG_SS_CORE_DIST && reg_addr != REG_PLL0_FREQ &&
		    reg_addr != REG_PLL1_FREQ && reg_addr != REG_PLL2_FREQ &&
		    reg_addr != REG_PLL3_FREQ && reg_addr != REG_PVT_RESULT) {
			a3197s_soft_reset(channel);
		}
	}

	return ret;
}

void a3197s_efuse_read(uint8_t channel, uint32_t *data)
{
	union asic_efuse_ctrl efuse_ctrl;
	uint32_t i = 0;

	for (i = 0; i < 16; i++)
		data[i] = 0;

	a3197s_set_reg(channel, REG_EFUSE_CTRL, 0x08);
	a3197s_set_reg(channel, REG_EFUSE_CTRL, 0x01);

	efuse_ctrl.raw = a3197s_get_reg(channel, REG_EFUSE_CTRL);
	if(efuse_ctrl.sense_rdy)
	{
		a3197s_write_fifo(channel, REG_EFUSE_FIFO, NULL, 16, UART_READ_MODE);
		a3197s_read_fifo(channel, REG_EFUSE_FIFO, data, 16);
	}
}

uint16_t asic_get_chip_select(void)
{
	return a3197s_get_active_chip();
}

void a3197s_set_uart_baud(uint8_t channel, uint32_t baud)
{
	a3197s_send_baud_cmd(channel, baud);
	delay_ms(30);
	uart_config_baud(channel, baud);
	delay_ms(30);
}

void a3197s_select_chip(uint8_t channel_addr, uint16_t chip_addr)
{
	g_selected_chip[channel_addr] = chip_addr;
	a3197s_set_active_chip(channel_addr, chip_addr);
}

void a3197s_clear_ecc(uint8_t channel, uint8_t index)
{
	uint32_t tmp[7] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
	a3197s_write_fifo(channel, REG_UART_ERR_BLOCK, tmp, 7, UART_WRITE_MODE);
	g_error_count[channel] = 0;
}

int a3197s_set_reg(uint8_t channel, uint32_t addr, uint32_t value)
{
	a3197s_write_fifo(channel, addr, &value, 1, UART_WRITE_MODE);
	return 0;
}

uint32_t a3197s_get_reg(uint8_t channel, uint32_t addr)
{
	uint32_t tmp32 = 0;

	a3197s_write_fifo(channel, addr, NULL, 1, UART_READ_MODE);
	a3197s_read_fifo(channel, addr, &tmp32, 1);
	return tmp32;
}

/* Like a3197s_get_reg(), but returns the transport read/write result
 * instead of discarding it, so a failed read (which leaves *out at 0)
 * can be told apart from a genuine register value of 0. */
int a3197s_get_reg_ex(uint8_t channel, uint32_t addr, uint32_t *out)
{
	*out = 0;
	a3197s_write_fifo(channel, addr, NULL, 1, UART_READ_MODE);
	return a3197s_read_fifo(channel, addr, out, 1);
}

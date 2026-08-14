/*
 * A3197S chain enumeration: the UART_INIT_MODE broadcast that discovers
 * how many chips are on the chain, plus per-chip enumeration
 * verification diagnostics (write/read-back pattern checks and
 * REG_UART_STATUS address confirmation).
 */

#include <stdint.h>
#include "util.h"
#include "chip_link.h"
#include "asic_engine.h"
#include "asic_internal.h"

static char g_chipid[7] = "A3197S";
uint32_t g_asic_count[HBOARD_COUNT];

char *a3197s_get_chipid(uint8_t channel)
{
	return g_chipid;
}

uint32_t a3197s_enumerate(uint8_t channel)
{
	uint32_t tmp32 = 0;
	uint32_t asic_sum = 0;
	uint32_t ret = 0;

	a3197s_select_chip(channel, CHIP_ADDR_BROADCAST);

	a3197s_set_reg(channel, REG_UART_CFG1, DEFAULT_UART_CFG1);
	a3197s_send_chiplast_cmd(channel, 0);
	uart_clear_rxfifo(channel);

	a3197s_write_fifo(channel, 0x0, &tmp32, 1, UART_INIT_MODE);
	delay_ms(150);
	ret = a3197s_read_fifo(channel, 0x0, &tmp32, 1);
	if(ret)
	{
		return 0;
	}
	asic_sum = tmp32;
	if (channel < HBOARD_COUNT)
		g_asic_count[channel] = asic_sum;
	return asic_sum;
}

/* Per-chip enumeration verification: selects each chip, writes and
 * reads back a verify pattern, then does a final broadcast write.
 * Returns 0 if every chip verified, nonzero otherwise. Diagnostic only. */
int asic_enum_verify(uint8_t channel, uint32_t chip_count)
{
	uint32_t chip;
	uint32_t verify;
	uint32_t tmp32;
	int ret;
	int fail_count = 0;

	for (chip = 0; chip < chip_count; chip++) {
		a3197s_select_chip(channel, (uint16_t)chip);
		tmp32 = 0xFFFFFFFE;
		a3197s_write_fifo(channel, 0x4e0, &tmp32, 1, UART_WRITE_MODE);
		/* A separate read-request write is required to get the value back. */
		a3197s_write_fifo(channel, 0x4e0, NULL, 1, UART_READ_MODE);
		ret = a3197s_read_fifo(channel, 0x4e0, &verify, 1);
		if (ret || verify != 0xFFFFFFFE)
			fail_count++;
	}

	a3197s_select_chip(channel, CHIP_ADDR_BROADCAST);
	tmp32 = 0xFFFFFFFF;
	a3197s_write_fifo(channel, 0x4e0, &tmp32, 1, UART_WRITE_MODE);

	return fail_count;
}

/* Reads REG_UART_STATUS (0x3CA0) on the last enumerated chip: bits
 * [9:0] are chip ID, bit 10 is the chip-last flag, bits [31:11] are a
 * raw (undecoded) UART frequency ratio. Returns 0 if chip ID matches
 * chip_count-1, nonzero otherwise. Diagnostic only. */
int asic_enum_verify_lastchip(uint8_t channel, uint32_t chip_count,
	uint32_t *out_chipid, uint32_t *out_chiplast, uint32_t *out_freq_ratio_raw)
{
	uint32_t status = 0;
	int ret;

	if (chip_count == 0)
		return 1;

	a3197s_select_chip(channel, (uint16_t)(chip_count - 1));
	/* Sends the read-request frame before listening; a3197s_read_fifo()
	 * only listens, it never transmits. */
	a3197s_write_fifo(channel, REG_UART_STATUS, NULL, 1, UART_READ_MODE);
	ret = a3197s_read_fifo(channel, REG_UART_STATUS, &status, 1);

	if (out_chipid)
		*out_chipid = status & 0x3FF;
	if (out_chiplast)
		*out_chiplast = (status >> 10) & 0x1;
	if (out_freq_ratio_raw)
		*out_freq_ratio_raw = status >> 11;

	if (ret)
		return 1;

	return (status & 0x3FF) != (chip_count - 1);
}

/* Same check as asic_enum_verify_lastchip(), applied to every chip.
 * out_ret[chip]: 0=success, 1=failure (frame started but invalid),
 * 2=no response. out_chipid[chip] only meaningful when out_ret[chip]==0.
 * Caller must size both arrays to at least chip_count. */
void asic_enum_verify_alladdr(uint8_t channel, uint32_t chip_count,
	int *out_ret, uint32_t *out_chipid)
{
	uint32_t chip;

	for (chip = 0; chip < chip_count; chip++) {
		uint32_t status = 0;
		int ret;

		a3197s_select_chip(channel, (uint16_t)chip);
		/* Read-request write is required here too. */
		a3197s_write_fifo(channel, REG_UART_STATUS, NULL, 1, UART_READ_MODE);
		ret = a3197s_read_fifo(channel, REG_UART_STATUS, &status, 1);

		out_ret[chip] = ret;
		out_chipid[chip] = status & 0x3FF;
	}

	a3197s_select_chip(channel, CHIP_ADDR_BROADCAST);
}

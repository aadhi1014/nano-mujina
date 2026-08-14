/*
 * A3197S ASIC bring-up: per-chain UART RX FIFO reset, the fixed
 * ~40-register init sequence (core reset, error-block clear, ASIC/PLL/
 * SmartSpeed-threshold defaults), and a3197s_init_chain()'s full
 * broadcast-select -> init -> clear-ecc -> config-regs -> PVT-init ->
 * counter-reset bring-up sequence.
 */

#include <stdint.h>
#include <string.h>
#include "chip_link.h"
#include "pvt_sensor.h"
#include "asic_engine.h"
#include "asic_internal.h"

/* th0-4 are the SmartSpeed pass/fail thresholds (REG_SS_THRESH0-4),
 * applied identically to all HBOARD_COUNT channels. */
static asic_base_setting g_base_setting = {
	.mask = 0xff,
	.ncheck = 0,
	.roll_en = 0,
	.p_int = 20,
	.ss = {
		{
			.en = 0,
			.p_sel = 0,
			.ssdn = 0,
			.clk_sel = 0,
			.low = 0,
			.high = 0,
			.th0 = 0x00AA2710,
			.th1 = 0x00007FFF,
			.th2 = 0x0013D620,
			.th3 = 0x00000008,
			.th4 = 0x00000000,
		},
		{
			.en = 0,
			.p_sel = 0,
			.ssdn = 0,
			.clk_sel = 0,
			.low = 0,
			.high = 0,
			.th0 = 0x00AA2710,
			.th1 = 0x00007FFF,
			.th2 = 0x0013D620,
			.th3 = 0x00000008,
			.th4 = 0x00000000,
		},
		{
			.en = 0,
			.p_sel = 0,
			.ssdn = 0,
			.clk_sel = 0,
			.low = 0,
			.high = 0,
			.th0 = 0x00AA2710,
			.th1 = 0x00007FFF,
			.th2 = 0x0013D620,
			.th3 = 0x00000008,
			.th4 = 0x00000000,
		},
		{
			.en = 0,
			.p_sel = 0,
			.ssdn = 0,
			.clk_sel = 0,
			.low = 0,
			.high = 0,
			.th0 = 0x00AA2710,
			.th1 = 0x00007FFF,
			.th2 = 0x0013D620,
			.th3 = 0x00000008,
			.th4 = 0x00000000,
		},
	},
};

void asic_init(uint8_t channel)
{
	uart_clear_rxfifo(channel);
}

static int a3197s_configure_regs(uint8_t miner_id)
{
	/* Core soft reset (W1T), before the rest of setup. */
	a3197s_set_reg(miner_id, REG_CORE_RST, 1);

	/* Clears the UART FSM/timeout/header/CRC/glitch error-counter
	 * block (REG_UART_ERR_BLOCK, 0x3ca4-0x3cbc), a 7-word W1C burst. */
	{
		static const uint32_t clear_burst[7] = {
			0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
			0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
		};
		asic_raw_write_words(miner_id, REG_UART_ERR_BLOCK, clear_burst, 7);
	}

	a3197s_set_reg(miner_id, REG_ASIC_CFG, DEFAULT_ASIC_CFG);
	a3197s_set_reg(miner_id, REG_ROLLTIME, DEFAULT_ROLLTIME);
	a3197s_set_reg(miner_id, REG_TBASE, 0);
	a3197s_set_reg(miner_id, REG_CG_EN, DEFAULT_CG_EN);
	a3197s_set_reg(miner_id, REG_SPD_SYNC_CFG, DEFAULT_SPD_SYNC_CFG);
	a3197s_set_reg(miner_id, REG_UNK_220, 0x04C08198);
	a3197s_set_reg(miner_id, REG_UNK_22C, 0x80000100);
	a3197s_set_reg(miner_id, REG_UNK_234, 0x0000000E);
	a3197s_set_reg(miner_id, REG_SS_CTRL, DEFAULT_SS_CTRL);
	a3197s_set_reg(miner_id, REG_PLL_TABLE_SEL, DEFAULT_PLL_TABLE_SEL);
	a3197s_set_reg(miner_id, REG_SS_LOG_TIMER, g_ss_timer_default_value);
	a3197s_set_reg(miner_id, REG_RFU1, 0xFFFF0001);
	a3197s_set_reg(miner_id, REG_UNK_448, 0x00000000);
	a3197s_set_reg(miner_id, REG_UNK_450, 0xFFF80020);
	a3197s_set_reg(miner_id, REG_UNK_490, 0x00000000);
	a3197s_set_reg(miner_id, REG_UNK_498, 0x0001FFFF);
	a3197s_set_reg(miner_id, REG_RFU2, 0x0001FFFF);
	a3197s_set_reg(miner_id, REG_UNK_4B4, 0x04000000);
	a3197s_set_reg(miner_id, REG_UNK_4C0, 0x0001FFFF);
	a3197s_set_reg(miner_id, REG_UNK_4C4, 0x00040000);
	a3197s_set_reg(miner_id, REG_UNK_4CC, 0x00000190);
	a3197s_set_reg(miner_id, REG_RFU4, 0xFFFFFFFF);
	a3197s_set_reg(miner_id, REG_RFU5, 0xFFFFFFFF);
	a3197s_set_reg(miner_id, REG_RFU6, 0xFFFFFFFF);
	a3197s_set_reg(miner_id, REG_RFU7, 0x07FFFFFF);
	a3197s_set_reg(miner_id, REG_PLL_COMMON_CFG, DEFAULT_PLL_COMMON_CFG);
	a3197s_set_reg(miner_id, REG_PLL0_FREQ, DEFAULT_PLL0_FREQ);
	a3197s_set_reg(miner_id, REG_PLL1_FREQ, DEFAULT_PLL1_FREQ);
	a3197s_set_reg(miner_id, REG_PLL2_FREQ, DEFAULT_PLL2_FREQ);
	a3197s_set_reg(miner_id, REG_PLL3_FREQ, DEFAULT_PLL3_FREQ);
	a3197s_set_reg(miner_id, REG_PLL_CTRL01, 0);
	a3197s_set_reg(miner_id, REG_PLL_CTRL23, 0);
	a3197s_set_reg(miner_id, REG_PLL_COMMIT, DEFAULT_PLL_COMMIT);
	a3197s_set_reg(miner_id, REG_DIVCNT_CTRL, DEFAULT_DIVCNT_CTRL);
	a3197s_set_reg(miner_id, REG_SS_THRESH0, g_base_setting.ss[miner_id].th0);
	a3197s_set_reg(miner_id, REG_SS_THRESH1, g_base_setting.ss[miner_id].th1);
	a3197s_set_reg(miner_id, REG_SS_THRESH2, g_base_setting.ss[miner_id].th2);
	a3197s_set_reg(miner_id, REG_SS_THRESH3, g_base_setting.ss[miner_id].th3);
	a3197s_set_reg(miner_id, REG_SS_THRESH4, g_base_setting.ss[miner_id].th4);
	a3197s_set_reg(miner_id, REG_RI_WRITE_EN, 1);
	a3197s_set_reg(miner_id, REG_RI_ENABLE, 0);
	a3197s_set_reg(miner_id, REG_RI_WRITE_EN, 0);
	a3197s_set_reg(miner_id, REG_CHKDROP, 1);
	a3197s_set_reg(miner_id, REG_NONCE_CNT_MODE, 1);
	/* asic_config_noncemask() is intentionally not called here; the
	 * noncemask sequence is configured separately, right before the
	 * first job/work burst -- see asic_set_noncemask() below. */
	a3197s_set_reg(miner_id, REG_SS_LOG_RESET, 1);

	return 0;
}

int a3197s_init_chain(uint8_t miner_id)
{
	uint32_t j, k;

	a3197s_select_chip(miner_id, CHIP_ADDR_BROADCAST);

	asic_init(miner_id);

	a3197s_clear_ecc(miner_id, 2);
	a3197s_configure_regs(miner_id);

	a3197s_pvt_init(miner_id, CHIP_ADDR_BROADCAST);

	g_error_count[miner_id] = 0;

	memset(g_work_vmask, 0, sizeof(g_work_vmask));
	for (j = 0; j < MAX_ASIC_ON_SINGLE_HASH; j++) {
		for (k = 0; k < PLL_DOMAIN_COUNT; k++) {
			g_pll_state.set_pll[miner_id][j][k] = 100;
			g_pll_state.get_pll[miner_id][j][k] = 100;
		}
	}

	return 0;
}

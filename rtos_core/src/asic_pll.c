/*
 * A3197S PLL programming: MHz<->register-encoding table lookups, the
 * blocking one-second-per-step ramp, and a3197s_pll_apply() (formerly
 * asic_set_miner_pll()) -- the PLL0-3 register write sequence bracketed
 * by the SmartSpeed SS_CTRL PRE/MID/COMMIT handshake (see
 * asic_smartspeed.c's a3197s_smartspeed_set_ctrl()). The call sequence
 * and every register write below are unchanged from the pre-split
 * function; only the 3 repeated SS_CTRL+SPD_SYNC_CFG write pairs were
 * factored into a shared helper.
 */

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "pll_cpm_table.h"
#include "asic_engine.h"
#include "asic_internal.h"

a3197s_pll_state_t g_pll_state;

static uint32_t get_cpm_pll(uint32_t freq)
{
	uint32_t i;
	for (i = 0; i < PLL_TABLE_ROWS; i++)
	{
		if (freq <= cpm_table[i][0])
			return cpm_table[i][1];
	}

	return 0;
}

uint32_t get_cpm_freq(uint32_t pll)
{
	uint32_t i, freq = 0;

	pll = (pll & 0x9fffffff) | 1;
	for (i = 0; i < PLL_TABLE_ROWS; i++)
	{
		if (pll == cpm_table[i][1])
		{
			return cpm_table[i][0];
		}
	}

	return 0;
}

/* Sets g_pll_state.set_pll[miner_id][0][0..3] and applies it via
 * a3197s_pll_apply(), which broadcasts to the whole chain. */
void asic_apply_pll_freq(uint8_t miner_id, const uint32_t pll_freq[4])
{
	int k;

	for (k = 0; k < 4; k++)
		g_pll_state.set_pll[miner_id][0][k] = (uint16_t)pll_freq[k];

	a3197s_pll_apply(miner_id, 0);
}

/* Blocking PLL ramp: steps PLL0 from init_freq to target_freq in
 * PLL_RAMP_STEP_MHZ increments, once per second. Each step, PLL1-3 are
 * set to PLL0 + domain_interval*{1,2,3}. */
#define PLL_RAMP_STEP_MHZ 10

void asic_ramp_pll_freq(uint8_t miner_id, uint16_t init_freq, uint16_t target_freq, uint16_t domain_interval)
{
	int span = (int)target_freq - (int)init_freq;
	int direction = (span >= 0) ? 1 : -1;
	unsigned abs_span = (unsigned)((span >= 0) ? span : -span);
	unsigned m = abs_span / PLL_RAMP_STEP_MHZ;
	unsigned n = abs_span % PLL_RAMP_STEP_MHZ;
	unsigned step;
	uint32_t pll_freq[4];

	if (target_freq == init_freq)
		return;

	for (step = 0; step <= m; step++) {
		uint16_t freq_step = (uint16_t)(init_freq + step * PLL_RAMP_STEP_MHZ * direction);

		pll_freq[0] = freq_step;
		pll_freq[1] = freq_step + domain_interval;
		pll_freq[2] = freq_step + domain_interval * 2;
		pll_freq[3] = freq_step + domain_interval * 3;
		asic_apply_pll_freq(miner_id, pll_freq);
		delay_ms(1000);
	}

	if (n != 0) {
		pll_freq[0] = target_freq;
		pll_freq[1] = target_freq + domain_interval;
		pll_freq[2] = target_freq + domain_interval * 2;
		pll_freq[3] = target_freq + domain_interval * 3;
		asic_apply_pll_freq(miner_id, pll_freq);
		delay_ms(1000);
	}
}

void a3197s_pll_apply(uint8_t miner_id, uint16_t asic_id)
{
	uint32_t tmp32;
	uint32_t freq0, freq1, freq2, freq3;
	uint16_t chip_select_id;

	chip_select_id = asic_get_chip_select();

	freq0 = g_pll_state.set_pll[miner_id][0][0];
	freq1 = g_pll_state.set_pll[miner_id][0][1];
	freq2 = g_pll_state.set_pll[miner_id][0][2];
	freq3 = g_pll_state.set_pll[miner_id][0][3];

	a3197s_select_chip(miner_id, CHIP_ADDR_BROADCAST);

	/* SS_CTRL handshake bracketing this PLL step. */
	a3197s_smartspeed_set_ctrl(miner_id, SS_CTRL_RAMP_PRE);

	tmp32 = get_cpm_pll(freq1);
	a3197s_set_reg(miner_id, REG_PLL1_FREQ, tmp32);
	tmp32 = get_cpm_pll(freq2);
	a3197s_set_reg(miner_id, REG_PLL2_FREQ, tmp32);
	tmp32 = get_cpm_pll(freq3);
	a3197s_set_reg(miner_id, REG_PLL3_FREQ, tmp32);

	a3197s_smartspeed_set_ctrl(miner_id, SS_CTRL_RAMP_MID);

	tmp32 = get_cpm_pll(freq0);
	a3197s_set_reg(miner_id, REG_PLL0_FREQ, tmp32);

	a3197s_smartspeed_set_ctrl(miner_id, SS_CTRL_RAMP_COMMIT);

	tmp32 = ROLLTIME_TIMEOUT_CONST / freq3;
	a3197s_set_reg(miner_id, REG_ROLLTIME, tmp32);
	delay_ms(1);

	/* Read-only diagnostic: logs the LOCK/LOCK_IP bits inside the PLLxL1
	 * registers just written, read back from chip 0 as a representative
	 * sample. Not used to gate anything. */
	if (g_lock_diag_fp && g_lock_diag_count < PLL_LOCK_DIAG_MAX_LINES) {
		uint32_t pll0_raw, pll1_raw, pll2_raw, pll3_raw;

		a3197s_select_chip(miner_id, 0);
		pll0_raw = a3197s_get_reg(miner_id, REG_PLL0_FREQ);
		pll1_raw = a3197s_get_reg(miner_id, REG_PLL1_FREQ);
		pll2_raw = a3197s_get_reg(miner_id, REG_PLL2_FREQ);
		pll3_raw = a3197s_get_reg(miner_id, REG_PLL3_FREQ);

		fprintf(g_lock_diag_fp,
			"PLL_LOCK_DIAG chip0 pll0=0x%08x(lock=%d,lock_ip=%d) "
			"pll1=0x%08x(lock=%d,lock_ip=%d) pll2=0x%08x(lock=%d,lock_ip=%d) "
			"pll3=0x%08x(lock=%d,lock_ip=%d)\n",
			pll0_raw, !!(pll0_raw & PLL_LOCK_MASK), !!(pll0_raw & PLL_LOCK_IP_MASK),
			pll1_raw, !!(pll1_raw & PLL_LOCK_MASK), !!(pll1_raw & PLL_LOCK_IP_MASK),
			pll2_raw, !!(pll2_raw & PLL_LOCK_MASK), !!(pll2_raw & PLL_LOCK_IP_MASK),
			pll3_raw, !!(pll3_raw & PLL_LOCK_MASK), !!(pll3_raw & PLL_LOCK_IP_MASK));
		fflush(g_lock_diag_fp);
		g_lock_diag_count++;
	}

	a3197s_select_chip(miner_id, chip_select_id);
}

/* Periodic REG_ROLLTIME refresh, computed from freq3. Deliberately
 * separate from a3197s_pll_apply() so it doesn't also rewrite the
 * PLL0-3 dividers. */
void asic_refresh_rolltime(uint8_t miner_id, uint16_t freq3)
{
	uint16_t chip_select_id = asic_get_chip_select();
	uint32_t tmp32 = ROLLTIME_TIMEOUT_CONST / freq3;

	a3197s_select_chip(miner_id, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(miner_id, REG_ROLLTIME, tmp32);
	a3197s_select_chip(miner_id, chip_select_id);
}

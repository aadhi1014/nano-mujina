/*
 * A3197S telemetry: SmartSpeed pass/fail counters and the two derived
 * hashrate estimates (PLL-config nameplate via a3197s_calculate_hashrate(),
 * and SmartSpeed-pass-count-based via a3197s_calculate_smartspeed_error_rate()),
 * plus the per-chip PLL core-distribution/measured-frequency readback.
 */

#include <stdint.h>
#include "asic_engine.h"
#include "asic_internal.h"

static uint16_t g_get_pllcnt[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH][PLL_DOMAIN_COUNT];
static uint32_t g_ss_pass_count[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
static uint32_t g_ss_fail_count[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
static uint32_t g_estimated_hashrate_ghs = 0;
static uint32_t g_smartspeed_hashrate_ghs = 0;
static double g_spd_dh = 0;

static void set_ghsmm(uint32_t ghsmm)
{
	g_estimated_hashrate_ghs = ghsmm;
}

uint32_t get_ghsmm(void)
{
	return g_estimated_hashrate_ghs;
}

static void set_spd_dh(double spd_dh)
{
	g_spd_dh = spd_dh;
}

double get_spd_dh(void)
{
	return g_spd_dh;
}

static void set_ghsspd(uint32_t ghsspd)
{
	g_smartspeed_hashrate_ghs = ghsspd;
}

uint32_t get_ghsspd(void)
{
	return g_smartspeed_hashrate_ghs;
}

static void a3197s_calculate_hashrate(uint32_t *ghsmm)
{
	uint16_t i, j,k;
	*ghsmm = 0;
	for (i = 0; i < HBOARD_COUNT; i++)
	{
		for (j = 0; j < g_asic_count[i]; j++)
		{
			for (k = 0; k < PLL_DOMAIN_COUNT; k++)
			{
				*ghsmm += (g_get_pllcnt[i][j][k] * g_pll_state.get_pll[i][j][k] * 8);
			}
		}
	}
	*ghsmm /= 1000;
}

static double a3197s_calculate_smartspeed_error_rate(uint32_t *ghsspd)
{
	uint16_t i, j;
	double dh, spdlog_pass_sum, spdlog_fail_sum;

	spdlog_pass_sum = 0;
	spdlog_fail_sum = 0;

	for (i = 0; i < HBOARD_COUNT; i++)
	{
		for (j = 0; j < g_asic_count[i]; j++)
		{
			if (g_ss_pass_count[i][j])
			{
				spdlog_pass_sum += g_ss_pass_count[i][j];
				spdlog_fail_sum += g_ss_fail_count[i][j];
			}
		}
	}

	*ghsspd = spdlog_pass_sum * 0.256 / 1000;

	if (spdlog_pass_sum)
		dh = (spdlog_fail_sum / (spdlog_pass_sum + spdlog_fail_sum) * 100.0);
	else
		dh = 100.0;

	return dh;
}

/* Per-chip version of a3197s_calculate_smartspeed_error_rate(): same formula applied to one chip's
 * g_ss_pass_count[]/g_ss_fail_count[] entry instead of the chain sum. */
void asic_get_spd_chip_stat(uint8_t channel, uint16_t asic_id, float *ghsspd, float *spd_dh)
{
	uint32_t pass, fail;

	if (channel >= HBOARD_COUNT || asic_id >= MAX_ASIC_ON_SINGLE_HASH) {
		*ghsspd = 0.0f;
		*spd_dh = 100.0f;
		return;
	}

	pass = g_ss_pass_count[channel][asic_id];
	fail = g_ss_fail_count[channel][asic_id];

	*ghsspd = (float)(pass * 0.256 / 1000);
	*spd_dh = pass ? (float)((double)fail / (double)(pass + fail) * 100.0) : 100.0f;
}

void a3197s_read_smartspeed_log(uint8_t miner_id, uint16_t asic_id)
{
	uint32_t ghsmm, tmp, ghsspd;

	a3197s_select_chip(miner_id, asic_id);

	tmp = a3197s_get_reg(miner_id, REG_SS_LOG_TIMER);
	if (tmp == g_ss_timer_valid_value)
	{
		g_ss_pass_count[miner_id][asic_id] = a3197s_get_reg(miner_id, REG_SS_LOG_PASS);
		g_ss_fail_count[miner_id][asic_id] = a3197s_get_reg(miner_id, REG_SS_LOG_FAIL);

		if (asic_id == (g_asic_count[miner_id] - 1))
		{
			a3197s_calculate_hashrate(&ghsmm);
			set_ghsmm(ghsmm);
			set_spd_dh(a3197s_calculate_smartspeed_error_rate(&ghsspd));
			set_ghsspd(ghsspd);
		}
		/* Reconfigures noncemask for this specific chip (already
		 * selected above) each time its SmartSpeed window restarts. */
		asic_set_noncemask(miner_id, 0x00000001);
		a3197s_set_reg(miner_id, REG_SS_LOG_TIMER, g_ss_timer_default_value);
	}
}

void a3197s_read_smartspeed_counts(uint8_t miner_id, uint16_t asic_id)
{
	union asic_pll_core_dist pllcnt1;
	uint32_t tmp32;

	a3197s_select_chip(miner_id, asic_id);

	pllcnt1.raw = a3197s_get_reg(miner_id, REG_SS_CORE_DIST);

	g_get_pllcnt[miner_id][asic_id][0] = pllcnt1.pll0_cnt;
	g_get_pllcnt[miner_id][asic_id][1] = pllcnt1.pll1_cnt;
	g_get_pllcnt[miner_id][asic_id][2] = pllcnt1.pll2_cnt;
	g_get_pllcnt[miner_id][asic_id][3] = pllcnt1.pll3_cnt;

	tmp32 = a3197s_get_reg(miner_id, REG_PLL0_FREQ);
	g_pll_state.get_pll[miner_id][asic_id][0] = get_cpm_freq(tmp32);

	tmp32 = a3197s_get_reg(miner_id, REG_PLL1_FREQ);
	g_pll_state.get_pll[miner_id][asic_id][1] = get_cpm_freq(tmp32);

	tmp32 = a3197s_get_reg(miner_id, REG_PLL2_FREQ);
	g_pll_state.get_pll[miner_id][asic_id][2] = get_cpm_freq(tmp32);

	tmp32 = a3197s_get_reg(miner_id, REG_PLL3_FREQ);
	g_pll_state.get_pll[miner_id][asic_id][3] = get_cpm_freq(tmp32);
}

/* Getter for per-chip, per-PLL-domain SmartSpeed core distribution
 * (g_get_pllcnt) and each domain's measured frequency (g_pll_state.get_pll). */
void asic_get_pll_detail(uint8_t miner_id, uint16_t asic_id, uint16_t cnt_out[4], uint32_t freq_out[4])
{
	uint8_t k;

	for (k = 0; k < PLL_DOMAIN_COUNT; k++) {
		cnt_out[k] = g_get_pllcnt[miner_id][asic_id][k];
		freq_out[k] = g_pll_state.get_pll[miner_id][asic_id][k];
	}
}

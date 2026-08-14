/*
 * A3197S SmartSpeed: the SS_CTRL register handshake used to bracket
 * PLL steps (see asic_pll.c's a3197s_pll_apply()), the SmartSpeed
 * log-window timer constants, and the noncemask configuration sequence
 * issued right before the first job burst.
 */

#include <stdint.h>
#include "asic_engine.h"
#include "asic_internal.h"

uint32_t g_ss_timer_valid_value = SS_TIMER_VALID_VALUE;
uint32_t g_ss_timer_default_value = SS_TIMER_DEFAULT_VALUE;

/* SS_CTRL handshake bracketing a PLL step. SPD_SYNC_CFG (spd_syc
 * trigger) is rewritten after every SS_CTRL transition to commit the
 * change. */
void a3197s_smartspeed_set_ctrl(uint8_t miner_id, uint32_t ss_ctrl_value)
{
	a3197s_set_reg(miner_id, REG_SS_CTRL, ss_ctrl_value);
	a3197s_set_reg(miner_id, REG_SPD_SYNC_CFG, DEFAULT_SPD_SYNC_CFG);
}

static int asic_config_noncemask(uint8_t miner_id, uint32_t nonce_mask)
{
	a3197s_set_reg(miner_id, REG_NONCEMASK_CFG, 0x000075ff);
	a3197s_set_reg(miner_id, REG_NONCEMASK_WR, 0x11f);

	a3197s_set_reg(miner_id, REG_NONCEMASK_CFG, 0x000081ff);
	a3197s_set_reg(miner_id, REG_NONCEMASK_WR, nonce_mask);

	a3197s_set_reg(miner_id, REG_NONCEMASK_CFG, 0x000090ff);
	a3197s_set_reg(miner_id, REG_NONCEMASK_WR, 0xFF001001);

	a3197s_set_reg(miner_id, REG_NONCEMASK_CFG, 0x000075ff);
	a3197s_set_reg(miner_id, REG_NONCEMASK_WR, 0x1f);
	return 0;
}

/* Public wrapper: configures the noncemask, called right before a
 * job/work sequence rather than during early init. */
int asic_set_noncemask(uint8_t channel, uint32_t nonce_mask)
{
	return asic_config_noncemask(channel, nonce_mask);
}

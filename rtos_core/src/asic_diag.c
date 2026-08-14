/*
 * A3197S diagnostics: optional debug/worklog/PLL-lock log sinks and the
 * error/nonce-responsiveness counter getters exposed for the IPC
 * status message and troubleshooting.
 */

#include <stdint.h>
#include <stdio.h>
#include "asic_engine.h"
#include "asic_internal.h"

/* Optional log file for register-read failures (address + channel).
 * NULL unless asic_set_debug_log() is called. */
FILE *g_debug_fp = NULL;
void asic_set_debug_log(FILE *fp)
{
	g_debug_fp = fp;
}

/* Optional log file: logs the exact wire words a3197s_submit_job() sends
 * to the chip and the raw bytes a3197s_receive_nonces() decodes from a nonce
 * response, keyed by job_id/nonce2. NULL unless asic_set_worklog() is
 * called. */
FILE *g_worklog_fp = NULL;
void asic_set_worklog(FILE *fp)
{
	g_worklog_fp = fp;
}

/* Optional, bounded log for the PLL LOCK/LOCK_IP diagnostic in
 * a3197s_pll_apply(). Capped at PLL_LOCK_DIAG_MAX_LINES entries, then
 * stops. NULL unless asic_set_lock_diag_log() is called. */
FILE *g_lock_diag_fp = NULL;
uint32_t g_lock_diag_count = 0;
void asic_set_lock_diag_log(FILE *fp)
{
	g_lock_diag_fp = fp;
	g_lock_diag_count = 0;
}

uint32_t a3197s_get_ecc(uint8_t channel, uint8_t index)
{
	uint32_t tmp32 = 0;

	uint32_t value[7] = {0};

	a3197s_write_fifo(channel, REG_UART_ERR_BLOCK, NULL, 7, UART_READ_MODE);
	a3197s_read_fifo(channel, REG_UART_ERR_BLOCK, value, 7);
	tmp32 += (value[0] & 0xff) + ((value[0] >> 8) & 0xfff);
	tmp32 += value[4] + (value[5] & 0xffffff) + (value[6] & 0xffffff);

	return tmp32;
}

uint32_t a3197s_get_errcnt(uint8_t channel)
{
	return g_error_count[channel];
}

void asic_get_errcnt_detail(uint8_t channel, uint32_t *nonce_read_err,
	uint32_t *nonce_bad_len, uint32_t *nonce_overflow, uint32_t *regread_err)
{
	*nonce_read_err = g_nonce_read_err[channel];
	*nonce_bad_len  = g_nonce_bad_len[channel];
	*nonce_overflow = g_nonce_overflow[channel];
	*regread_err    = g_regread_err[channel];
}

/* Reports whether the chip is responding: nonce_timeout counts responses
 * with nothing new, nonce_heartbeat counts valid short heartbeat frames. */
void asic_get_nonce_responsiveness(uint8_t channel, uint32_t *nonce_timeout,
	uint32_t *nonce_heartbeat)
{
	*nonce_timeout   = g_nonce_timeout[channel];
	*nonce_heartbeat = g_nonce_heartbeat[channel];
}

/* Per-chip breakdown of asic_get_nonce_responsiveness(). */
void asic_get_nonce_chip_stat(uint8_t channel, uint16_t asic_id,
	uint32_t *nonce_timeout, uint32_t *nonce_heartbeat, uint32_t *nonce_data)
{
	*nonce_timeout   = g_nonce_timeout_chip[channel][asic_id];
	*nonce_heartbeat = g_nonce_heartbeat_chip[channel][asic_id];
	*nonce_data      = g_nonce_data_chip[channel][asic_id];
}

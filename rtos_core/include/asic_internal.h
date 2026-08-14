/* Private cross-file declarations shared only among the asic_*.c
 * translation units that together implement the A3197S ASIC engine
 * (formerly one file, asic_engine.c). NOT part of the public API --
 * do not include this from asic_engine.h, main.c, or pvt_sensor.c.
 *
 * This exists purely so the asic_engine.c -> asic_*.c split is pure
 * code motion: every global here kept its exact name, type, and the
 * exact file that already wrote/read it most; only its `static`
 * qualifier changed where a genuine cross-file caller required it.
 * No function body was rewritten to use these -- expressions like
 * `g_error_count[miner_id] = 0;` or `g_pll_state.get_pll[...]` are
 * unchanged from asic_engine.c, they just now resolve via extern
 * instead of a file-local static.
 */

#ifndef ASIC_INTERNAL_H
#define ASIC_INTERNAL_H

#include <stdint.h>
#include <stdio.h>
#include "asic_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- asic_regs.c: register-access primitives, promoted from static
 * so a3197s_enumerate.c/asic_init.c/asic_job.c/asic_nonce.c/asic_diag.c can
 * call them directly, exactly as asic_engine.c's other functions did
 * when they were all one translation unit. ---- */
int a3197s_write_fifo(uint8_t channel, uint32_t reg_addr, uint32_t *data_send, uint32_t len, uint8_t mode);
int a3197s_read_fifo(uint8_t channel, uint32_t reg_addr, uint32_t *data_recv, uint32_t len);
uint32_t a3197s_soft_reset(uint8_t channel);
uint16_t asic_get_chip_select(void);

/* g_error_count: incremented by a3197s_soft_reset() (asic_regs.c),
 * zeroed by a3197s_clear_ecc() (asic_regs.c) and a3197s_init_chain()
 * (asic_init.c), read by a3197s_get_errcnt() (asic_diag.c). Owning
 * definition lives in asic_regs.c. */
extern uint32_t g_error_count[HBOARD_COUNT];

/* g_selected_chip is NOT here -- it stays static, only asic_regs.c
 * (a3197s_select_chip writes it, a3197s_read_fifo reads it for logging)
 * ever touches it. */

/* ---- asic_nonce.c: per-channel and per-chip nonce/error counters,
 * written by a3197s_receive_nonces(), read by asic_diag.c's getters. ---- */
extern uint32_t g_nonce_read_err[HBOARD_COUNT];
extern uint32_t g_nonce_bad_len[HBOARD_COUNT];
extern uint32_t g_nonce_overflow[HBOARD_COUNT];
extern uint32_t g_nonce_timeout[HBOARD_COUNT];
extern uint32_t g_nonce_heartbeat[HBOARD_COUNT];
extern uint32_t g_nonce_timeout_chip[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
extern uint32_t g_nonce_heartbeat_chip[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
extern uint32_t g_nonce_data_chip[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];

/* g_regread_err: written by a3197s_read_fifo() (asic_regs.c), read by
 * asic_get_errcnt_detail() (asic_diag.c). Owning definition in
 * asic_regs.c, alongside a3197s_read_fifo(). */
extern uint32_t g_regread_err[HBOARD_COUNT];

/* ---- asic_job.c: pending-job vmask table, written by
 * a3197s_submit_job() and zeroed by a3197s_init_chain() (asic_init.c),
 * also read by a3197s_receive_nonces() (asic_nonce.c) as the vmask
 * table to match nonce responses against. ---- */
extern uint32_t g_work_vmask[HBOARD_COUNT][8];

/* ---- a3197s_enumerate.c: enumerated chip count, read by asic_telemetry.c
 * (a3197s_calculate_hashrate, a3197s_calculate_smartspeed_error_rate,
 * a3197s_read_smartspeed_log). ---- */
extern uint32_t g_asic_count[HBOARD_COUNT];

/* ---- asic_pll.c: measured/target PLL frequency state, seeded by
 * a3197s_init_chain() (asic_init.c), .get_pll[] written by
 * a3197s_read_smartspeed_counts() and read by a3197s_calculate_hashrate()/
 * asic_get_pll_detail() (all asic_telemetry.c). ---- */
extern a3197s_pll_state_t g_pll_state;
uint32_t get_cpm_freq(uint32_t pll);

/* ---- asic_smartspeed.c: SmartSpeed log-window timer constants, read
 * by a3197s_configure_regs() (asic_init.c, default value only) and
 * a3197s_read_smartspeed_log() (asic_telemetry.c, both values). Write-once at
 * static-init time; never reassigned. ---- */
extern uint32_t g_ss_timer_valid_value;
extern uint32_t g_ss_timer_default_value;

/* Writes REG_SS_CTRL then rewrites REG_SPD_SYNC_CFG to commit it -- the
 * exact 2-write pattern a3197s_pll_apply() (asic_pll.c) issues 3 times
 * (PRE/MID/COMMIT) around a PLL step. Factored out here, called from
 * asic_pll.c, so the bracketing sequence has one implementation
 * instead of being repeated inline 3 times as it was pre-split. */
void a3197s_smartspeed_set_ctrl(uint8_t miner_id, uint32_t ss_ctrl_value);

/* ---- asic_diag.c: optional debug/worklog/PLL-lock-diagnostic log
 * sinks, set only via asic_set_debug_log()/asic_set_worklog()/
 * asic_set_lock_diag_log() (asic_diag.c), read by the modules that
 * actually emit into them. ---- */
extern FILE *g_debug_fp;      /* read by a3197s_read_fifo() (asic_regs.c) */
extern FILE *g_worklog_fp;    /* read by a3197s_receive_nonces() (asic_nonce.c) and a3197s_submit_job()/asic_send_work() (asic_job.c) */
extern FILE *g_lock_diag_fp;  /* read by a3197s_pll_apply()'s LOCK diagnostic (asic_pll.c) */
extern uint32_t g_lock_diag_count;
#define PLL_LOCK_DIAG_MAX_LINES 40

#ifdef __cplusplus
}
#endif

#endif

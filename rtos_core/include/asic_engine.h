/* Register map, defaults, and wire-struct layout for asic_engine.c. */

#ifndef ASIC_ENGINE_H
#define ASIC_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include "chip_link.h"
#include "pll_cpm_table.h"
#include "miner_defs.h"
#include "a3197s_job.h"

#define ROLLTIME_TIMEOUT_CONST						335000000
#define NONCEMASK_CNT_PER_CHIP		        		1216
#define PLL_DOMAIN_COUNT							4
#define NONCE_BUF_FIFO_LEN					24
#define NONCE_RECORD_MAX               		8
#define CHIP_ADDR_BROADCAST					0x3ff
#define SS_TIMER_VALID_VALUE					(0x30000 + 3000)
#define SS_TIMER_DEFAULT_VALUE					(0x10000 + 3000)

#define DEFAULT_ROLLTIME					20000
#define DEFAULT_ASIC_CFG						0x800377ff
#define DEFAULT_CG_EN						0x00000000
#define DEFAULT_SPD_SYNC_CFG				0x80039700
/* SmartSpeed control register bring-up default, written once before the
 * first PLL step. */
#define DEFAULT_SS_CTRL					0xFFFF0421
/* SS_CTRL values written around each PLL step: PRE before the domain1-3
 * write, MID before the domain0 write, COMMIT after the domain0 write
 * (left as the steady-state value once ramping stops). */
#define SS_CTRL_RAMP_PRE					0x00001429 /* before domain1-3 write */
#define SS_CTRL_RAMP_MID					0x0000142A /* before domain0 write */
#define SS_CTRL_RAMP_COMMIT				0x00001C2A /* after domain0 write -- final/steady-state value */
#define DEFAULT_PLL_TABLE_SEL					0x00004688
#define DEFAULT_PLL_COMMON_CFG				0x005803ed
#define DEFAULT_PLL0_FREQ					0x90084302
#define DEFAULT_PLL1_FREQ					0x90084302
#define DEFAULT_PLL2_FREQ					0x90084302
#define DEFAULT_PLL3_FREQ					0x90084302
#define DEFAULT_PLL_COMMIT						0x108c000f
#define DEFAULT_DIVCNT_CTRL				0x00000001
#define DEFAULT_PVT_SELECT					0x00016380
#define DEFAULT_UART_CFG1					0x0012525a
#define DEFAULT_UART_CFG3					0x00522001

#define REG_VMASK_TABLE						0x00
#define REG_WORK							0x20
#define REG_NONCE_BUF						0xa0
#define REG_NONCEMASK_CFG						0x100
#define REG_NONCEMASK_WR						0x104
#define REG_SPD_MUX						0x140
#define REG_NONCE_UPDATE					0x200
#define REG_ASIC_CFG							0x204
#define REG_TARGET_LO						0x208
#define REG_TARGET_HI						0x20c
#define REG_ROLLTIME						0x214
/* Tbase register, written once to 0 during init. */
#define REG_TBASE						0x210
#define REG_CG_EN						0x218
#define REG_SPD_SYNC_CFG					0x21c
/* Unnamed registers, written once during init right after SPD_SYNC_CFG. */
#define REG_UNK_220						0x220
#define REG_UNK_22C						0x22c
#define REG_UNK_234						0x234
#define REG_CHKDROP						0x238
#define REG_NONCE_CNT_MODE					0x23c
/* Unnamed register, address only (not written). */
#define REG_UNK_244						0x244
#define REG_SS_CTRL						0x400
#define REG_SS_THRESH0						0x404
#define REG_SS_THRESH1						0x408
#define REG_SS_THRESH2						0x40c
#define REG_SS_THRESH3						0x410
#define REG_SS_THRESH4						0x414
#define REG_PLL_TABLE_SEL						0x418
#define REG_SS_CORE_DIST						0x420
#define REG_SS_LOG_TIMER					0x428
#define REG_SS_LOG_PASS				0x42c
#define REG_SS_LOG_FAIL				0x430
/* Reserved/unnamed registers, written once during init. */
#define REG_RFU1							0x438
#define REG_UNK_448						0x448
#define REG_UNK_450						0x450
#define REG_UNK_490						0x490
#define REG_UNK_498						0x498
#define REG_RFU2							0x4b0
#define REG_UNK_4B4						0x4b4
#define REG_UNK_4C0						0x4c0
/* Unnamed registers, written once during init between UNK_4C0 and RFU4. */
#define REG_UNK_4C4						0x4c4
#define REG_UNK_4CC						0x4cc
#define REG_RFU4							0x4e4
#define REG_RFU5							0x4e8
#define REG_RFU6							0x4ec
#define REG_RFU7							0x4f0
#define REG_SS_LOG_RESET					0x444
#define REG_PLL_COMMON_CFG					0x3000
#define REG_PLL0_FREQ						0x3004
#define REG_PLL1_FREQ						0x300c
#define REG_PLL2_FREQ						0x3014
#define REG_PLL3_FREQ						0x301c
#define REG_PLL_COMMIT							0x303c
/* LOCK/LOCK_IP status bits within each PLLxL1 register (0x3004/0x300c/
 * 0x3014/0x301c). LOCK_IP (bit 30) is the raw PLL lock signal; LOCK
 * (bit 29) is the filtered signal, gated by LOCK_EN (bit 28). */
#define PLL_LOCK_IP_MASK					0x40000000u
#define PLL_LOCK_MASK						0x20000000u
#define PLL_LOCK_EN_MASK					0x10000000u
#define PLL_LOCK_BOTH_MASK				0x60000000u
/* PLL control registers, written 0x00000000 once during init. */
#define REG_PLL_CTRL01					0x3034
#define REG_PLL_CTRL23					0x3038
#define REG_DIVCNT_CTRL					0x3060
#define REG_EFUSE_CTRL					0x3400
#define REG_EFUSE_FIFO					0x3480
#define REG_PVT_SELECT						0x3700
#define REG_PVT_RESULT						0x3704
#define REG_UART_CFG1					0x3c84
#define REG_UART_CFG3					0x3c8c
/* Per-chip status: chip ID, chip-last flag, and UART frequency-ratio,
 * read during enumeration to confirm each chip's assigned address. */
#define REG_UART_STATUS					0x3ca0
#define REG_UART_ERR_BLOCK					0x3ca4
#define REG_RI_WRITE_EN						0x3f00
#define REG_RI_ENABLE					0x3f04
/* Core soft-reset register, write-one-triggered (W1T). */
#define REG_CORE_RST						0x3f08

struct asic_work_frame {
	volatile uint32_t init;
	volatile uint32_t data[3];
	volatile uint32_t work[15];
	volatile uint32_t note[4];
};

union asic_pll_core_dist {
	struct {
		volatile uint32_t pll0_cnt : 8;
		volatile uint32_t pll1_cnt : 8;
		volatile uint32_t pll2_cnt : 8;
		volatile uint32_t pll3_cnt : 8;
	};
	volatile uint32_t raw;
};

union asic_efuse_ctrl {
	struct {
		volatile uint32_t sense : 1;
		volatile uint32_t start_prog : 1;
		volatile uint32_t setn_cycle : 1;
		volatile uint32_t efuse_rst : 1;
		volatile uint32_t burn_off : 1;
		volatile uint32_t sense_rdy : 1;
		volatile uint32_t reset_en : 1;
		volatile uint32_t rfu : 25;
	};
	volatile uint32_t raw;
};

struct asic_nonce_record {
	volatile uint32_t job_id : 32;
	volatile uint32_t nonce2 : 32;
	volatile uint32_t nonce : 32;
	volatile uint32_t asic_id : 10;
	volatile uint32_t miner_id : 6;
	volatile uint32_t ntime : 8;
	volatile uint32_t mid_id : 4;
	volatile uint32_t valid : 4;
};


void asic_init(uint8_t channel);
void a3197s_clear_ecc(uint8_t channel, uint8_t index);
void a3197s_efuse_read(uint8_t channel, uint32_t *data);
void a3197s_read_smartspeed_log(uint8_t miner_id, uint16_t asic_id);
void a3197s_read_smartspeed_counts(uint8_t miner_id, uint16_t asic_id);
void asic_get_pll_detail(uint8_t miner_id, uint16_t asic_id, uint16_t cnt_out[4], uint32_t freq_out[4]);
void a3197s_set_uart_baud(uint8_t channel, uint32_t baud);
void a3197s_select_chip(uint8_t channel, uint16_t chip_addr);
void a3197s_submit_job(uint8_t miner_id, uint16_t asic_id, uint8_t block);
void a3197s_pll_apply(uint8_t miner_id, uint16_t asic_id);
/* Refreshes REG_ROLLTIME only, without touching PLL0-3. */
void asic_refresh_rolltime(uint8_t miner_id, uint16_t freq3);
/* Writes the given per-domain PLL frequency values. */
void asic_apply_pll_freq(uint8_t miner_id, const uint32_t pll_freq[4]);
/* Blocking PLL ramp: steps domain 0 from init_freq to target_freq in
 * PLL_RAMP_STEP_MHZ increments, one real second apart. Each step,
 * domains 1-3 are set to domain0 + domain_interval*{1,2,3}. Returns
 * once domain0 has reached target_freq exactly. */
void asic_ramp_pll_freq(uint8_t miner_id, uint16_t init_freq, uint16_t target_freq, uint16_t domain_interval);
char *a3197s_get_chipid(uint8_t channel);
int a3197s_init_chain(uint8_t miner_id);
int a3197s_set_reg(uint8_t channel, uint32_t addr, uint32_t value);
int asic_get_nonce(uint8_t channel, uint16_t asic_id,struct asic_nonce_record *nonce, uint8_t *nonce_count);
/* Sets the pending work buffer used by a3197s_submit_job(). Copies by value. */
void a3197s_set_job(const a3197s_job_t *job);
/* Writes an array of words to consecutive register addresses starting at addr. */
void asic_raw_write_words(uint8_t channel, uint32_t addr, const uint32_t *words, uint32_t count);
uint32_t a3197s_enumerate(uint8_t channel);
/* Per-chip enumeration verification: selects each chip, writes and
 * reads back a verify pattern, then a final broadcast write. Diagnostic only. */
int asic_enum_verify(uint8_t channel, uint32_t chip_count);
/* Reads REG_UART_STATUS on the last enumerated chip and checks its
 * reported address against chip_count-1. Returns 0 on match, nonzero
 * otherwise (including on a transport read failure). Diagnostic only. */
int asic_enum_verify_lastchip(uint8_t channel, uint32_t chip_count,
	uint32_t *out_chipid, uint32_t *out_chiplast, uint32_t *out_freq_ratio_raw);
/* Same check as asic_enum_verify_lastchip(), applied to every chip.
 * out_ret[chip]: 0=success, 1=failure (frame started but invalid),
 * 2=no response. out_chipid[chip] only meaningful when out_ret[chip]==0.
 * Both arrays must be sized >= chip_count by the caller. Diagnostic only. */
void asic_enum_verify_alladdr(uint8_t channel, uint32_t chip_count,
	int *out_ret, uint32_t *out_chipid);
uint32_t a3197s_get_errcnt(uint8_t channel);
/* Breaks a3197s_get_errcnt()'s single counter into its component sources.
 * Diagnostic only, not sent over the IPC_MSG_STATUS wire. */
void asic_get_errcnt_detail(uint8_t channel, uint32_t *nonce_read_err,
	uint32_t *nonce_bad_len, uint32_t *nonce_overflow, uint32_t *regread_err);
/* Reports whether the chip is responding: nonce timeout and heartbeat counters. */
void asic_get_nonce_responsiveness(uint8_t channel, uint32_t *nonce_timeout,
	uint32_t *nonce_heartbeat);
/* Per-chip breakdown of asic_get_nonce_responsiveness(). */
void asic_get_nonce_chip_stat(uint8_t channel, uint16_t asic_id,
	uint32_t *nonce_timeout, uint32_t *nonce_heartbeat, uint32_t *nonce_data);
/* Per-chip breakdown of get_ghsspd()/get_spd_dh(), computed from this
 * chip's g_ss_pass_count[]/g_ss_fail_count[] entry. */
void asic_get_spd_chip_stat(uint8_t channel, uint16_t asic_id,
	float *ghsspd, float *spd_dh);
/* Logs "regread_fail channel=.. chip=.. addr=.. len=.." to fp for every
 * a3197s_read_fifo() failure. Pass NULL to disable (default). */
void asic_set_debug_log(FILE *fp);
/* Logs a3197s_submit_job()'s per-job WORK burst (header bytes, vmask table,
 * wire words sent) and each received nonce's raw wire bytes, keyed by
 * job_id/nonce2. Pass NULL to disable (default). */
void asic_set_worklog(FILE *fp);
void asic_set_lock_diag_log(FILE *fp);
uint32_t a3197s_get_ecc(uint8_t channel, uint8_t index);
uint32_t a3197s_get_reg(uint8_t channel, uint32_t addr);
int a3197s_get_reg_ex(uint8_t channel, uint32_t addr, uint32_t *out);
int asic_set_noncemask(uint8_t channel, uint32_t nonce_mask);
uint32_t get_ghsmm(void);
uint32_t get_ghsspd(void);
double get_spd_dh(void);

#ifdef __cplusplus
}
#endif

#endif

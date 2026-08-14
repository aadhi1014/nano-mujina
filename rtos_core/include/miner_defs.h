#ifndef MINER_DEFS_H
#define MINER_DEFS_H

#include <stddef.h>
#include <stdint.h>

#define HBOARD_COUNT 4
#define MAX_ASIC_ON_SINGLE_HASH 1024
#define AVA_P_COINBASE_SIZE (6 * 1024 + 64)
#define AVA_P_MERKLES_COUNT 30

typedef struct ss_config_t {
	uint8_t en;
	uint8_t p_sel;
	uint8_t ssdn;
	uint8_t clk_sel;
	uint32_t low;
	uint32_t high;
	uint32_t th0;
	uint32_t th1;
	uint32_t th2;
	uint32_t th3;
	uint32_t th4;
} ss_config;

typedef struct asic_base_t {
	uint8_t mask;
	uint8_t ncheck;
	uint8_t roll_en;
	uint16_t p_int;
	ss_config ss[HBOARD_COUNT];
} asic_base_setting;

typedef struct a3197s_pll_state {
	uint16_t set_pll[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH][4];
	uint16_t get_pll[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH][4];
} a3197s_pll_state_t;

/* The job type (a3197s_job_t) and bitcoin_build_nonce2_job()'s
 * declaration live in a3197s_job.h, not here -- this header only holds
 * ASIC-config-shaped types (SmartSpeed thresholds, PLL state) plus the
 * shared size constants above. */

#endif

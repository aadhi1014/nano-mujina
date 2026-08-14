#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#include <stdint.h>

#ifndef AVA_P_COINBASE_SIZE
#define AVA_P_COINBASE_SIZE (6 * 1024 + 64)
#endif

#ifndef AVA_P_MERKLES_COUNT
#define AVA_P_MERKLES_COUNT 30
#endif

struct ipc_msg_hdr {
	uint16_t type;
	uint16_t len;
};

enum {
	IPC_MSG_JOB          = 1,
	IPC_MSG_NONCE        = 2,
	IPC_MSG_STATUS       = 3,
	IPC_MSG_SET_MODE     = 4,
	IPC_MSG_HELLO        = 5,
	/* rtos -> mujina: request a core voltage change. mujina applies it
	 * over I2C and replies with IPC_MSG_STATUS carrying the voltage it
	 * actually set. */
	IPC_MSG_SET_VOLTAGE = 6,
	/* mujina -> rtos: user-requested pause/resume. PAUSE asserts RST and
	 * idles the chain without tearing down the IPC connection or exiting
	 * the process. RESUME re-runs the chain bring-up sequence and
	 * continues mining. Fire-and-forget, no payload -- rtos_core's next
	 * STATUS carries the resulting `paused` state back. */
	IPC_MSG_PAUSE  = 7,
	IPC_MSG_RESUME = 8,
	/* Diagnostic: mujina/nonce_probe -> rtos, overrides
	 * REG_ROLLTIME with a raw value, bypassing the normal
	 * timeout/freq-derived computation. Fire-and-forget, no reply. */
	IPC_MSG_SET_ROLLTIME_RAW = 9,
	/* Diagnostic: mujina/probe -> rtos, requests rtos issue a fresh
	 * IPC_MSG_SET_VOLTAGE sync round-trip to mujina with the given
	 * target_mv. */
	IPC_MSG_SET_VOLTAGE_RAW = 10,
	/* Diagnostic: mujina/probe -> rtos, forces a fresh SmartSpeed
	 * pass/fail sampling window: broadcast writes
	 * REG_SS_LOG_RESET=1 (resets counters) then
	 * REG_SS_LOG_TIMER=SS_TIMER_DEFAULT_VALUE (re-arms the window
	 * timer). Fire-and-forget, 1-byte dummy payload. */
	IPC_MSG_RESET_SPDLOG_RAW = 11,
};

struct ipc_job {
	uint32_t job_id;
	uint32_t nonce2_start;
	int32_t  nonce2_offset;
	int32_t  nonce2_size;
	int32_t  merkle_offset;
	int32_t  nmerkles;
	uint8_t  merkles[AVA_P_MERKLES_COUNT][32];
	uint8_t  coinbase[AVA_P_COINBASE_SIZE];
	uint32_t coinbase_len;
	uint8_t  header[128];
	uint8_t  target[32];
	uint8_t  work_restart;
	/* AsicBoost version-rolling mask table, mirrors a3197s_job_t's
	 * vmask[8] (a3197s_job.h). */
	uint32_t vmask[8];
};

struct ipc_nonce {
	uint32_t job_id;
	uint32_t nonce2;
	uint32_t nonce;
	uint16_t asic_id;
	uint8_t  miner_id;
	uint8_t  ntime;
	uint8_t  mid_id;
};

/* Per-chip status breakdown carried in struct ipc_status. */
#define IPC_STATUS_MAX_CHIPS 12

struct ipc_chip_status {
	float    temp_c;
	float    volt_mv;
	uint8_t  pll_cnt[4];      /* engine count per domain (0-152 each) */
	uint16_t pll_freq[4];     /* measured freq per domain, MHz */
	uint32_t nonce_timeout;
	uint32_t nonce_heartbeat;
	uint32_t nonce_data;
	/* Per-chip SmartSpeed pass-count-based hashrate/fail-rate, same
	 * formula as asic_engine.c's a3197s_calculate_smartspeed_error_rate()
	 * applied to this chip's g_ss_pass_count[]/g_ss_fail_count[] entry
	 * instead of the chain sum. */
	float    ghsspd;          /* measured hashrate, GH/s */
	float    spd_dh;          /* SmartSpeed fail rate, % */
};

struct ipc_status {
	uint32_t asics_total;
	uint32_t ghsmm;
	/* SmartSpeed pass-count-based measured hashrate and internal fail
	 * rate -- see asic_engine.c's
	 * a3197s_calculate_smartspeed_error_rate()/get_ghsspd()/get_spd_dh().
	 * ghsmm above is the theoretical PLL-config nameplate value. */
	uint32_t ghsspd;
	float    spd_dh;
	float    temp_avg;
	float    temp_max;
	uint32_t pll_freq[4];
	uint32_t err_crc;
	/* Last core voltage mujina actually applied (commanded value; the
	 * DC/DC regulator is write-only, so this is not read back). */
	uint32_t voltage_mv;
	/* 1 = chain currently RST-asserted/idle (IPC_MSG_PAUSE was applied
	 * and no IPC_MSG_RESUME since), 0 = mining normally. */
	uint8_t  paused;
	/* Chain-wide error breakdown -- see
	 * asic_get_errcnt_detail()/asic_get_nonce_responsiveness().
	 * regread_err is chain-wide only (tracked per UART channel, not per
	 * chip); nonce_timeout/heartbeat/data are per-chip (see
	 * ipc_chip_status above). */
	uint32_t nonce_read_err;
	uint32_t nonce_bad_len;
	uint32_t nonce_overflow;
	uint32_t regread_err;
	uint8_t  chip_count;
	struct ipc_chip_status chips[IPC_STATUS_MAX_CHIPS];
};

struct ipc_set_mode {
	uint32_t pll_freq[4];
	uint8_t  work_mode;
};

/* IPC_MSG_SET_ROLLTIME_RAW payload -- see that enum value's comment. */
struct ipc_set_rolltime_raw {
	uint32_t value;
};

/* IPC_MSG_SET_VOLTAGE_RAW payload -- see that enum value's comment. */
struct ipc_set_voltage_raw {
	int32_t target_mv;
};

/* IPC_MSG_SET_VOLTAGE payload. mujina does the mV -> I2C-code conversion,
 * so rtos_core only ever deals in millivolts. */
struct ipc_set_voltage {
	int32_t target_mv;
};

/*
 * DC/DC core-voltage regulator wire format (I2C-3, addr 0x48). Every
 * write is exactly 3 bytes:
 *   [ (0x48<<1)|0 ,  0xF8 ,  code ]
 * where code's bit7 is the sign (1 = below the 3600mV base, 0 = above) and
 * bits[6:0] are a magnitude in 26mV steps:
 *   mV = 3600 - magnitude*26   if bit7 set
 *   mV = 3600 + magnitude*26   if bit7 clear
 */
#define IPC_DCDC_I2C_BUS      3
#define IPC_DCDC_I2C_ADDR     0x48
#define IPC_DCDC_VOLT_REG     0xF8
#define IPC_DCDC_BASE_MV      3600
#define IPC_DCDC_STEP_MV      26

/*
 * struct ipc_job is sized for the worst case (AVA_P_COINBASE_SIZE=6208B,
 * AVA_P_MERKLES_COUNT=30 -> ~7.3KB total), but K_IPCMSG_MAX_CONTENT_LEN
 * (sdk/k_comm_ipcmsg.h) is 1024B. ipc_job_pack()/ipc_job_unpack() send
 * only the actual coinbase_len/nmerkles data over the wire, not the full
 * fixed-size buffers. IPC_JOB_WIRE_HDR_LEN is the fixed portion (all
 * scalar fields + header[128] + target[32] + work_restart); merkles/
 * coinbase ride after it. No chunking/fragmentation: a job whose packed
 * size exceeds K_IPCMSG_MAX_CONTENT_LEN fails (ipc_job_pack() returns -1)
 * rather than truncating.
 */
#define IPC_JOB_WIRE_HDR_LEN (4u*7u + 128u + 32u + 1u + 4u*8u) /* = 221 */

/* Packs job into buf (caller-provided, capacity buf_cap). On success,
 * *out_len is the packed length (<= buf_cap) and returns 0. Returns -1 if
 * job->nmerkles/coinbase_len are out of range or the packed result would
 * exceed buf_cap -- most likely because the payload is too large for one
 * IPCM message (see above); this function does not fragment. */
int ipc_job_pack(const struct ipc_job *job, uint8_t *buf, uint16_t buf_cap, uint16_t *out_len);

/* Unpacks a wire-format job (as produced by ipc_job_pack()) from buf/len
 * into *job (zeroes *job first). Returns 0 on success, -1 on a malformed
 * or truncated buffer, including a length that doesn't exactly match the
 * declared nmerkles/coinbase_len. */
int ipc_job_unpack(const uint8_t *buf, uint16_t len, struct ipc_job *job);

#endif

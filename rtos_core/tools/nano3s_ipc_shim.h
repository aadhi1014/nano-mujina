#ifndef NANO3S_IPC_SHIM_H
#define NANO3S_IPC_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors struct ipc_nonce (ipc_protocol.h) field-for-field. */
typedef struct {
	uint32_t job_id;
	uint32_t nonce2;
	uint32_t nonce;
	uint16_t asic_id;
	uint8_t  miner_id;
	uint8_t  ntime;
	uint8_t  mid_id;
} nano3s_nonce_t;

#define NANO3S_STATUS_MAX_CHIPS 12

/* Mirrors struct ipc_chip_status (ipc_protocol.h) field-for-field. */
typedef struct {
	float    temp_c;
	float    volt_mv;
	uint8_t  pll_cnt[4];
	uint16_t pll_freq[4];
	uint32_t nonce_timeout;
	uint32_t nonce_heartbeat;
	uint32_t nonce_data;
	float    ghsspd;
	float    spd_dh;
} nano3s_chip_status_t;

/* Mirrors struct ipc_status (ipc_protocol.h) field-for-field, including
 * per-chip stats and chain-wide error counters. */
typedef struct {
	uint32_t asics_total;
	uint32_t ghsmm;
	uint32_t ghsspd;
	float    spd_dh;
	float    temp_avg;
	float    temp_max;
	uint32_t pll_freq[4];
	uint32_t err_crc;
	uint32_t voltage_mv;
	uint8_t  paused;
	uint32_t nonce_read_err;
	uint32_t nonce_bad_len;
	uint32_t nonce_overflow;
	uint32_t regread_err;
	uint8_t  chip_count;
	nano3s_chip_status_t chips[NANO3S_STATUS_MAX_CHIPS];
} nano3s_status_t;

/*
 * Connects to rtos_core.elf over IPCM (blocking call) and starts the
 * background dispatch thread. Must be called exactly once, before any
 * other nano3s_ipc_* call.
 *
 * IPC_MSG_HELLO and IPC_MSG_SET_VOLTAGE are handled transparently inside
 * the shim (ack HELLO with an empty reply; SET_VOLTAGE shells out to
 * i2cset and replies with the applied mV) -- the caller never sees these
 * message types.
 *
 * Returns 0 on success, -1 on failure. Only one process may hold the
 * mujina_svc/port 300 registration at a time.
 */
int nano3s_ipc_open(void);

/*
 * Sends one job to rtos_core.elf (fire-and-forget, IPC_MSG_JOB). All
 * buffers are copied internally (packed via ipc_job_pack()) -- caller may
 * free/reuse its buffers immediately after this returns.
 *
 * merkles is nmerkles rows of 32 bytes each, row-major (same layout as
 * struct ipc_job.merkles[AVA_P_MERKLES_COUNT][32]). vmask is always
 * exactly 8 uint32_t (AsicBoost version-rolling mask table).
 *
 * Returns 0 on success, -1 on failure (including nmerkles/coinbase_len
 * out of range for the wire format).
 */
int nano3s_ipc_send_job(
	uint32_t job_id,
	uint32_t nonce2_start,
	int32_t nonce2_offset,
	int32_t nonce2_size,
	const uint8_t *coinbase, uint32_t coinbase_len,
	int32_t merkle_offset,
	const uint8_t *merkles, int32_t nmerkles,
	const uint8_t header[128],
	const uint8_t target[32],
	uint8_t work_restart,
	const uint32_t vmask[8]);

/*
 * Dequeues one nonce report if available (non-blocking). Returns 1 and
 * fills *out if a nonce was waiting, 0 if the queue is empty. Nonces are
 * queued in arrival order; once the queue reaches NANO3S_NONCE_QUEUE_CAP,
 * the oldest entry is dropped and a warning is logged to stderr.
 */
int nano3s_ipc_poll_nonce(nano3s_nonce_t *out);

/*
 * Copies the most recently received IPC_MSG_STATUS into *out. Returns 1
 * if at least one status has ever been received, 0 if none yet.
 * rtos_core.elf sends these periodically.
 */
int nano3s_ipc_get_status(nano3s_status_t *out);

/*
 * Fire-and-forget pause/resume (IPC_MSG_PAUSE/IPC_MSG_RESUME). PAUSE
 * asserts RST on the RTOS side only; cutting power/fan is handled
 * elsewhere. Both send a 1-byte dummy payload rather than a zero-length
 * body, since a zero-length body fails to send.
 *
 * Returns 0 on send success, -1 on failure.
 */
int nano3s_ipc_pause(void);
int nano3s_ipc_resume(void);

/*
 * Sends IPC_MSG_SET_MODE (mujina -> rtos, PLL frequency ramp target).
 * pll_freq[4] mirrors struct ipc_set_mode field-for-field: rtos_core's
 * handler ramps toward pll_freq[0] using interval=pll_freq[1]-pll_freq[0]
 * as the per-domain step -- this is not four independent per-domain
 * targets, it describes one ramp as [target, target+interval, ...].
 * Returns 0 on send success, -1 on failure.
 */
int nano3s_ipc_set_mode(const uint32_t pll_freq[4], uint8_t work_mode);

/*
 * Directly overrides REG_ROLLTIME with a raw value on the RTOS
 * side, bypassing the normal frequency-derived computation and
 * suppressing its periodic refresh while active. A SET_MODE call
 * restores normal behavior. Fire-and-forget, returns 0 on send success,
 * -1 on failure.
 */
int nano3s_ipc_set_rolltime_raw(uint32_t value);

/*
 * Requests rtos_core issue a fresh IPC_MSG_SET_VOLTAGE sync round-trip
 * with the given target_mv, re-triggerable at runtime without a reboot.
 * Fire-and-forget from this shim's perspective (the SET_VOLTAGE
 * round-trip happens rtos-side); returns 0 on send success, -1 on
 * failure.
 */
int nano3s_ipc_set_voltage_raw(int32_t target_mv);

/*
 * Forces a fresh SmartSpeed pass/fail sampling window
 * (REG_SS_LOG_RESET=1 + re-arm REG_SS_LOG_TIMER),
 * broadcast to all chips. Returns 0 on send success, -1 on failure.
 */
int nano3s_ipc_reset_spdlog_raw(void);

/*
 * Disconnects and cleans up. Safe to call even if nano3s_ipc_open()
 * failed or was never called.
 */
void nano3s_ipc_close(void);

#ifdef __cplusplus
}
#endif

#endif

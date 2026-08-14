/*
 * a3197s_job_t: the RTOS-core-internal job representation. This is the
 * type the A3197S ASIC driver (asic_job.c, main.c's Stage 6 loop) works
 * with -- it has no conceptual dependency on Mujina or any other
 * upstream client.
 *
 * Desired architecture:
 *
 *   Mujina (or any other upstream: LuxOS, Braiins, VNish, a custom
 *   Stratum client)
 *      |
 *   <client>-specific adapter (translates the client's job shape into
 *      a3197s_job_t -- see ipc_job_adapter.c for the Mujina/IPC case)
 *      |
 *   generic IPC protocol (ipc_protocol.c/.h: struct ipc_job on the wire)
 *      |
 *   A3197S RTOS driver (this codebase)
 *      |
 *   A3197S ASICs
 *
 * Field set is unchanged from the pre-refactor struct mm_work -- this
 * type relocates the job representation conceptually, it does not
 * redesign it. bitcoin_build_nonce2_job() (src/miner.c) needs every
 * field here to do the Stratum merkle-branch math.
 */

#ifndef A3197S_JOB_H
#define A3197S_JOB_H

#include <stddef.h>
#include <stdint.h>
#include "miner_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct a3197s_job {
	uint32_t job_id;
	size_t coinbase_len;
	uint8_t coinbase[AVA_P_COINBASE_SIZE];
	uint32_t nonce2;
	int nonce2_offset;
	int nonce2_size;
	int merkle_offset;
	int nmerkles;
	uint8_t merkles[AVA_P_MERKLES_COUNT][32];
	uint8_t header[128];
	uint8_t target[32];
	uint32_t vmask[8];
	uint8_t work_restart;
} a3197s_job_t;

/* Implementation in src/miner.c -- computes the nonce2-dependent merkle
 * root (Stratum merkle-branch algorithm) and writes it into
 * job->header at job->merkle_offset. */
void bitcoin_build_nonce2_job(a3197s_job_t *job, uint32_t nonce2);

#ifdef __cplusplus
}
#endif

#endif

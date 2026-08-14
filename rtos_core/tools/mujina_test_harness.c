/*
 * Throwaway Linux-side counterpart for rtos_core's Stage 5/6 (ipc_mujina
 * bring-up). NOT real mujina -- see rtos_core/docs/DESIGN.md, which
 * explicitly calls for a disposable harness here, not the real thing.
 * Cross-compiled for the device's actual glibc userspace (unlike
 * rtos_core itself, which targets RT-Smart musl) -- see BUILD_HARNESS.md.
 *
 * Uses the K230 SDK's real kd_ipcmsg_* API (../vendor/sdk_libs/
 * libipcmsg.a, headers in ../include/sdk/) rather than hand-rolled
 * /dev/ipcm_user ioctls -- see docs/BUILD_NOTES.md's Stage 5 section for
 * why the raw-ioctl approach could open a channel but never actually
 * exchange a real message.
 *
 * Stage 5 role: "server"/responder, matching the K230 SDK's own
 * sample_receiver.c -- registers a service, connects (blocking), answers
 * rtos_core's kd_ipcmsg_send_sync HELLO/SET_VOLTAGE requests from its recv
 * callback via kd_ipcmsg_create_resp_message + kd_ipcmsg_send_async.
 *
 * Stage 6 role (2026-07-24, additive): sends one small dummy IPC_MSG_JOB
 * shortly after connecting (fire-and-forget, matching how a real mujina
 * would push a stratum job), logs every IPC_MSG_NONCE rtos_core finds, and
 * on the first IPC_MSG_STATUS received, sends one IPC_MSG_SET_MODE back --
 * exercises all six message types in ipc_mujina.h in one run.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sdk/k_ipcmsg.h"
#include "ipc_mujina.h"

/* Stage 6 (2026-07-24): struct ipc_job is large/complex enough (variable
 * merkles/coinbase, wire-packed -- see ipc_mujina.h) that duplicating it
 * here the way the small Stage 5 structs used to be duplicated would be
 * error-prone. Includes the canonical header instead (needs ipc_mujina.c
 * compiled in too -- see BUILD_HARNESS.md) and drops the local IPC_MSG_*
 * enum and ipc_status/ipc_set_voltage copies in favor of the real ones. */

/* Must match rtos_core/src/main.c's STAGE5_SERVICE_NAME/STAGE5_PORT.
 * CHANGED (2026-07-26): was 0xc9 (201, mm_miner's own port) -- switched to a
 * distinct port so mm_miner can stay completely alive during testing (own
 * watchdog petting, no reboot risk) with zero chance of it ever connecting
 * to this harness's registration. See main.c's STAGE5_PORT comment for the
 * full rationale, including why the old "port 300 caused an EFAULT" belief
 * was wrong and already corrected in docs/BUILD_NOTES.md. */
#define SERVICE_NAME "mujina_svc"
#define PORT 300
/* Fixed two-node topology: rtos_core (RT-Smart/big-core) = node 0, this
 * harness (Linux/little-core) = node 1 -- matches stock's own convention,
 * which never queries this dynamically. See main.c's STAGE5_RTOS_TARGET
 * comment for the other half of this pair. */
#define STAGE5_HARNESS_TARGET 1
/* Was 90 (Stage 5 only, a quick request/response test). rtos_core's Stage 6
 * is a single attempt (not retried -- see main.c's stage6 dispatch comment,
 * 2026-07-24) with its own generous internal budget: STAGE6_STARTUP_DELAY_SEC
 * =20s before it even attempts to connect (gives the operator time to kill
 * mm_miner first -- see BUILD_HARNESS.md), then up to
 * STAGE6_CONNECT_BUDGET_SEC=180s for kd_ipcmsg_connect() itself (observed
 * live to genuinely take on that order sometimes), then STAGE6_RUN_SEC=90s
 * of mining loop. The harness needs to outlast that whole window -- launch
 * it promptly (right after killing mm_miner) so it's not itself the
 * bottleneck. */
#define HARD_DEADLINE_SEC 340
/* Small dummy job, same values/reasoning as rtos_core's own Stage 4 test
 * job (main.c's STAGE4_DUMMY_JOB_ID etc.) -- nmerkles=0 so the merkle root
 * collapses to plain sha256d(coinbase), no real coinbase tx/branch
 * structure needed to prove the JOB wire round-trip. */
#define JOB_DUMMY_ID 0xC0FFEE02u
#define JOB_COINBASE_LEN 64
#define JOB_MERKLE_OFFSET 36

/*
 * mV -> DC/DC I2C code. Confirmed wire format (see ipc_mujina.h /
 * long_capture_2026-07-22_findings.md): bit7 = sign (1 = below 3600mV
 * base), bits[6:0] = magnitude in 26mV steps. Rounds toward zero (a
 * partial step gets dropped, not rounded up) -- fine for the Stage 5 test
 * value (3600mV exactly, offset 0), not tuned for precision mode-switching
 * yet.
 */
static unsigned char mv_to_code(int target_mv, int *applied_mv_out)
{
	int off = target_mv - IPC_DCDC_BASE_MV;
	int steps = off / IPC_DCDC_STEP_MV;
	unsigned char code;
	int applied_off;

	if (steps >= 0) {
		if (steps > 0x7F)
			steps = 0x7F;
		code = (unsigned char)steps;
		applied_off = steps * IPC_DCDC_STEP_MV;
	} else {
		int mag = -steps;

		if (mag > 0x7F)
			mag = 0x7F;
		code = (unsigned char)(0x80 | mag);
		applied_off = -(mag * IPC_DCDC_STEP_MV);
	}
	if (applied_mv_out)
		*applied_mv_out = IPC_DCDC_BASE_MV + applied_off;
	return code;
}

/* Returns applied mV on success, -1 on failure (i2cset nonzero exit). */
static int apply_voltage_mv(int target_mv)
{
	int applied_mv;
	unsigned char code = mv_to_code(target_mv, &applied_mv);
	char cmd[128];
	int rc;

	snprintf(cmd, sizeof(cmd), "i2cset -y %d 0x%02x 0x%02x 0x%02x",
		IPC_DCDC_I2C_BUS, IPC_DCDC_I2C_ADDR, IPC_DCDC_VOLT_REG, code);
	fprintf(stderr, "[harness] target=%dmV -> code=0x%02x (applied=%dmV) -> %s\n",
		target_mv, code, applied_mv, cmd);

	rc = system(cmd);
	if (rc != 0) {
		fprintf(stderr, "[harness] i2cset failed, system() rc=%d\n", rc);
		return -1;
	}
	return applied_mv;
}

/* Fire-and-forget send -- mirrors rtos_core's own ipc_link_send_only(),
 * just called directly against the SDK API here since the harness doesn't
 * link ipc_link.c (that's rtos_core-side only). */
static int send_only(k_s32 id, uint16_t cmd, const void *payload, uint16_t len)
{
	k_ipcmsg_message_t *req = kd_ipcmsg_create_message(1u /* IPC_LINK_MODULE_ID */, cmd, payload, len);
	k_s32 rc;

	if (!req)
		return -1;
	rc = kd_ipcmsg_send_only(id, req);
	kd_ipcmsg_destroy_message(req);
	return (rc == K_SUCCESS) ? 0 : -1;
}

static void send_dummy_job(k_s32 id)
{
	struct ipc_job job;
	unsigned char wire[IPC_JOB_WIRE_HDR_LEN + JOB_COINBASE_LEN];
	uint16_t wire_len;

	memset(&job, 0, sizeof(job));
	job.job_id = JOB_DUMMY_ID;
	job.coinbase_len = JOB_COINBASE_LEN;
	memset(job.coinbase, 0x42, JOB_COINBASE_LEN);
	job.nonce2_start = 0;
	job.nonce2_offset = 4;
	job.nonce2_size = 4;
	job.merkle_offset = JOB_MERKLE_OFFSET;
	job.nmerkles = 0;
	job.header[0] = 0x01;
	memset(job.header + 4, 0xAA, 32);
	*(uint32_t *)(job.header + 68) = 0x5f5e1000;
	*(uint32_t *)(job.header + 72) = 0x1d00ffff;
	memset(job.target, 0xff, sizeof(job.target)); /* maximally easy, same as Stage 4's TARGETL/H */
	/* Real vmask table captured live from stock firmware at job start
	 * (uart_register_map_observed.md, t=31.392s, written once to
	 * 0x00..0x1C) -- not a guess. Exercises task_send_work()'s real,
	 * previously-dead vmask-diff-and-write path (toast.c), which had
	 * nothing non-zero to send before ipc_job carried this field. */
	job.vmask[0] = 0x00000020;
	job.vmask[1] = 0x00E0FF3F;
	job.vmask[2] = 0x00800020;
	job.vmask[3] = 0x00000120;
	job.vmask[4] = 0x00000220;
	job.vmask[5] = 0x00000420;
	job.vmask[6] = 0x00000820;
	job.vmask[7] = 0x00001020;
	job.work_restart = 1;

	if (ipc_job_pack(&job, wire, sizeof(wire), &wire_len) != 0) {
		fprintf(stderr, "[harness] ipc_job_pack failed (shouldn't happen for this small a job)\n");
		return;
	}
	if (send_only(id, IPC_MSG_JOB, wire, wire_len) == 0)
		fprintf(stderr, "[harness] sent JOB job_id=0x%08x (%u bytes packed)\n", job.job_id, (unsigned)wire_len);
	else
		fprintf(stderr, "[harness] JOB send_only failed\n");
}

/* NEW (2026-07-26): unlike --stage6replay's toast_raw_write_words() bypass
 * (which never recomputes the merkle root when nonce2 changes -- the flaw
 * behind an earlier false-positive "nonce found" this session), the real
 * IPC_MSG_JOB path goes through toast_set_work()/task_send_work(), which
 * calls miner_gen_nonce2_work() (src/miner.c) on every single work refresh
 * -- it genuinely recomputes header+merkle_offset from THIS job's own
 * coinbase+nonce2 each time, via a real double-SHA256 (gen_hash()). Any
 * nonce reported back through this path is checkable against a header we
 * fully control, no guessing about mm_miner's real stratum data required.
 *
 * prevhash/ntime/nbits below are NOT invented -- they're the real bytes
 * from long_full_capture.sr's actual captured job (uart_d1_packets.csv,
 * the same job g_real_job_words[] in rtos_core/src/main.c was built from),
 * confirmed byte-identical across all 12 chips' captured payloads (proving
 * they don't depend on nonce2, unlike the merkle_root region, which is
 * chip-specific in the capture and therefore NOT reused here -- our own
 * coinbase+nonce2 will produce a different, but self-consistent, computed
 * merkle root instead). Target is deliberately kept maximally easy
 * (all-0xff, matching send_dummy_job()) so a genuine finding is fast and
 * unambiguous -- this tests "does the real IPC-driven job pipeline
 * genuinely search and report a valid nonce at all", not real Bitcoin
 * network difficulty. */
#define JOB_REAL_ID 0xC0FFEE03u

static void send_real_job(k_s32 id)
{
	struct ipc_job job;
	unsigned char wire[IPC_JOB_WIRE_HDR_LEN + JOB_COINBASE_LEN];
	uint16_t wire_len;

	memset(&job, 0, sizeof(job));
	job.job_id = JOB_REAL_ID;
	job.coinbase_len = JOB_COINBASE_LEN;
	memset(job.coinbase, 0x42, JOB_COINBASE_LEN);
	job.nonce2_start = 0;
	job.nonce2_offset = 4;
	job.nonce2_size = 4;
	job.merkle_offset = JOB_MERKLE_OFFSET;
	job.nmerkles = 0;

	/* Real prevhash (header[4:36], 32 bytes) -- byte-identical across all
	 * 12 chips in the real capture, confirming it's the shared, nonce2-
	 * independent field. Written as raw uint32_t stores at the same
	 * offsets task_send_work() reads them from (no byte-swap -- matches
	 * how the dummy job above writes ntime/nbits). */
	*(uint32_t *)(job.header +  4) = 0x347AB651u;
	*(uint32_t *)(job.header +  8) = 0x51CB630Au;
	*(uint32_t *)(job.header + 12) = 0x562C2C6Eu;
	*(uint32_t *)(job.header + 16) = 0xC42CBD7Bu;
	*(uint32_t *)(job.header + 20) = 0x6E1A2F29u;
	*(uint32_t *)(job.header + 24) = 0xB9070200u;
	*(uint32_t *)(job.header + 28) = 0x00000000u;
	*(uint32_t *)(job.header + 32) = 0x00000000u;
	/* header[36:68] (merkle_offset, 32 bytes) intentionally left zeroed --
	 * miner_gen_nonce2_work() overwrites it every work refresh. */
	/* Real ntime/nbits (header[68:76]) -- also byte-identical across all
	 * 12 chips, same reasoning as prevhash above. */
	*(uint32_t *)(job.header + 68) = 0xC80E6067u;
	*(uint32_t *)(job.header + 72) = 0xFA970217u;

	/* CHANGED (2026-07-26, user-directed): real, non-trivial target
	 * (TARGETL=0x00000000, TARGETH=0x000FFFF0 -- the same real difficulty
	 * from long_full_capture.sr, not the maximally-easy all-0xff guess).
	 * stage6_worker() maps target[0:4]->TARGETL, target[4:8]->TARGETH
	 * (memcpy, no byte-swap). Now that comms are healthy (--enum removed,
	 * real temps confirmed) and PLL is ramping to a real LOW-mode
	 * frequency, a hit against this target would actually be meaningful
	 * and hash-verifiable, unlike the easy target where literally any
	 * hash qualifies and yield says nothing either way. */
	memset(job.target, 0x00, sizeof(job.target));
	*(uint32_t *)(job.target + 0) = 0x00000000u; /* TARGETL */
	*(uint32_t *)(job.target + 4) = 0x000FFFF0u; /* TARGETH */

	job.vmask[0] = 0x00000020;
	job.vmask[1] = 0x00E0FF3F;
	job.vmask[2] = 0x00800020;
	job.vmask[3] = 0x00000120;
	job.vmask[4] = 0x00000220;
	job.vmask[5] = 0x00000420;
	job.vmask[6] = 0x00000820;
	job.vmask[7] = 0x00001020;
	job.work_restart = 1;

	if (ipc_job_pack(&job, wire, sizeof(wire), &wire_len) != 0) {
		fprintf(stderr, "[harness] ipc_job_pack failed (real job)\n");
		return;
	}
	if (send_only(id, IPC_MSG_JOB, wire, wire_len) == 0)
		fprintf(stderr, "[harness] sent REAL JOB job_id=0x%08x (%u bytes packed, "
			"real prevhash/ntime/nbits from long_full_capture.sr)\n",
			job.job_id, (unsigned)wire_len);
	else
		fprintf(stderr, "[harness] REAL JOB send_only failed\n");
}

static int g_set_mode_sent;

static void handle_message(k_s32 s32Id, k_ipcmsg_message_t *msg)
{
	if (!msg)
		return;

	fprintf(stderr, "[harness] recv module=%u cmd=%u len=%u\n",
		(unsigned)msg->u32Module, (unsigned)msg->u32CMD, (unsigned)msg->u32BodyLen);

	if (msg->u32CMD == IPC_MSG_HELLO) {
		k_ipcmsg_message_t *resp = kd_ipcmsg_create_resp_message(msg, 0, NULL, 0);

		if (resp) {
			kd_ipcmsg_send_async(s32Id, resp, NULL);
			kd_ipcmsg_destroy_message(resp);
		}
		fprintf(stderr, "[harness] replied HELLO\n");
	} else if (msg->u32CMD == IPC_MSG_SET_VOLTAGE && msg->u32BodyLen == sizeof(struct ipc_set_voltage) && msg->pBody) {
		struct ipc_set_voltage req;
		struct ipc_status st;
		int applied;
		k_ipcmsg_message_t *resp;

		memcpy(&req, msg->pBody, sizeof(req));
		applied = apply_voltage_mv(req.target_mv);

		memset(&st, 0, sizeof(st));
		st.voltage_mv = applied > 0 ? (uint32_t)applied : 0;

		resp = kd_ipcmsg_create_resp_message(msg, applied > 0 ? 0 : -1, &st, sizeof(st));
		if (resp) {
			kd_ipcmsg_send_async(s32Id, resp, NULL);
			kd_ipcmsg_destroy_message(resp);
		}
		fprintf(stderr, "[harness] applied=%dmV, replied STATUS\n", applied);
	} else if (msg->u32CMD == IPC_MSG_NONCE && msg->u32BodyLen == sizeof(struct ipc_nonce) && msg->pBody) {
		struct ipc_nonce n;

		memcpy(&n, msg->pBody, sizeof(n));
		fprintf(stderr, "[harness] NONCE job_id=0x%08x nonce2=0x%08x nonce=0x%08x asic_id=%u "
			"miner_id=%u ntime=%u mid_id=%u\n",
			n.job_id, n.nonce2, n.nonce, (unsigned)n.asic_id, (unsigned)n.miner_id,
			(unsigned)n.ntime, (unsigned)n.mid_id);
	} else if (msg->u32CMD == IPC_MSG_STATUS && msg->u32BodyLen == sizeof(struct ipc_status) && msg->pBody) {
		struct ipc_status st;

		memcpy(&st, msg->pBody, sizeof(st));
		fprintf(stderr, "[harness] STATUS asics_total=%u ghsmm=%u temp_avg=%.1f temp_max=%.1f "
			"pll_freq=[%u,%u,%u,%u] err_crc=%u voltage_mv=%u\n",
			st.asics_total, st.ghsmm, st.temp_avg, st.temp_max,
			st.pll_freq[0], st.pll_freq[1], st.pll_freq[2], st.pll_freq[3],
			st.err_crc, st.voltage_mv);

		/* CHANGED (2026-07-26, user-directed): real LOW-mode target
		 * (220->280MHz across the 4 domains, interval=20 -- matches
		 * --stage6replay's own toast_ramp_pll_freq(channel, 100, 220, 20)
		 * convention). This is no longer a no-op wire round-trip test --
		 * it drives stage6_worker()'s real gradual-ramp code path
		 * (toast_ramp_pll_freq(), added this session). Voltage must
		 * already be at LOW's 3392mV, which SET_VOLTAGE above already
		 * confirmed applying (0x88) before this ever gets sent.
		 * SAFETY CONSTRAINT (user-mandated 2026-07-25): LOW mode only,
		 * do not raise domain0 past 220 (giving domain3=280) at this
		 * voltage. */
		if (!g_set_mode_sent) {
			struct ipc_set_mode sm;

			g_set_mode_sent = 1;
			memset(&sm, 0, sizeof(sm));
			sm.pll_freq[0] = 220;
			sm.pll_freq[1] = 240;
			sm.pll_freq[2] = 260;
			sm.pll_freq[3] = 280;
			sm.work_mode = 0;
			if (send_only(s32Id, IPC_MSG_SET_MODE, &sm, sizeof(sm)) == 0)
				fprintf(stderr, "[harness] sent SET_MODE pll_freq=[220,240,260,280] (LOW mode)\n");
			else
				fprintf(stderr, "[harness] SET_MODE send_only failed\n");
		}
	}
}

static void *run_thread(void *arg)
{
	k_s32 id = *(k_s32 *)arg;

	kd_ipcmsg_run(id); /* blocks until kd_ipcmsg_disconnect() closes the fd */
	return NULL;
}

int main(void)
{
	k_ipcmsg_connect_t attr;
	k_s32 id = -1;
	pthread_t tid;
	time_t deadline;

	attr.u32RemoteId = STAGE5_HARNESS_TARGET;
	attr.u32Port = PORT;
	attr.u32Priority = 0;

	if (kd_ipcmsg_add_service(SERVICE_NAME, &attr) != K_SUCCESS) {
		fprintf(stderr, "[harness] kd_ipcmsg_add_service failed\n");
		return 1;
	}

	fprintf(stderr, "[harness] connecting (blocking kd_ipcmsg_connect) target=%d port=%d ...\n",
		STAGE5_HARNESS_TARGET, PORT);
	if (kd_ipcmsg_connect(&id, SERVICE_NAME, handle_message) != K_SUCCESS) {
		fprintf(stderr, "[harness] kd_ipcmsg_connect failed\n");
		kd_ipcmsg_del_service(SERVICE_NAME);
		return 2;
	}
	fprintf(stderr, "[harness] connected, id=%d\n", id);

	if (pthread_create(&tid, NULL, run_thread, &id) != 0) {
		fprintf(stderr, "[harness] pthread_create failed\n");
		kd_ipcmsg_disconnect(id);
		kd_ipcmsg_del_service(SERVICE_NAME);
		return 3;
	}

	/* Give rtos_core's Stage 6 worker a moment to finish its own setup
	 * (RST pulse, uart_init, set_miner_init, ipc_link_open) before pushing
	 * a job -- matches the "shortly after connecting" framing, not tied
	 * to any documented requirement, just avoids a race against rtos_core
	 * still being mid-init on its side of the same connect. */
	sleep(2);
	send_real_job(id);

	deadline = time(NULL) + HARD_DEADLINE_SEC;
	while (time(NULL) < deadline)
		sleep(1);

	kd_ipcmsg_disconnect(id);
	pthread_join(tid, NULL);
	kd_ipcmsg_del_service(SERVICE_NAME);
	fprintf(stderr, "[harness] exiting\n");
	return 0;
}

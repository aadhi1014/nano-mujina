/*
 * C shim bridging mujina-miner to rtos_core.elf's IPCM protocol
 * (ipc_protocol.h). Talks to the raw SDK API directly so it can reply to
 * IPC_MSG_HELLO/IPC_MSG_SET_VOLTAGE sync requests from rtos_core.
 * Reuses ipc_protocol.c's ipc_job_pack()/ipc_job_unpack() for wire-format
 * packing.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdk/k_ipcmsg.h"
#include "ipc_protocol.h"
#include "nano3s_ipc_shim.h"

/* Must match rtos_core/src/main.c's service name/port; only one process
 * may hold this registration at a time. */
#define SERVICE_NAME "mujina_svc"
#define PORT 300
#define HARNESS_TARGET 1

#define NANO3S_NONCE_QUEUE_CAP 64

static k_s32 g_ipc_id = -1;
static pthread_t g_run_tid;
static int g_run_thread_started = 0;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static nano3s_nonce_t g_nonce_queue[NANO3S_NONCE_QUEUE_CAP];
static size_t g_nonce_head = 0;  /* next slot to pop */
static size_t g_nonce_count = 0; /* number queued */

static nano3s_status_t g_last_status;
static int g_have_status = 0;

static int send_only(k_s32 id, uint16_t cmd, const void *payload, uint16_t len)
{
	k_ipcmsg_message_t *req = kd_ipcmsg_create_message(1u, cmd, payload, len);
	k_s32 rc;

	if (!req)
		return -1;
	rc = kd_ipcmsg_send_only(id, req);
	kd_ipcmsg_destroy_message(req);
	return (rc == K_SUCCESS) ? 0 : -1;
}

static void handle_ipc_message(k_s32 s32Id, k_ipcmsg_message_t *msg)
{
	if (!msg)
		return;

	if (msg->u32CMD == IPC_MSG_HELLO) {
		k_ipcmsg_message_t *resp = kd_ipcmsg_create_resp_message(msg, 0, NULL, 0);

		if (resp) {
			kd_ipcmsg_send_async(s32Id, resp, NULL);
			kd_ipcmsg_destroy_message(resp);
		}
	} else if (msg->u32CMD == IPC_MSG_NONCE && msg->u32BodyLen == sizeof(struct ipc_nonce) && msg->pBody) {
		struct ipc_nonce n;
		nano3s_nonce_t out;

		memcpy(&n, msg->pBody, sizeof(n));
		out.job_id = n.job_id;
		out.nonce2 = n.nonce2;
		out.nonce = n.nonce;
		out.asic_id = n.asic_id;
		out.miner_id = n.miner_id;
		out.ntime = n.ntime;
		out.mid_id = n.mid_id;

		pthread_mutex_lock(&g_lock);
		if (g_nonce_count == NANO3S_NONCE_QUEUE_CAP) {
			/* Queue full: drop oldest entry by advancing head; count stays at cap. */
			g_nonce_head = (g_nonce_head + 1) % NANO3S_NONCE_QUEUE_CAP;
			fprintf(stderr, "[nano3s_ipc_shim] nonce queue full, dropped oldest "
				"(consumer not polling fast enough?)\n");
		} else {
			g_nonce_count++;
		}
		{
			size_t tail = (g_nonce_head + g_nonce_count - 1) % NANO3S_NONCE_QUEUE_CAP;
			g_nonce_queue[tail] = out;
		}
		pthread_mutex_unlock(&g_lock);
	} else if (msg->u32CMD == IPC_MSG_STATUS && msg->u32BodyLen == sizeof(struct ipc_status) && msg->pBody) {
		struct ipc_status st;
		nano3s_status_t out;
		uint32_t i, k;

		memcpy(&st, msg->pBody, sizeof(st));
		out.asics_total = st.asics_total;
		out.ghsmm = st.ghsmm;
		out.ghsspd = st.ghsspd;
		out.spd_dh = st.spd_dh;
		out.temp_avg = st.temp_avg;
		out.temp_max = st.temp_max;
		memcpy(out.pll_freq, st.pll_freq, sizeof(out.pll_freq));
		out.err_crc = st.err_crc;
		out.voltage_mv = st.voltage_mv;
		out.paused = st.paused;
		out.nonce_read_err = st.nonce_read_err;
		out.nonce_bad_len = st.nonce_bad_len;
		out.nonce_overflow = st.nonce_overflow;
		out.regread_err = st.regread_err;
		out.chip_count = st.chip_count;
		for (i = 0; i < NANO3S_STATUS_MAX_CHIPS; i++) {
			out.chips[i].temp_c = st.chips[i].temp_c;
			out.chips[i].volt_mv = st.chips[i].volt_mv;
			for (k = 0; k < 4; k++) {
				out.chips[i].pll_cnt[k] = st.chips[i].pll_cnt[k];
				out.chips[i].pll_freq[k] = st.chips[i].pll_freq[k];
			}
			out.chips[i].nonce_timeout = st.chips[i].nonce_timeout;
			out.chips[i].nonce_heartbeat = st.chips[i].nonce_heartbeat;
			out.chips[i].nonce_data = st.chips[i].nonce_data;
			out.chips[i].ghsspd = st.chips[i].ghsspd;
			out.chips[i].spd_dh = st.chips[i].spd_dh;
		}

		pthread_mutex_lock(&g_lock);
		g_last_status = out;
		g_have_status = 1;
		pthread_mutex_unlock(&g_lock);
	} else if (msg->u32CMD == IPC_MSG_SET_VOLTAGE && msg->u32BodyLen == sizeof(struct ipc_set_voltage) && msg->pBody) {
		/* Converts target_mv to a DC/DC step code and applies it via i2cset. */
		struct ipc_set_voltage req;
		int off, steps, applied;
		unsigned char code;
		char cmd[128];
		int rc;
		struct ipc_status st;
		k_ipcmsg_message_t *resp;

		memcpy(&req, msg->pBody, sizeof(req));
		off = req.target_mv - IPC_DCDC_BASE_MV;
		steps = off / IPC_DCDC_STEP_MV;
		if (steps >= 0) {
			if (steps > 0x7F) steps = 0x7F;
			code = (unsigned char)steps;
			applied = IPC_DCDC_BASE_MV + steps * IPC_DCDC_STEP_MV;
		} else {
			int mag = -steps;

			if (mag > 0x7F) mag = 0x7F;
			code = (unsigned char)(0x80 | mag);
			applied = IPC_DCDC_BASE_MV - mag * IPC_DCDC_STEP_MV;
		}
		snprintf(cmd, sizeof(cmd), "i2cset -y %d 0x%02x 0x%02x 0x%02x",
			IPC_DCDC_I2C_BUS, IPC_DCDC_I2C_ADDR, IPC_DCDC_VOLT_REG, code);
		fprintf(stderr, "[nano3s_ipc_shim] SET_VOLTAGE target=%dmV -> code=0x%02x applied=%dmV -> %s\n",
			req.target_mv, code, applied, cmd);

		rc = system(cmd);
		memset(&st, 0, sizeof(st));
		st.voltage_mv = (rc == 0) ? (uint32_t)applied : 0;
		resp = kd_ipcmsg_create_resp_message(msg, (rc == 0) ? 0 : -1, &st, sizeof(st));
		if (resp) {
			kd_ipcmsg_send_async(s32Id, resp, NULL);
			kd_ipcmsg_destroy_message(resp);
		}
		if (rc != 0)
			fprintf(stderr, "[nano3s_ipc_shim] i2cset failed\n");
	}
}

static void *run_thread(void *arg)
{
	k_s32 id = *(k_s32 *)arg;

	kd_ipcmsg_run(id); /* blocks until rtos_core.elf disconnects */

	/* Terminates the process on disconnect so its supervisor relaunches it
	 * to reconnect. */
	fprintf(stderr, "[nano3s_ipc_shim] rtos_core IPC connection closed, exiting "
		"(supervisor will relaunch to reconnect)\n");
	exit(0);
	return NULL;
}

int nano3s_ipc_open(void)
{
	k_ipcmsg_connect_t attr;

	attr.u32RemoteId = HARNESS_TARGET;
	attr.u32Port = PORT;
	attr.u32Priority = 0;

	if (kd_ipcmsg_add_service(SERVICE_NAME, &attr) != K_SUCCESS) {
		fprintf(stderr, "[nano3s_ipc_shim] kd_ipcmsg_add_service failed\n");
		return -1;
	}

	fprintf(stderr, "[nano3s_ipc_shim] connecting to rtos_core (blocking) ...\n");
	if (kd_ipcmsg_connect(&g_ipc_id, SERVICE_NAME, handle_ipc_message) != K_SUCCESS) {
		fprintf(stderr, "[nano3s_ipc_shim] kd_ipcmsg_connect failed\n");
		kd_ipcmsg_del_service(SERVICE_NAME);
		return -1;
	}
	fprintf(stderr, "[nano3s_ipc_shim] connected, id=%d\n", g_ipc_id);

	if (pthread_create(&g_run_tid, NULL, run_thread, &g_ipc_id) != 0) {
		fprintf(stderr, "[nano3s_ipc_shim] pthread_create failed\n");
		return -1;
	}
	g_run_thread_started = 1;
	return 0;
}

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
	const uint32_t vmask[8])
{
	struct ipc_job job;
	unsigned char wire[IPC_JOB_WIRE_HDR_LEN + AVA_P_COINBASE_SIZE + AVA_P_MERKLES_COUNT * 32];
	uint16_t wire_len;

	if (coinbase_len > sizeof(job.coinbase) || nmerkles < 0 || (size_t)nmerkles > AVA_P_MERKLES_COUNT)
		return -1;

	memset(&job, 0, sizeof(job));
	job.job_id = job_id;
	job.nonce2_start = nonce2_start;
	job.nonce2_offset = nonce2_offset;
	job.nonce2_size = nonce2_size;
	job.merkle_offset = merkle_offset;
	job.nmerkles = nmerkles;
	if (nmerkles > 0)
		memcpy(job.merkles, merkles, (size_t)nmerkles * 32);
	memcpy(job.coinbase, coinbase, coinbase_len);
	job.coinbase_len = coinbase_len;
	memcpy(job.header, header, sizeof(job.header));
	memcpy(job.target, target, sizeof(job.target));
	job.work_restart = work_restart;
	memcpy(job.vmask, vmask, sizeof(job.vmask));

	if (ipc_job_pack(&job, wire, sizeof(wire), &wire_len) != 0) {
		fprintf(stderr, "[nano3s_ipc_shim] ipc_job_pack failed (job_id=0x%08x)\n", job_id);
		return -1;
	}
	if (send_only(g_ipc_id, IPC_MSG_JOB, wire, wire_len) != 0) {
		fprintf(stderr, "[nano3s_ipc_shim] JOB send_only failed (job_id=0x%08x)\n", job_id);
		return -1;
	}
	return 0;
}

int nano3s_ipc_poll_nonce(nano3s_nonce_t *out)
{
	int got = 0;

	pthread_mutex_lock(&g_lock);
	if (g_nonce_count > 0) {
		*out = g_nonce_queue[g_nonce_head];
		g_nonce_head = (g_nonce_head + 1) % NANO3S_NONCE_QUEUE_CAP;
		g_nonce_count--;
		got = 1;
	}
	pthread_mutex_unlock(&g_lock);
	return got;
}

int nano3s_ipc_get_status(nano3s_status_t *out)
{
	int have;

	pthread_mutex_lock(&g_lock);
	have = g_have_status;
	if (have)
		*out = g_last_status;
	pthread_mutex_unlock(&g_lock);
	return have;
}

int nano3s_ipc_pause(void)
{
	uint8_t dummy = 0; /* a zero-length body fails to send; use a 1-byte payload instead */

	return send_only(g_ipc_id, IPC_MSG_PAUSE, &dummy, sizeof(dummy));
}

int nano3s_ipc_resume(void)
{
	uint8_t dummy = 0;

	return send_only(g_ipc_id, IPC_MSG_RESUME, &dummy, sizeof(dummy));
}

int nano3s_ipc_set_mode(const uint32_t pll_freq[4], uint8_t work_mode)
{
	struct ipc_set_mode sm;

	memcpy(sm.pll_freq, pll_freq, sizeof(sm.pll_freq));
	sm.work_mode = work_mode;
	return send_only(g_ipc_id, IPC_MSG_SET_MODE, &sm, sizeof(sm));
}

int nano3s_ipc_set_rolltime_raw(uint32_t value)
{
	struct ipc_set_rolltime_raw rt;

	rt.value = value;
	return send_only(g_ipc_id, IPC_MSG_SET_ROLLTIME_RAW, &rt, sizeof(rt));
}

int nano3s_ipc_set_voltage_raw(int32_t target_mv)
{
	struct ipc_set_voltage_raw vr;

	vr.target_mv = target_mv;
	return send_only(g_ipc_id, IPC_MSG_SET_VOLTAGE_RAW, &vr, sizeof(vr));
}

int nano3s_ipc_reset_spdlog_raw(void)
{
	uint8_t dummy = 0; /* a zero-length body fails to send; use a 1-byte payload instead */

	return send_only(g_ipc_id, IPC_MSG_RESET_SPDLOG_RAW, &dummy, sizeof(dummy));
}

void nano3s_ipc_close(void)
{
	if (g_ipc_id >= 0) {
		kd_ipcmsg_disconnect(g_ipc_id);
		if (g_run_thread_started)
			pthread_join(g_run_tid, NULL);
		kd_ipcmsg_del_service(SERVICE_NAME);
		g_ipc_id = -1;
	}
}

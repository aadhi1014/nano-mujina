/*
 * IPCM userspace link, built on the K230 SDK's real kd_ipcmsg_* API
 * (vendor/sdk_libs/libipcmsg.a) rather than the raw /dev/ipcm_user ioctls
 * directly. See include/ipc_link.h and docs/BUILD_NOTES.md's Stage 5
 * section for why: the raw-ioctl CONNECT sequence this file used to
 * implement was disassembly-confirmed correct (it matched the vendor
 * library's own IPCMSG_TransConnect instruction-for-instruction: same
 * ioctl numbers, same 44-byte attr struct, same ATTR_INIT-clobbers-fields
 * fix) but a connected channel alone was never enough -- nothing in that
 * layer does the actual message framing/dispatch, which lives in
 * libipcmsg.a's kd_ipcmsg_send_sync/kd_ipcmsg_run instead. This file now
 * calls that real code for both halves.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ipc_link.h"
#include "sdk/k_ipcmsg.h"

static ipc_link_recv_fn g_recv_handler;

void ipc_link_set_recv_handler(ipc_link_recv_fn fn)
{
	g_recv_handler = fn;
}

static void ipc_link_recv_cb(k_s32 s32Id, k_ipcmsg_message_t *msg)
{
	/* Fires for unsolicited messages from the peer -- kd_ipcmsg_send_sync's
	 * own replies are matched and consumed by the library internally
	 * before reaching here. This is where mujina's pushed IPC_MSG_JOB/
	 * IPC_MSG_SET_MODE arrive (Stage 6 onward). */
	(void)s32Id;
	if (!msg)
		return;
	if (g_recv_handler) {
		g_recv_handler((uint16_t)msg->u32CMD, msg->pBody, (uint16_t)msg->u32BodyLen);
		return;
	}
	fprintf(stderr, "[ipc_link] unsolicited msg module=%u cmd=%u len=%u (no handler registered)\n",
		(unsigned)msg->u32Module, (unsigned)msg->u32CMD, (unsigned)msg->u32BodyLen);
}

struct run_thread_arg {
	k_s32 id;
};

static void *run_thread_main(void *arg)
{
	struct run_thread_arg *a = arg;
	k_s32 id = a->id;

	free(a);
	kd_ipcmsg_run(id); /* blocks until kd_ipcmsg_disconnect() closes the fd */
	return NULL;
}

int ipc_link_open(const char *service_name, uint32_t remote_id, uint32_t port)
{
	k_ipcmsg_connect_t attr;
	k_s32 id = -1;
	pthread_t run_tid;
	struct run_thread_arg *run_arg;

	attr.u32RemoteId = remote_id;
	attr.u32Port = port;
	attr.u32Priority = 0;

	if (kd_ipcmsg_add_service(service_name, &attr) != K_SUCCESS)
		return -1;

	if (kd_ipcmsg_connect(&id, service_name, ipc_link_recv_cb) != K_SUCCESS) {
		kd_ipcmsg_del_service(service_name);
		return -1;
	}

	run_arg = malloc(sizeof(*run_arg));
	if (!run_arg) {
		kd_ipcmsg_disconnect(id);
		kd_ipcmsg_del_service(service_name);
		return -1;
	}
	run_arg->id = id;

	if (pthread_create(&run_tid, NULL, run_thread_main, run_arg) != 0) {
		free(run_arg);
		kd_ipcmsg_disconnect(id);
		kd_ipcmsg_del_service(service_name);
		return -1;
	}
	pthread_detach(run_tid);

	return (int)id;
}

int ipc_link_send_sync(int id, uint16_t cmd, const void *payload, uint16_t len,
			void *reply_buf, uint16_t reply_buf_len, uint16_t *out_reply_len,
			int32_t *out_ret_val, int timeout_ms)
{
	k_ipcmsg_message_t *req, *resp = NULL;
	k_s32 rc;

	req = kd_ipcmsg_create_message(IPC_LINK_MODULE_ID, cmd, payload, len);
	if (!req)
		return -1;

	rc = kd_ipcmsg_send_sync((k_s32)id, req, &resp, timeout_ms);
	kd_ipcmsg_destroy_message(req);

	if (rc != K_SUCCESS)
		return (rc == K_IPCMSG_ETIMEOUT) ? K_IPCMSG_ETIMEOUT : -1;

	if (!resp)
		return -1;

	if (out_ret_val)
		*out_ret_val = resp->s32RetVal;
	if (reply_buf && reply_buf_len && resp->pBody && resp->u32BodyLen) {
		uint16_t n = (resp->u32BodyLen < reply_buf_len) ? (uint16_t)resp->u32BodyLen : reply_buf_len;
		memcpy(reply_buf, resp->pBody, n);
	}
	if (out_reply_len)
		*out_reply_len = (uint16_t)resp->u32BodyLen;

	kd_ipcmsg_destroy_message(resp);
	return 0;
}

int ipc_link_send_only(int id, uint16_t cmd, const void *payload, uint16_t len)
{
	k_ipcmsg_message_t *req;
	k_s32 rc;

	req = kd_ipcmsg_create_message(IPC_LINK_MODULE_ID, cmd, payload, len);
	if (!req)
		return -1;

	rc = kd_ipcmsg_send_only((k_s32)id, req);
	kd_ipcmsg_destroy_message(req);

	return (rc == K_SUCCESS) ? 0 : -1;
}

void ipc_link_close(int id, const char *service_name)
{
	kd_ipcmsg_disconnect((k_s32)id);
	kd_ipcmsg_del_service(service_name);
}

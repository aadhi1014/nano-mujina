#ifndef IPC_LINK_H
#define IPC_LINK_H

#include <stdint.h>

/*
 * Wrapper around the K230 SDK's kd_ipcmsg_* API (vendor/sdk_libs/
 * libipcmsg.a, headers in include/sdk/) for connecting and exchanging
 * messages over an IPCM channel.
 */

/* User-defined "module" id for all rtos_core <-> mujina traffic. The SDK's
 * cmd/module split is entirely user-defined (see k_ipcmsg.h) -- module is
 * fixed since ipc_protocol.h's IPC_MSG_* enum already disambiguates message
 * kinds via cmd. */
#define IPC_LINK_MODULE_ID 1u

/*
 * Registers the given service name (kd_ipcmsg_add_service), connects
 * (blocking kd_ipcmsg_connect -- returns only once both sides have
 * completed the handshake or the vendor library gives up), and starts the
 * kd_ipcmsg_run() dispatch pump on a detached background thread (required
 * even just to route kd_ipcmsg_send_sync's own replies back -- the pump is
 * what reads the fd and dispatches to the recv callback and the sync/async
 * bookkeeping).
 *
 * Returns the kd_ipcmsg handle (>=0) on success, -1 on failure.
 */
int ipc_link_open(const char *service_name, uint32_t remote_id, uint32_t port);

/*
 * Blocking request/reply over the connected channel. cmd is one of
 * ipc_protocol.h's IPC_MSG_* values. reply_buf/reply_buf_len/out_reply_len
 * capture the response message's body (truncated to reply_buf_len if
 * needed); out_ret_val gets the response's s32RetVal (may be NULL).
 *
 * Returns 0 on a reply received, K_IPCMSG_ETIMEOUT (see sdk/k_comm_ipcmsg.h)
 * on timeout, -1 on other failure (message alloc, send, or malformed
 * response).
 */
int ipc_link_send_sync(int id, uint16_t cmd, const void *payload, uint16_t len,
			void *reply_buf, uint16_t reply_buf_len, uint16_t *out_reply_len,
			int32_t *out_ret_val, int timeout_ms);

/*
 * Fire-and-forget send, no reply expected -- for messages the peer doesn't
 * ack at the IPC layer (IPC_MSG_NONCE, IPC_MSG_STATUS). Returns 0 on
 * successful send (not delivery/processing -- there's no round trip to
 * confirm that), -1 on failure.
 */
int ipc_link_send_only(int id, uint16_t cmd, const void *payload, uint16_t len);

typedef void (*ipc_link_recv_fn)(uint16_t cmd, const void *body, uint16_t len);

/*
 * Registers a handler for unsolicited (non-reply) messages from the peer --
 * e.g. mujina pushing IPC_MSG_JOB or IPC_MSG_SET_MODE. Must be called
 * before ipc_link_open() (the recv callback is wired up as part of
 * connect). Invoked from the kd_ipcmsg_run() dispatch thread -- keep it
 * short and thread-safe (own locking if it touches state the main loop
 * also reads/writes). Pass NULL to go back to just logging (the default).
 */
void ipc_link_set_recv_handler(ipc_link_recv_fn fn);

void ipc_link_close(int id, const char *service_name);

#endif

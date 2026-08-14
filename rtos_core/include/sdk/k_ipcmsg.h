/*
 * Verbatim (comments trimmed) from Canaan's K230 SDK,
 * src/common/cdk/user/component/ipcmsg/include/k_ipcmsg.h
 * (kendryte/k230_sdk on GitHub) -- fetched directly, not guessed. These are
 * the real symbols exported by vendor/sdk_libs/libipcmsg.a's
 * ipcmsg-operation.o/ipcmsg-message.o (confirmed via nm before writing this
 * header -- see docs/BUILD_NOTES.md's Stage 5 section for how the earlier
 * "libipcmsg.a only has the Trans layer" read turned out to be a bug in our
 * own ar-parsing script, not a real gap in the vendored archive).
 */
#ifndef __K_IPCMSG_H__
#define __K_IPCMSG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "k_comm_ipcmsg.h"

/* Register service before use; must be called with same params on both
 * sides. */
k_s32 kd_ipcmsg_add_service(const k_char* pszServiceName, const k_ipcmsg_connect_t* pstConnectAttr);
k_s32 kd_ipcmsg_del_service(const k_char* pszServiceName);

/* Non-blocking; caller must poll kd_ipcmsg_is_connect(). */
k_s32 kd_ipcmsg_try_connect(k_s32* ps32Id, const k_char* pszServiceName, k_ipcmsg_handle_fn_ptr pfnMessageHandle);
/* Blocks until both sides' connect has completed. */
k_s32 kd_ipcmsg_connect(k_s32* ps32Id, const k_char* pszServiceName, k_ipcmsg_handle_fn_ptr pfnMessageHandle);

k_s32 kd_ipcmsg_disconnect(k_s32 s32Id);
k_bool kd_ipcmsg_is_connect(k_s32 s32Id);

k_s32 kd_ipcmsg_send_only(k_s32 s32Id, k_ipcmsg_message_t *pstRequest);
k_s32 kd_ipcmsg_send_async(k_s32 s32Id, k_ipcmsg_message_t* pstMsg, k_ipcmsg_resphandle_fn_ptr pfnRespHandle);
k_s32 kd_ipcmsg_send_sync(k_s32 s32Id, k_ipcmsg_message_t* pstMsg, k_ipcmsg_message_t** ppstMsg, k_s32 s32TimeoutMs);

/* Message-dispatch pump -- must be run on its own thread once connected;
 * required even just to route kd_ipcmsg_send_sync's own replies back. */
void kd_ipcmsg_run(k_s32 s32Id);

k_ipcmsg_message_t* kd_ipcmsg_create_message(k_u32 u32Module, k_u32 u32CMD, const void* pBody, k_u32 u32BodyLen);
k_ipcmsg_message_t* kd_ipcmsg_create_resp_message(k_ipcmsg_message_t* pstRequest, k_s32 s32RetVal, void* pBody, k_u32 u32BodyLen);
void kd_ipcmsg_destroy_message(k_ipcmsg_message_t* pstMsg);

#ifdef __cplusplus
}
#endif

#endif

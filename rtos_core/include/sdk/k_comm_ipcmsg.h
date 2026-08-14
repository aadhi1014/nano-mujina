/*
 * Verbatim (comments trimmed) from Canaan's K230 SDK,
 * src/common/cdk/user/component/ipcmsg/include/k_comm_ipcmsg.h
 * (kendryte/k230_sdk on GitHub) -- fetched directly. Struct layouts here
 * are load-bearing: they must match what vendor/sdk_libs/libipcmsg.a's
 * object code expects byte-for-byte.
 */
#ifndef __K_COMM_IPCMSG_H__
#define __K_COMM_IPCMSG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <string.h>
#include "k_type.h"

#define IPCMSG_MAX_PORT_NUM 512
#define K_IPCMSG_MAX_CONTENT_LEN (1024)
#define K_IPCMSG_PRIVDATA_NUM (8)
#define K_IPCMSG_INVALID_MSGID (0xFFFFFFFFFFFFFFFF)

typedef struct {
	k_char aszServiceName[16];
	k_s32 s32Id;
	k_char pData[0];
} IPCMSG_TRANS_CONNECT_ATTR_S;

typedef struct IPCMSG_CONNECT_S {
	k_u32 u32RemoteId;
	k_u32 u32Port;
	k_u32 u32Priority;
} k_ipcmsg_connect_t;

/* Message structure */
typedef struct IPCMSG_MESSAGE_S {
	k_bool bIsResp;
	k_u64 u64Id;
	k_u32 u32Module;
	k_u32 u32CMD;
	k_s32 s32RetVal;
	k_s32 as32PrivData[K_IPCMSG_PRIVDATA_NUM];
	void* pBody;
	k_u32 u32BodyLen;
} k_ipcmsg_message_t;

#define K_IPCMSG_ERRNO_BASE 0x1900
#define K_IPCMSG_EINVAL (K_IPCMSG_ERRNO_BASE+1)
#define K_IPCMSG_ETIMEOUT (K_IPCMSG_ERRNO_BASE+2)
#define K_IPCMSG_ENOOP (K_IPCMSG_ERRNO_BASE+3)
#define K_IPCMSG_EINTER (K_IPCMSG_ERRNO_BASE+4)
#define K_IPCMSG_ENULL_PTR (K_IPCMSG_ERRNO_BASE+5)

#define K_IPCMSG_MAX_SERVICENAME_LEN (16)

int IPCMSG_Log(unsigned int level, const char *str, ...);

#define PRINT_LEVEL_ERROR   (1)
#define PRINT_LEVEL_WARN    (2)
#define PRINT_LEVEL_INFO    (3)
#define PRINT_LEVEL_DEBUG   (4)
#define PRINT_LEVEL         PRINT_LEVEL_ERROR

#define IPCMSG_LOGE(fmt, ...)   IPCMSG_Log(PRINT_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define IPCMSG_LOGW(fmt, ...)   IPCMSG_Log(PRINT_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define IPCMSG_LOGI(fmt, ...)   IPCMSG_Log(PRINT_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define IPCMSG_LOGD(fmt, ...)   IPCMSG_Log(PRINT_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

typedef void (*k_ipcmsg_handle_fn_ptr)(k_s32 s32Id, k_ipcmsg_message_t* pstMsg);
typedef void (*k_ipcmsg_resphandle_fn_ptr)(k_ipcmsg_message_t* pstMsg);

#ifdef __cplusplus
}
#endif

#endif

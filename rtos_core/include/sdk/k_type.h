/*
 * Verbatim (comments trimmed) from Canaan's K230 SDK,
 * src/big/mpp/include/k_type.h (kendryte/k230_sdk on GitHub) -- fetched
 * directly, not guessed, since rtos_core now links the real
 * vendor/sdk_libs/libipcmsg.a and must match its ABI exactly.
 */
#ifndef __K_TYPE_H__
#define __K_TYPE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char           k_u8;
typedef unsigned short          k_u16;
typedef unsigned int            k_u32;

typedef signed char             k_s8;
typedef short                   k_s16;
typedef int                     k_s32;

typedef unsigned long           k_u64;
typedef signed long              k_s64;

typedef char                    k_char;

typedef enum {
	K_FALSE = 0,
	K_TRUE  = 1,
} k_bool;

typedef k_u32 k_handle;

#define K_SUCCESS               (0)
#define K_FAILED                (-1)

#ifdef __cplusplus
}
#endif

#endif /* __K_TYPE_H__ */

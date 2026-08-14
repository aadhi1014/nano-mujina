#ifndef IPC_JOB_ADAPTER_H
#define IPC_JOB_ADAPTER_H

#include "a3197s_job.h"
#include "ipc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Translates a wire-format struct ipc_job (see ipc_protocol.h) into the
 * RTOS-core-internal a3197s_job_t. This is the Mujina/IPC adapter
 * boundary -- the one place the a3197s_job_t shape is derived from the
 * generic IPC job message. `out` is fully overwritten (any fields not
 * present on the wire, e.g. beyond nmerkles/coinbase_len, are zeroed). */
void a3197s_job_from_ipc(a3197s_job_t *out, const struct ipc_job *in);

#ifdef __cplusplus
}
#endif

#endif

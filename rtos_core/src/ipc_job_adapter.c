/*
 * Mujina/IPC -> a3197s_job_t adapter boundary. See a3197s_job.h for the
 * intended layering (Mujina -> this adapter -> generic IPC protocol ->
 * A3197S RTOS driver). Field mapping is unchanged from the field-by-field
 * copy this replaces (main.c's stage6_recv_handler() used to do this
 * inline); only nonce2_start -> nonce2 is renamed, matching a3197s_job_t's
 * field name.
 */

#include <string.h>
#include "ipc_job_adapter.h"

void a3197s_job_from_ipc(a3197s_job_t *out, const struct ipc_job *in)
{
	memset(out, 0, sizeof(*out));
	out->job_id = in->job_id;
	out->coinbase_len = in->coinbase_len;
	memcpy(out->coinbase, in->coinbase, in->coinbase_len);
	out->nonce2 = in->nonce2_start;
	out->nonce2_offset = in->nonce2_offset;
	out->nonce2_size = in->nonce2_size;
	out->merkle_offset = in->merkle_offset;
	out->nmerkles = in->nmerkles;
	memcpy(out->merkles, in->merkles, (size_t)in->nmerkles * 32);
	memcpy(out->header, in->header, sizeof(in->header));
	memcpy(out->target, in->target, sizeof(in->target));
	/* a3197s_submit_job() diffs vmask against the last-sent table and
	 * writes any changed entry to REG_VMASK_TABLE. */
	memcpy(out->vmask, in->vmask, sizeof(in->vmask));
	out->work_restart = in->work_restart;
}

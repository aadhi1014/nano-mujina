/*
 * Wire packing for ipc_protocol.h's message payloads. Only IPC_MSG_JOB
 * needs this: it serializes struct ipc_job field-by-field into a byte
 * buffer (and back) using explicit offsets rather than a raw struct
 * memcpy. This is the generic IPC protocol layer -- Mujina-specific
 * job translation lives in ipc_job_adapter.c, one layer up.
 */
#include <string.h>

#include "ipc_protocol.h"

static void put_u32(uint8_t **p, uint32_t v)
{
	memcpy(*p, &v, 4);
	*p += 4;
}

static void put_i32(uint8_t **p, int32_t v)
{
	memcpy(*p, &v, 4);
	*p += 4;
}

static uint32_t get_u32(const uint8_t **p)
{
	uint32_t v;

	memcpy(&v, *p, 4);
	*p += 4;
	return v;
}

static int32_t get_i32(const uint8_t **p)
{
	int32_t v;

	memcpy(&v, *p, 4);
	*p += 4;
	return v;
}

int ipc_job_pack(const struct ipc_job *job, uint8_t *buf, uint16_t buf_cap, uint16_t *out_len)
{
	uint8_t *p = buf;
	size_t need;

	if (job->nmerkles < 0 || job->nmerkles > AVA_P_MERKLES_COUNT)
		return -1;
	if (job->coinbase_len > AVA_P_COINBASE_SIZE)
		return -1;

	need = (size_t)IPC_JOB_WIRE_HDR_LEN + (size_t)job->nmerkles * 32u + job->coinbase_len;
	if (need > buf_cap)
		return -1;

	put_u32(&p, job->job_id);
	put_u32(&p, job->nonce2_start);
	put_i32(&p, job->nonce2_offset);
	put_i32(&p, job->nonce2_size);
	put_i32(&p, job->merkle_offset);
	put_i32(&p, job->nmerkles);
	put_u32(&p, job->coinbase_len);
	memcpy(p, job->header, sizeof(job->header));
	p += sizeof(job->header);
	memcpy(p, job->target, sizeof(job->target));
	p += sizeof(job->target);
	*p++ = job->work_restart;
	{
		int i;
		for (i = 0; i < 8; i++)
			put_u32(&p, job->vmask[i]);
	}
	memcpy(p, job->merkles, (size_t)job->nmerkles * 32u);
	p += (size_t)job->nmerkles * 32u;
	memcpy(p, job->coinbase, job->coinbase_len);
	p += job->coinbase_len;

	*out_len = (uint16_t)(p - buf);
	return 0;
}

int ipc_job_unpack(const uint8_t *buf, uint16_t len, struct ipc_job *job)
{
	const uint8_t *p = buf;
	uint32_t coinbase_len;
	int32_t nmerkles;

	if (len < IPC_JOB_WIRE_HDR_LEN)
		return -1;

	memset(job, 0, sizeof(*job));

	job->job_id = get_u32(&p);
	job->nonce2_start = get_u32(&p);
	job->nonce2_offset = get_i32(&p);
	job->nonce2_size = get_i32(&p);
	job->merkle_offset = get_i32(&p);
	nmerkles = get_i32(&p);
	coinbase_len = get_u32(&p);
	memcpy(job->header, p, sizeof(job->header));
	p += sizeof(job->header);
	memcpy(job->target, p, sizeof(job->target));
	p += sizeof(job->target);
	job->work_restart = *p++;
	{
		int i;
		for (i = 0; i < 8; i++)
			job->vmask[i] = get_u32(&p);
	}

	if (nmerkles < 0 || nmerkles > AVA_P_MERKLES_COUNT)
		return -1;
	if (coinbase_len > AVA_P_COINBASE_SIZE)
		return -1;
	if ((size_t)(p - buf) + (size_t)nmerkles * 32u + coinbase_len != len)
		return -1;

	job->nmerkles = nmerkles;
	memcpy(job->merkles, p, (size_t)nmerkles * 32u);
	p += (size_t)nmerkles * 32u;
	job->coinbase_len = coinbase_len;
	memcpy(job->coinbase, p, coinbase_len);

	return 0;
}

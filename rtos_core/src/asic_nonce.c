/*
 * A3197S nonce reception/decode: issues the NONCE_BUF read, classifies
 * timeout/heartbeat/data responses, and decodes each 10-byte wire
 * record into a struct asic_nonce_record. See a3197s_receive_nonces()
 * for the observed wire format.
 */

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "chip_link.h"
#include "asic_engine.h"
#include "asic_internal.h"

uint32_t g_nonce_read_err[HBOARD_COUNT];
uint32_t g_nonce_bad_len[HBOARD_COUNT];
uint32_t g_nonce_overflow[HBOARD_COUNT];
/* Distinguishes "chip responded with nothing new" (timeout) from "chip
 * responded with a valid short heartbeat frame". */
uint32_t g_nonce_timeout[HBOARD_COUNT];
uint32_t g_nonce_heartbeat[HBOARD_COUNT];
/* Per-chip breakdown of the same timeout/heartbeat/data distinction,
 * indexed by asic_id. */
uint32_t g_nonce_timeout_chip[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
uint32_t g_nonce_heartbeat_chip[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];
uint32_t g_nonce_data_chip[HBOARD_COUNT][MAX_ASIC_ON_SINGLE_HASH];

/* ---- Observed wire format (NONCE_BUF response) ----
 * A response frame is a variable-length word array:
 *   - < 7 words: either a 4-word heartbeat frame (no new nonce, chip
 *     alive) or any other short length, which is a bad/corrupt frame.
 *   - >= 7 words: a data frame. word[0..3] is a fixed-size preamble
 *     (word[0]=nonce2, word[1]=job_id, word[2]=asic_id/miner_id, all
 *     byte-swapped from wire order); words[4..] hold one or more
 *     10-byte nonce records back to back, record count = (total_bytes
 *     - 16) / 10. Each 10-byte record is: [0..3]=nonce (little-endian,
 *     NOT byte-swapped), [4..7]=version-rolling vmask match word
 *     (native order), [8]=ntime, [9]=unused/padding.
 */

/* Step: map version mask -- returns the vmask table index whose entry
 * equals `vmask`, or 0 if no entry matches. */
static int a3197s_find_version_mask_index(uint32_t pvmask[], uint32_t vmask)
{
	uint8_t i = 0;

	for (i = 0; i < 8; i++)
	{
		if (pvmask[i] == vmask)
			return i;
	}

	return 0;
}

/* Step: receive frame -- issues the NONCE_BUF read request and blocks
 * for the chip's response. Returns the transport result unchanged
 * (0=frame received, 1=failure, 2=nothing came back) and, on success,
 * *out_len = response length in 32-bit words. */
static int a3197s_nonce_receive_frame(uint8_t channel, uint16_t asic_id, uint32_t nonce_buf[NONCE_BUF_FIFO_LEN], uint32_t *out_len)
{
	uint32_t reg_addr = REG_NONCE_BUF;
	uint32_t chipid = asic_id;

	*out_len = NONCE_BUF_FIFO_LEN;
	a3197s_send_frame(channel, reg_addr, NULL, NONCE_BUF_FIFO_LEN, UART_READ_MODE);
	return a3197s_recv_frame_ex(channel, &chipid, &reg_addr, nonce_buf, out_len, 0);
}

/* Step: decode metadata + decode nonce record -- decodes one 10-byte
 * wire record at `p` (plus the frame's shared preamble words
 * preamble[0..2]) into *out, including mapping its version-rolling
 * vmask word to a table index. Optionally logs the raw and decoded
 * fields, keyed to a3197s_submit_job()'s WORK log lines by
 * job_id/nonce2. */
static void a3197s_nonce_decode_record(uint8_t channel, uint16_t asic_id,
	const uint32_t preamble[3], const uint8_t *p, uint32_t *pvmask,
	struct asic_nonce_record *out)
{
	uint32_t tmp32;

	/* preamble[0]/[1]/[2] are byte-swapped from wire order; nonce
	 * itself is reassembled little-endian from raw bytes p[0..3]. */
	out->nonce2   = bswap_32(preamble[0]);
	out->job_id   = bswap_32(preamble[1]);
	tmp32 = bswap_32(preamble[2]);
	out->asic_id  = tmp32;
	out->miner_id = tmp32 >> 16;
	out->nonce    = ((p[0] << 0) | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
	tmp32 = ((p[4] << 0) | (p[5] << 8) | (p[6] << 16) | (p[7] << 24));
	out->mid_id = a3197s_find_version_mask_index(pvmask, tmp32);
	out->ntime  = p[8];
	out->valid  = 1;

	if (g_worklog_fp) {
		fprintf(g_worklog_fp,
			"NONCE channel=%u chip=%u raw_nonce_buf=[0x%08x,0x%08x,0x%08x] "
			"raw_p_bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x "
			"decoded nonce2=0x%08x job_id=0x%08x asic_id=%u miner_id=%u "
			"nonce=0x%08x vmask_match_word=0x%08x mid_id=%d ntime=0x%02x\n",
			channel, asic_id,
			preamble[0], preamble[1], preamble[2],
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9],
			out->nonce2, out->job_id, out->asic_id, out->miner_id,
			out->nonce, tmp32, out->mid_id, out->ntime);
		fflush(g_worklog_fp);
	}
}

static int a3197s_receive_nonces(uint8_t channel,uint16_t asic_id, struct asic_nonce_record *nonce, uint8_t *nonce_count, uint32_t *pvmask)
{
	uint8_t  *p = NULL;
	uint32_t i, nonce_num = 0;
	uint32_t nonce_buf[NONCE_BUF_FIFO_LEN];
	uint32_t tmp_len = NONCE_BUF_FIFO_LEN;
	int ret = 0;

	/* 1. receive frame */
	ret = a3197s_nonce_receive_frame(channel, asic_id, nonce_buf, &tmp_len);

	/* 2. validate frame: ret==2 means nothing came back (chip has no
	 * new nonce yet) -- not an error. ret==1 means a frame started
	 * arriving but failed validation. */
	if(ret == 2)
	{
		g_nonce_timeout[channel]++;
		g_nonce_timeout_chip[channel][asic_id]++;
		return 0;
	}
	if(ret)
	{
		g_nonce_read_err[channel]++;
		goto ERR_RET;
	}

	/* 3. determine record count: distinguish a 4-word heartbeat frame
	 * from a >=7-word data frame and compute the record count from
	 * the response's byte length. */
	if (tmp_len < 7)
	{
		if (tmp_len != 4)
		{
			g_nonce_bad_len[channel]++;
			goto ERR_RET;
		}
		g_nonce_heartbeat[channel]++;
		g_nonce_heartbeat_chip[channel][asic_id]++;
		return 0;
	}
	g_nonce_data_chip[channel][asic_id]++;

	nonce_num = (tmp_len * 4 - 16) / 10;
	*nonce_count  = 0;

	p = (uint8_t *)&nonce_buf[4];

	/* 4-6. decode metadata / decode each nonce record (map version
	 * mask happens inside a3197s_nonce_decode_record()). */
	for (i = 0; i < nonce_num; )
	{
		a3197s_nonce_decode_record(channel, asic_id, nonce_buf, p, pvmask, &nonce[i]);

		i++;
		if ((i >= NONCE_RECORD_MAX) && (nonce_num > NONCE_RECORD_MAX))
		{
			/* nonce_num exceeding NONCE_RECORD_MAX means the response's
			 * reported length field is corrupt; discard what was parsed. */
			g_nonce_overflow[channel]++;
			*nonce_count = i;
			goto ERR_RET;
		}
		p += 10;
	}

	/* 7. return parsed records */
	*nonce_count = nonce_num;
	return 0;

ERR_RET:
	return 1;
}

int asic_get_nonce(uint8_t channel, uint16_t asic_id,struct asic_nonce_record *nonce, uint8_t *nonce_count)
{
	int ret = 0;
	ret = a3197s_receive_nonces(channel,asic_id, nonce, nonce_count, g_work_vmask[channel]);
	if(ret)
		a3197s_soft_reset(channel);

	return ret;
}

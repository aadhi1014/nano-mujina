/*
 * A3197S job construction/submission: stages the pending job set by
 * a3197s_set_job(), diffs and rewrites the version-rolling vmask table
 * when it changes, translates the job into the wire work frame (via
 * bitcoin_build_nonce2_job()'s merkle-root fixup), and writes it to
 * REG_WORK.
 */

#include <stdint.h>
#include <stdio.h>
#include "util.h"
#include "asic_engine.h"
#include "asic_internal.h"

static a3197s_job_t g_active_job;
uint32_t g_work_vmask[HBOARD_COUNT][8];

static int asic_send_work(uint8_t channel, struct asic_work_frame *work, uint32_t len, uint8_t block)
{
	(void)(block);
	a3197s_write_fifo(channel, REG_WORK, (uint32_t *)work, len >> 2, UART_WRITE_MODE);

	return 0;
}

void a3197s_set_job(const a3197s_job_t *job)
{
	g_active_job = *job;
}

void a3197s_submit_job(uint8_t miner_id, uint16_t asic_id, uint8_t block)
{
	struct asic_work_frame t_work;
	uint32_t i;
	uint8_t vmask_changed = 0;
	/* Logs only on job_id change or a vmask write, not every call, since
	 * this function runs once per nonce2 increment. Tracked per chain. */
	static uint32_t s_worklog_last_job_id[HBOARD_COUNT];
	static uint8_t  s_worklog_last_job_id_valid[HBOARD_COUNT];

	for (i = 0; i < 8; i++)
	{
		if (g_active_job.vmask[i] != g_work_vmask[miner_id][i])
		{
			g_work_vmask[miner_id][i] = g_active_job.vmask[i];
			a3197s_select_chip(miner_id, CHIP_ADDR_BROADCAST);
			a3197s_set_reg(miner_id, REG_VMASK_TABLE + i * 4, g_work_vmask[miner_id][i]);
			a3197s_select_chip(miner_id, asic_id);
			vmask_changed = 1;
		}
	}

	bitcoin_build_nonce2_job(&g_active_job, g_active_job.nonce2);

	for (i = 0; i < 15; i++)
		t_work.work[14 - i] = *((uint32_t *)&g_active_job.header[4 + 4 * i]);

	for (i = 0; i < 3; i++)
		t_work.data[i] = *((uint32_t *)&g_active_job.header[64 + 4 * i]);

	t_work.note[0] = bswap_32(g_active_job.nonce2);
	t_work.note[1] = bswap_32(g_active_job.job_id);
	t_work.note[2] = bswap_32((miner_id << 16) | asic_id);

	/* note[3] is a fixed protocol constant embedded in the work burst,
	 * identical across all chips; explicitly assigned since t_work is an
	 * uninitialized stack struct. */
	t_work.note[3] = 0x0003E5C0;

	t_work.init = 0;

	if (g_worklog_fp &&
	    (vmask_changed ||
	     !s_worklog_last_job_id_valid[miner_id] ||
	     s_worklog_last_job_id[miner_id] != g_active_job.job_id))
	{
		s_worklog_last_job_id[miner_id] = g_active_job.job_id;
		s_worklog_last_job_id_valid[miner_id] = 1;

		fprintf(g_worklog_fp,
			"WORK miner=%u asic=%u job_id=0x%08x nonce2=0x%08x vmask_changed=%u "
			"vmask=[0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x]\n",
			miner_id, asic_id, g_active_job.job_id, g_active_job.nonce2, vmask_changed,
			g_active_job.vmask[0], g_active_job.vmask[1], g_active_job.vmask[2], g_active_job.vmask[3],
			g_active_job.vmask[4], g_active_job.vmask[5], g_active_job.vmask[6], g_active_job.vmask[7]);
		fprintf(g_worklog_fp, "WORK header[0:80)=");
		for (i = 0; i < 80; i++)
			fprintf(g_worklog_fp, "%02x", g_active_job.header[i]);
		fprintf(g_worklog_fp, "\n");
		/* coinbase_len is a size_t; cast to unsigned long for portable printf. */
		fprintf(g_worklog_fp, "WORK coinbase_len=%lu nonce2_offset=%d nonce2_size=%d "
			"merkle_offset=%d nmerkles=%d coinbase=",
			(unsigned long)g_active_job.coinbase_len, g_active_job.nonce2_offset,
			g_active_job.nonce2_size, g_active_job.merkle_offset, g_active_job.nmerkles);
		for (i = 0; i < g_active_job.coinbase_len && i < 512; i++)
			fprintf(g_worklog_fp, "%02x", g_active_job.coinbase[i]);
		fprintf(g_worklog_fp, "%s\n", g_active_job.coinbase_len > 512 ? "...(truncated)" : "");
		if (g_active_job.nmerkles > 0) {
			int m;

			fprintf(g_worklog_fp, "WORK merkles[0..%d]=", g_active_job.nmerkles - 1);
			for (m = 0; m < g_active_job.nmerkles && m < AVA_P_MERKLES_COUNT; m++) {
				int b;

				for (b = 0; b < 32; b++)
					fprintf(g_worklog_fp, "%02x", g_active_job.merkles[m][b]);
				fprintf(g_worklog_fp, " ");
			}
			fprintf(g_worklog_fp, "\n");
		}
		fprintf(g_worklog_fp, "WORK t_work.work[0..14]=");
		for (i = 0; i < 15; i++)
			fprintf(g_worklog_fp, "%08x ", t_work.work[i]);
		fprintf(g_worklog_fp, "\n");
		fprintf(g_worklog_fp, "WORK t_work.data[0..2]=");
		for (i = 0; i < 3; i++)
			fprintf(g_worklog_fp, "%08x ", t_work.data[i]);
		fprintf(g_worklog_fp,
			"\nWORK t_work.note=[0x%08x,0x%08x,0x%08x,0x%08x]\n",
			t_work.note[0], t_work.note[1], t_work.note[2], t_work.note[3]);
		fflush(g_worklog_fp);
	}

	asic_send_work(miner_id, &t_work, sizeof(struct asic_work_frame), block);
	g_active_job.nonce2++;
}

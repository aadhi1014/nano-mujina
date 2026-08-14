/*
 * A3197S UART command framing: builds and parses the fixed 5-byte
 * preamble, 5-byte command word, and 5-byte payload words used to talk
 * to the ASIC chips over UART (chip select, register read/write, baud
 * change, chip-last signaling).
 *
 * ---- Observed A3197S wire protocol facts ----
 *   - Every frame starts with a 5-byte preamble: five 0x5A sync bytes.
 *   - The preamble is followed by a 5-byte "frame header" (command
 *     word): chip_id/mode/reg_addr/word_count packed into 4 bytes plus
 *     a trailing CRC-8 byte -- see a3197s_encode_command()'s doc comment
 *     for the exact bit layout.
 *   - Payload words (when present) each occupy 5 bytes: 4 little-endian
 *     data bytes + 1 CRC-8 byte -- see a3197s_encode_payload_word().
 *   - CRC-8: polynomial 0xD5, init 0, no reflection, no final XOR,
 *     processed over the 4 preceding bytes in reverse order -- see
 *     a3197s_crc8().
 *   - Four command modes share the same header shape: READ (0),
 *     WRITE (1), BAUD (2, repurposes the reg_addr field to carry a UART
 *     clock divisor), INIT (3, used both for chain-wide enumeration and
 *     for chip-last signaling, which repurposes reg_addr to carry a
 *     flag bit).
 *   - Chip-last behavior: the last chip in a chain is told to stop
 *     repeating frames to a (nonexistent) next chip via an INIT-mode
 *     command with reg_addr = value<<13 -- see a3197s_send_chiplast_cmd().
 */

#include <string.h>
#include <time.h>
#include <sys/select.h>
#include "util.h"
#include "uart_transport.h"
#include "chip_link.h"

#define WORD_STRIDE       5   /* 4 payload bytes + 1 CRC byte */
#define HEADER_LEN        5   /* preamble */
#define COMMAND_LEN       5   /* chip/mode/addr/count + CRC */
#define FRAME_PREFIX_LEN  (HEADER_LEN + COMMAND_LEN)

static const uint8_t frame_preamble[HEADER_LEN] = { 0x5a, 0x5a, 0x5a, 0x5a, 0x5a };

static volatile uint32_t active_chip = 0;

#define CRC8_POLY 0xD5

static uint8_t crc8_table[256];
static int crc8_table_ready = 0;

static void crc8_table_build(void)
{
	unsigned i, bit;

	for (i = 0; i < 256; i++) {
		uint8_t c = (uint8_t)i;
		for (bit = 0; bit < 8; bit++)
			c = (c & 0x80) ? (uint8_t)((c << 1) ^ CRC8_POLY) : (uint8_t)(c << 1);
		crc8_table[i] = c;
	}
	crc8_table_ready = 1;
}

/* CRC-8, poly 0xD5, init 0, no reflection, no final XOR, processed over
 * `count` bytes in reverse order: bytes[count-1] down to bytes[0]. */
static uint8_t a3197s_crc8(const uint8_t *bytes, uint16_t count)
{
	uint8_t crc = 0;

	if (!crc8_table_ready)
		crc8_table_build();

	while (count--)
		crc = crc8_table[crc ^ bytes[count]];

	return crc;
}

/* Builds the 5-byte command word / "frame header" (chip_id/mode/reg_addr/
 * word_count + CRC):
 *   byte[0] = chip_id & 0xFF
 *   byte[1] = ((reg_addr & 0x3C) << 2) | (mode << 2) | ((chip_id >> 8) & 0x3)
 *   byte[2] = reg_addr >> 6
 *   byte[3] = word_count
 *   byte[4] = a3197s_crc8(byte[0..3])
 * `out` must have room for COMMAND_LEN bytes. */
static void a3197s_encode_command(uint8_t *out, uint16_t chip_id, uint8_t mode,
                                uint32_t reg_addr, uint8_t word_count)
{
	out[0] = chip_id & 0xff;
	out[1] = (uint8_t)(((reg_addr & 0x3c) << 2) | (mode << 2) | ((chip_id >> 8) & 0x3));
	out[2] = (uint8_t)(reg_addr >> 6);
	out[3] = word_count;
	out[4] = a3197s_crc8(out, 4);
}

/* Packs one 32-bit little-endian payload word + its CRC into `out`
 * (5 bytes). */
static void a3197s_encode_payload_word(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value >>  0);
	out[1] = (uint8_t)(value >>  8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
	out[4] = a3197s_crc8(out, 4);
}

static void a3197s_encode_preamble(uint8_t *out)
{
	memcpy(out, frame_preamble, HEADER_LEN);
}

void a3197s_set_active_chip(uint8_t channel, uint16_t chip_addr)
{
	(void)(channel);

	active_chip = chip_addr;
}

uint16_t a3197s_get_active_chip(void)
{
	return active_chip;
}

/* Optional read/write wire trace, off by default (NULL); enable via
 * uart_set_wiretrace_log() to log every a3197s_send_frame() call and
 * a3197s_recv_frame_ex() outcome (channel, chip, addr, mode, ret
 * code) to the given file. */
static FILE *wiretrace_fp = NULL;
void uart_set_wiretrace_log(FILE *fp)
{
	wiretrace_fp = fp;
}

int a3197s_send_frame(uint8_t channel, uint32_t reg_addr, uint32_t *data_send, uint32_t len, enum uart_mode mode)
{
	uint8_t tx_buf[UART_TX_BUFFER_SIZE];
	uint32_t i;

	if (wiretrace_fp) {
		fprintf(wiretrace_fp, "WR channel=%u chip=%u addr=0x%x mode=%u len=%u word0=0x%08x\n",
			channel, active_chip, reg_addr, (unsigned)mode, len,
			(data_send && len) ? data_send[0] : 0);
		fflush(wiretrace_fp);
	}

	if (len == 0)
		return 0;

	a3197s_encode_preamble(&tx_buf[0]);

	if (mode == UART_INIT_MODE) {
		/* Chain-wide broadcast/init commands use a fixed FF FF FF FF
		 * command word instead of the chip/mode/addr encoding. */
		tx_buf[5] = 0xff;
		tx_buf[6] = 0xff;
		tx_buf[7] = 0xff;
		tx_buf[8] = 0xff;
		tx_buf[9] = a3197s_crc8(&tx_buf[5], 4);
	} else {
		a3197s_encode_command(&tx_buf[5], (uint16_t)active_chip, (uint8_t)mode, reg_addr, (uint8_t)len);
	}

	if (data_send) {
		for (i = 0; i < len; i++)
			a3197s_encode_payload_word(&tx_buf[FRAME_PREFIX_LEN + WORD_STRIDE * i], data_send[i]);

		if (mode == UART_INIT_MODE) {
			uart_write(channel, &tx_buf[0], FRAME_PREFIX_LEN);
			delay_ms(1);
			uart_write(channel, &tx_buf[FRAME_PREFIX_LEN], len * WORD_STRIDE);
		} else {
			uart_write(channel, &tx_buf[0], len * WORD_STRIDE + FRAME_PREFIX_LEN);
		}
	} else {
		uart_write(channel, &tx_buf[0], FRAME_PREFIX_LEN);
	}

	return 0;
}

/* Step: transport receive -- byte-level sync-hunt/read loop. Blocks
 * (subject to READ_HARD_DEADLINE_MS and SELECT_MAX_RETRY) until a full
 * frame has arrived: it re-syncs on the 5x0x5A preamble as bytes trickle
 * in, validates the command-word CRC once enough of the header is
 * present, negotiates the actual payload word count against *plen
 * (honoring check_len), and keeps reading until exactly that many bytes
 * are in rx_buf. Read-path return codes: 0 = full valid frame received,
 * 1 = a transport/validation failure (frame started arriving but failed
 * CRC/length/addressing checks), 2 = nothing came back at all
 * (pos==0). rx_buf must have room for UART_RX_BUFFER_SIZE bytes. */
static int a3197s_transport_receive(uint8_t channel, uint8_t rx_buf[UART_RX_BUFFER_SIZE],
                            uint32_t *plen, int check_len)
{
#define SELECT_TIMEOUT_US   2000
#define SELECT_MAX_RETRY    3

	uint8_t crc, len_negotiated = 0;
	int rd_len, sync_offset;
	int count, pos = 0;
	int attempt = 0;
	int fd;
	fd_set read_fds;
	int ret;
	struct timeval timeout;
	struct timespec deadline_start;

	rd_len = FRAME_PREFIX_LEN;

	/* Hard wall-clock deadline on top of the retry/reset logic below:
	 * this function cannot block longer than READ_HARD_DEADLINE_MS
	 * regardless of how the byte stream trickles in. */
#define READ_HARD_DEADLINE_MS 100
	clock_gettime(CLOCK_MONOTONIC, &deadline_start);

	while (1) {
		struct timespec now;
		long elapsed_ms;

		clock_gettime(CLOCK_MONOTONIC, &now);
		elapsed_ms = (now.tv_sec - deadline_start.tv_sec) * 1000L +
			(now.tv_nsec - deadline_start.tv_nsec) / 1000000L;
		if (elapsed_ms >= READ_HARD_DEADLINE_MS)
			return (pos == 0) ? 2 : 1;

		fd = uart_get_fd(channel);
		FD_ZERO(&read_fds);
		FD_SET(fd, &read_fds);
		timeout.tv_sec = 0;
		timeout.tv_usec = SELECT_TIMEOUT_US;
		ret = select(fd + 1, &read_fds, NULL, NULL, &timeout);
		if (ret <= 0) {
			attempt++;
			if (attempt < SELECT_MAX_RETRY)
				continue;
			return (pos == 0) ? 2 : 1;
		}

		count = uart_read(channel, rx_buf + pos, rd_len - pos);
		if (count <= 0)
			return (pos == 0) ? 2 : 1;

		attempt = 0;
		pos += count;

		if (pos < FRAME_PREFIX_LEN)
			continue;

		for (sync_offset = 0; sync_offset <= (pos - HEADER_LEN); sync_offset++) {
			if (!memcmp(rx_buf + sync_offset, frame_preamble, HEADER_LEN))
				break;
		}
		if (sync_offset != 0) {
			memmove(rx_buf, rx_buf + sync_offset, pos - sync_offset);
			pos -= sync_offset;

			if (pos < FRAME_PREFIX_LEN)
				continue;
		}

		if (!len_negotiated) {
			crc = a3197s_crc8(&rx_buf[HEADER_LEN], 4);
			if (crc != rx_buf[HEADER_LEN + 4])
				return 1;

			if (rx_buf[HEADER_LEN + 1] == 0xff) {
				*plen = 1;
			} else if ((rx_buf[HEADER_LEN + 3]) != *plen) {
				if (check_len)
					return 1;
				*plen = rx_buf[HEADER_LEN + 3];
			}
			rd_len = FRAME_PREFIX_LEN + (*plen) * WORD_STRIDE;
			if (rd_len > UART_RX_BUFFER_SIZE)
				return 1;
			len_negotiated = 1;
		}

		if (pos == rd_len)
			break;
	}

	return 0;
}

/* Step: decode response -- given a fully-received frame in rx_buf
 * (exactly FRAME_PREFIX_LEN + plen*WORD_STRIDE bytes, per
 * a3197s_transport_receive()): for a READ_MODE response, validates the
 * responding chip id and echoed register address against *chipid/
 * *reg_addr (0x3ff/0xffffffff meaning "don't care", used for broadcast
 * reads), rejecting WRITE_MODE responses outright; then decodes each
 * payload word with its own per-word CRC check into data_recv. */
static int a3197s_decode_response(const uint8_t *rx_buf, uint32_t *chipid, uint32_t *reg_addr,
                            uint32_t *data_recv, uint32_t plen)
{
	uint32_t word;
	uint8_t crc;
	int i;

	word = (rx_buf[HEADER_LEN + 1] >> 2) & 0x3;
	if (word == UART_READ_MODE) {
		word = rx_buf[HEADER_LEN] + ((rx_buf[HEADER_LEN + 1] & 0x3) << 8);
		if ((word != *chipid) && (*chipid != 0x3ff) && (*chipid != 0xffffffff))
			return 1;
		*chipid = word;

		word = ((rx_buf[HEADER_LEN + 2] & 0xff) << 6) | ((rx_buf[HEADER_LEN + 1] & 0xf0) >> 2);
		if ((word != *reg_addr) && (*reg_addr != 0xffffffff))
			return 1;

		*reg_addr = word;
	} else if (word == UART_WRITE_MODE) {
		return 1;
	}

	{
		int offset = FRAME_PREFIX_LEN;
		for (i = 0; i < plen; i++) {
			crc = a3197s_crc8(&rx_buf[offset], 4);
			if (crc != rx_buf[offset + 4])
				return 1;

			data_recv[i] = ((uint32_t)rx_buf[offset + 0] <<  0) |
			               ((uint32_t)rx_buf[offset + 1] <<  8) |
			               ((uint32_t)rx_buf[offset + 2] << 16) |
			               ((uint32_t)rx_buf[offset + 3] << 24);
			offset += WORD_STRIDE;
		}
	}

	return 0;
}

static int uart_read_fifo_with_ret_impl(uint8_t channel, uint32_t *chipid, uint32_t *reg_addr,
                            uint32_t *data_recv, uint32_t *plen, int check_len)
{
	uint8_t rx_buf[UART_RX_BUFFER_SIZE] = {0};
	int ret;

	if (data_recv)
		memset(data_recv, 0, (*plen) * sizeof(uint32_t));
	else
		return 1;

	ret = a3197s_transport_receive(channel, rx_buf, plen, check_len);
	if (ret)
		return ret;

	return a3197s_decode_response(rx_buf, chipid, reg_addr, data_recv, *plen);
}

/* Tracing wrapper around the implementation above: logs the requested
 * chip/addr (captured before the call, since chipid/reg_addr are in/out
 * params overwritten on success) along with the return code and, on
 * success, the actual chip/addr/first word received. */
int a3197s_recv_frame_ex(uint8_t channel, uint32_t *chipid, uint32_t *reg_addr,
                            uint32_t *data_recv, uint32_t *plen, int check_len)
{
	uint32_t req_chipid = 0, req_addr = 0;
	int ret;

	if (wiretrace_fp) {
		req_chipid = chipid ? *chipid : 0;
		req_addr = reg_addr ? *reg_addr : 0;
	}

	ret = uart_read_fifo_with_ret_impl(channel, chipid, reg_addr, data_recv, plen, check_len);

	if (wiretrace_fp) {
		fprintf(wiretrace_fp, "RD channel=%u req_chip=%u req_addr=0x%x ret=%d",
			channel, req_chipid, req_addr, ret);
		if (ret == 0) {
			fprintf(wiretrace_fp, " got_chip=%u got_addr=0x%x word0=0x%08x",
				chipid ? *chipid : 0, reg_addr ? *reg_addr : 0,
				(data_recv && plen && *plen) ? data_recv[0] : 0);
		}
		fprintf(wiretrace_fp, "\n");
		fflush(wiretrace_fp);
	}

	return ret;
}

int a3197s_recv_frame(uint8_t channel, uint32_t reg_addr, uint32_t *data_recv, uint32_t len)
{
	uint32_t chipid = active_chip;
	uint32_t rd_reg_addr = reg_addr;
	uint32_t rd_len = len;
	return a3197s_recv_frame_ex(channel, &chipid, &rd_reg_addr, data_recv, &rd_len, 1);
}

/* UART speed-change command: mode 2, with byte[2] (normally reg_addr>>6)
 * repurposed to carry the divisor `m` directly. Passing reg_addr = m<<6
 * into the general command-word builder produces byte[2]==m while
 * leaving byte[1]'s reg_addr contribution at 0. */
void a3197s_send_baud_cmd(uint8_t channel, uint32_t baud)
{
	uint16_t m, n;
	uint8_t tx_buf[UART_TX_BUFFER_SIZE];

	/* round up */
	m = 24000000 / baud;
	n = 24000000 % baud;
	if (n)
		m += 1;

	a3197s_encode_preamble(&tx_buf[0]);
	a3197s_encode_command(&tx_buf[5], (uint16_t)active_chip, UART_BAUD_MODE, (uint32_t)m << 6, 0);

	uart_write(channel, &tx_buf[0], FRAME_PREFIX_LEN);
}

/* Chip-last signaling: mode 3 (INIT) with reg_addr = value<<13. Feeding
 * that reg_addr through the general command-word builder produces
 * byte[2] = (value<<13)>>6 = value<<7, with byte[1]'s reg_addr
 * contribution always zero. */
void a3197s_send_chiplast_cmd(uint8_t channel, uint8_t value)
{
	uint8_t tx_buf[UART_RX_BUFFER_SIZE];

	a3197s_encode_preamble(&tx_buf[0]);
	a3197s_encode_command(&tx_buf[5], (uint16_t)active_chip, UART_INIT_MODE, (uint32_t)value << 13, 0);

	uart_write(channel, tx_buf, FRAME_PREFIX_LEN);
}

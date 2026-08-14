#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pin_ctrl.h"
#include "ipc_link.h"
#include "ipc_protocol.h"
#include "ipc_job_adapter.h"
#include "k_comm_ipcmsg.h" /* K_IPCMSG_MAX_CONTENT_LEN, for sizing rbuf */
#include "pvt_sensor.h"
#include "hash_bridge.h"
#include "asic_engine.h"
#include "chip_link.h"
#include "util.h"

#define SHAREFS_HELLO_LOG "/sharefs/hello.log"
#define SHAREFS_ENUM_LOG "/sharefs/enum.log"
#define SHAREFS_PVT_LOG "/sharefs/pvt.log"
#define SHAREFS_PLLPVT_LOG "/sharefs/pllpvt.log"
#define SHAREFS_RSTTEST_LOG "/sharefs/rsttest.log"
#define SHAREFS_WDOG_LOG "/sharefs/wdog.log"
#define SHAREFS_SHATEST_LOG "/sharefs/shatest.log"
#define SHAREFS_WORK4_LOG "/sharefs/work4.log"
#define STAGE1_UART_CHANNEL 2
#define CHANNEL_PROBE_TIMEOUT_SEC 3

/*
 * Stops the RTOS kernel watchdog (/dev/watchdog1). Per-stage bounded
 * timeouts and hard-exit watchdog threads act as the safety net instead.
 */
#define WDT_DEVICE_NAME "/dev/watchdog1"
#define CTRL_WDT_GET_TIMEOUT  _IOW('W', 1, int)
#define CTRL_WDT_GET_TIMELEFT _IOW('W', 3, int)
#define CTRL_WDT_STOP         _IOW('W', 6, int)

static void disable_kernel_watchdog(void)
{
	FILE *fp = fopen(SHAREFS_WDOG_LOG, "a");
	int fd = open(WDT_DEVICE_NAME, O_WRONLY);
	int rc;
	uint32_t timeleft = 0xffffffff;

	if (fd < 0) {
		if (fp) {
			fprintf(fp, "disable_kernel_watchdog: open(%s) failed: %s -- watchdog may still "
				"be armed, kernel could still take over after 179s\n",
				WDT_DEVICE_NAME, strerror(errno));
			fclose(fp);
		}
		return;
	}

	ioctl(fd, CTRL_WDT_GET_TIMELEFT, &timeleft);
	rc = ioctl(fd, CTRL_WDT_STOP, NULL);

	if (fp) {
		fprintf(fp, "disable_kernel_watchdog: opened %s ok, timeleft_before_stop=%u, "
			"CTRL_WDT_STOP rc=%d (0 = stopped successfully)\n", WDT_DEVICE_NAME, timeleft, rc);
		fclose(fp);
	}

	close(fd);
}

static int append_timestamped_line(const char *path, const char *line)
{
	FILE *fp;
	time_t now;
	struct tm tm_now;
	char ts[32];

	fp = fopen(path, "a");
	if (!fp)
		return -1;

	now = time(NULL);
	if (localtime_r(&now, &tm_now))
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);
	else
		snprintf(ts, sizeof(ts), "time-unavailable");

	fprintf(fp, "%s %s\n", ts, line);
	fclose(fp);
	return 0;
}

#define SHAREFS_SAFEIDLE_LOG "/sharefs/safeidle.log"

/* Asserts RST (GPIO31, holds the ASIC chain in reset) and exits.
 * gpio_init() must run first since it opens the gpio_fd used by
 * gpio_asic_rst_init()/gpio_asic_rst_assert(); the outcome is logged
 * to SHAREFS_SAFEIDLE_LOG. */
static int run_stage_safeidle(void)
{
	int rc;

	gpio_init(HBOARD_COUNT);
	rc = gpio_asic_rst_init();
	if (rc != 0) {
		append_timestamped_line(SHAREFS_SAFEIDLE_LOG,
			"safeidle: gpio_asic_rst_init FAILED (gpio_fd not open) -- RST NOT asserted");
		return 1;
	}
	gpio_asic_rst_assert();
	append_timestamped_line(SHAREFS_SAFEIDLE_LOG,
		"safeidle: gpio_init+rst_init OK, RST asserted (chain held in reset)");
	return 0;
}

static int run_stage0(void)
{
	printf("rtos_core Stage 0 hello\n");
	if (append_timestamped_line(SHAREFS_HELLO_LOG, "rtos_core Stage 0 hello") != 0) {
		fprintf(stderr, "failed to write %s: %s\n", SHAREFS_HELLO_LOG, strerror(errno));
		return 1;
	}
	return 0;
}

/*
 * Each channel's probe runs in its own thread with a pthread_cleanup
 * handler that reverts the channel to WORK mode, whether the thread
 * finishes normally or is cancelled after a timeout -- open()/read()/
 * select() are POSIX cancellation points. The caller bounds the wait
 * with a semaphore deadline and force-cancels the thread on timeout.
 */
struct channel_ctx {
	uint8_t channel;
	uint32_t chip_count;
	uint32_t errcnt_before;
	uint32_t errcnt_after;
	char chipid[8];
	sem_t done_sem;
};

static void revert_work_mode_cleanup(void *arg)
{
	uint8_t channel = *(uint8_t *)arg;
	ENTER_WORK_MODE(channel);
}

static void *channel_probe_worker(void *arg)
{
	struct channel_ctx *ctx = arg;
	const char *chipid;

	pthread_cleanup_push(revert_work_mode_cleanup, &ctx->channel);

	/* Pulse RST before init on every channel probed. The return value
	 * of gpio_asic_rst_init() isn't checked here so a RST-access
	 * failure doesn't block probing the other channels. */
	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(ctx->channel, UART_DEFAULT_BAUD_RATE);

	ENTER_CONFIG_MODE(ctx->channel);
	delay_ms(3);

	a3197s_select_chip(ctx->channel, CHIP_ADDR_BROADCAST);
	asic_init(ctx->channel);

	ctx->errcnt_before = a3197s_get_errcnt(ctx->channel);
	ctx->chip_count = a3197s_enumerate(ctx->channel);
	ctx->errcnt_after = a3197s_get_errcnt(ctx->channel);

	chipid = a3197s_get_chipid(ctx->channel);
	if (chipid) {
		strncpy(ctx->chipid, chipid, sizeof(ctx->chipid) - 1);
		ctx->chipid[sizeof(ctx->chipid) - 1] = '\0';
	}

	/* execute=1: reverts to WORK mode on normal completion too. */
	pthread_cleanup_pop(1);

	/* Signal completion after cleanup has run, so the caller never
	 * observes "done" before the relay has been reverted. */
	sem_post(&ctx->done_sem);
	return NULL;
}

/*
 * Runs one channel's probe with a hard time bound.
 *
 * On success (return 0): *out is a heap-allocated, populated ctx that
 * the caller owns -- sem_destroy(&ctx->done_sem) and free(ctx) when done.
 *
 * On timeout (return 1): *out is NULL and the allocated ctx is left
 * untouched (not freed), since the worker thread may still be running
 * and could still write to it.
 */
static int try_enum_channel_bounded(uint8_t channel, struct channel_ctx **out)
{
	struct channel_ctx *ctx;
	pthread_t tid;
	struct timespec deadline;
	int rc;

	*out = NULL;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	ctx->channel = channel;
	sem_init(&ctx->done_sem, 0, 0);

	if (pthread_create(&tid, NULL, channel_probe_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += CHANNEL_PROBE_TIMEOUT_SEC;

	/* sem_timedwait bounds the wait regardless of what the worker
	 * thread is doing. */
	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		/* Timed out -- cancel the worker. open()/read()/select()
		 * are POSIX cancellation points, so cancellation should
		 * land inside whatever blocking call it's stuck in and run
		 * the cleanup handler on the way out. pthread_detach()
		 * (not pthread_join()) since join() would itself block if
		 * cancellation doesn't land. */
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	/* Worker already posted, so this join just reaps the thread.
	 * Ownership of ctx passes to the caller from here. */
	pthread_join(tid, NULL);
	*out = ctx;
	return 0;
}

#define HARD_SAFETY_DEADLINE_SEC (4 * CHANNEL_PROBE_TIMEOUT_SEC + 10)

/* The RTOS kernel watchdog (/dev/watchdog1) has a 179-second timeout;
 * if it expires, the kernel launches asic_miner_e directly.
 * HARD_SAFETY_DEADLINE_SEC must stay well under this value. */
#define RTOS_KERNEL_WATCHDOG_TIMEOUT_SEC 179

#if HARD_SAFETY_DEADLINE_SEC >= (RTOS_KERNEL_WATCHDOG_TIMEOUT_SEC - 30)
#error "HARD_SAFETY_DEADLINE_SEC is too close to the RTOS kernel's 179s watchdog " \
       "-- leave real margin for boot/IPC startup overhead we don't control."
#endif

/*
 * Backstop thread: polls g_stage1_complete once per second for up to
 * HARD_SAFETY_DEADLINE_SEC and returns early if it's set. Otherwise,
 * once the deadline is reached, forces every relay back to WORK mode
 * and calls _exit() to terminate the process. _exit() skips
 * atexit()/stdio-flush so termination isn't delayed by them.
 */
static volatile int g_stage1_complete = 0;

/* Set by --predelaysec in main(). */
static int g_predelay_sec = 0;

static void *hard_safety_watchdog(void *unused)
{
	(void)unused;
	int waited_sec;
	for (waited_sec = 0; waited_sec < HARD_SAFETY_DEADLINE_SEC; waited_sec++) {
		if (g_stage1_complete)
			return NULL;
		delay_ms(1000);
	}
	if (g_stage1_complete)
		return NULL;
	gpio_asic_rst_deassert();
	for (uint8_t ch = 0; ch < 4; ch++)
		ENTER_WORK_MODE(ch);
	_exit(3);
	return NULL;
}

static int run_stage1_enum(void)
{
	FILE *fp;
	int any_success = 0;
	pthread_t watchdog_tid;

	fp = fopen(SHAREFS_ENUM_LOG, "a");
	if (!fp) {
		fprintf(stderr, "failed to write %s: %s\n", SHAREFS_ENUM_LOG, strerror(errno));
		return 1;
	}

	fprintf(fp, "stage1 begin: sweeping all 4 uart channels (0-3), %ds timeout per channel, "
		"%ds hard safety backstop\n", CHANNEL_PROBE_TIMEOUT_SEC, HARD_SAFETY_DEADLINE_SEC);
	fflush(fp);
	gpio_init(HBOARD_COUNT);

	if (pthread_create(&watchdog_tid, NULL, hard_safety_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);
	else
		fprintf(fp, "WARNING: hard safety watchdog thread failed to start\n");

	for (uint8_t ch = 0; ch < 4; ch++) {
		struct channel_ctx *ctx = NULL;

		if (try_enum_channel_bounded(ch, &ctx) != 0) {
			fprintf(fp, "channel=%u TIMEOUT after %ds -- worker cancelled (may still be running; "
				"context intentionally leaked rather than risk a use-after-free), relay will be "
				"forced back to WORK mode by the %ds hard safety watchdog regardless\n",
				ch, CHANNEL_PROBE_TIMEOUT_SEC, HARD_SAFETY_DEADLINE_SEC);
			fflush(fp);
			continue;
		}

		fprintf(fp, "channel=%u chip_family=%s chip_count=%u errcnt_before=%u errcnt_after=%u\n",
			ch, ctx->chipid, ctx->chip_count, ctx->errcnt_before, ctx->errcnt_after);
		fflush(fp);

		if (ctx->chip_count > 0) {
			any_success = 1;
			for (uint32_t i = 0; i < ctx->chip_count; i++)
				fprintf(fp, "channel=%u chip_id=%u\n", ch, i);
			fflush(fp);
		}

		sem_destroy(&ctx->done_sem);
		free(ctx);
	}

	fprintf(fp, "stage1 sweep complete. any_success=%d\n", any_success);
	fclose(fp);
	/* Tells hard_safety_watchdog() this stage is done. */
	g_stage1_complete = 1;
	return any_success ? 0 : 2;
}

/*
 * Stage 2: reads PVT (temperature/voltage) telemetry only, no work
 * submission. Channel 2 is the live 12-chip A3197S chain.
 *
 * PVT reads (pvt_*_update(), built on a3197s_get_reg()/a3197s_set_reg())
 * stay in WORK mode and never touch the CONFIG/WORK relay, so a
 * timeout here has no relay state to restore.
 */
#define STAGE2_UART_CHANNEL 2
#define STAGE2_CHIP_COUNT 12
#define STAGE2_TIMEOUT_SEC 5

struct pvt_sweep_result {
	double temp_c[STAGE2_CHIP_COUNT];
	double volt_mv[STAGE2_CHIP_COUNT];
	int temp_attempts[STAGE2_CHIP_COUNT];
	int volt_attempts[STAGE2_CHIP_COUNT];
};

struct pvt_sweep_ctx {
	struct pvt_sweep_result result;
	sem_t done_sem;
};

static void *pvt_sweep_worker(void *arg)
{
	struct pvt_sweep_ctx *ctx = arg;

	/* Pulse RST before init. */
	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE2_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);
	a3197s_select_chip(STAGE2_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	asic_init(STAGE2_UART_CHANNEL);
	a3197s_pvt_init(STAGE2_UART_CHANNEL, CHIP_ADDR_BROADCAST);

	for (uint16_t chip = 0; chip < STAGE2_CHIP_COUNT; chip++) {
		pvt_tcore_update(STAGE2_UART_CHANNEL, chip);
		pvt_vcore_update(STAGE2_UART_CHANNEL, chip);
		ctx->result.temp_c[chip] = pvt_tcore_get(STAGE2_UART_CHANNEL, chip);
		ctx->result.volt_mv[chip] = pvt_vcore_get(STAGE2_UART_CHANNEL, chip);
	}

	sem_post(&ctx->done_sem);
	return NULL;
}

/* On timeout, ctx is intentionally never freed because the worker may
 * still be alive and writing to it. */
static int pvt_sweep_bounded(struct pvt_sweep_ctx **out)
{
	struct pvt_sweep_ctx *ctx;
	pthread_t tid;
	struct timespec deadline;
	int rc;

	*out = NULL;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	if (pthread_create(&tid, NULL, pvt_sweep_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE2_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		/* Detach, never join, so a wedged worker can't block the
		 * caller's forward progress. */
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	*out = ctx;
	return 0;
}

static void *stage2_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE2_TIMEOUT_SEC + 10) * 1000);
	/* Backstop: deassert RST and force the channel back to WORK mode. */
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE2_UART_CHANNEL);
	_exit(3);
	return NULL;
}

static int run_stage2_pvt(void)
{
	FILE *fp;
	struct pvt_sweep_ctx *ctx = NULL;
	pthread_t watchdog_tid;
	int ret;

	fp = fopen(SHAREFS_PVT_LOG, "a");
	if (!fp) {
		fprintf(stderr, "failed to write %s: %s\n", SHAREFS_PVT_LOG, strerror(errno));
		return 1;
	}

	fprintf(fp, "stage2 begin: pvt read on channel=%u, %u chips, %ds timeout\n",
		STAGE2_UART_CHANNEL, STAGE2_CHIP_COUNT, STAGE2_TIMEOUT_SEC);
	fflush(fp);

	/* Direct host register access requires CONFIG mode. */
	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE2_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage2_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);
	else
		fprintf(fp, "WARNING: hard safety watchdog thread failed to start\n");

	if (pvt_sweep_bounded(&ctx) != 0) {
		fprintf(fp, "stage2 TIMEOUT after %ds -- pvt sweep worker cancelled (may still be running; "
			"context intentionally leaked)\n", STAGE2_TIMEOUT_SEC);
		fclose(fp);
		return 2;
	}

	for (int chip = 0; chip < STAGE2_CHIP_COUNT; chip++) {
		fprintf(fp, "chip=%d temp_c=%.2f volt_mv=%.2f\n",
			chip, ctx->result.temp_c[chip], ctx->result.volt_mv[chip]);
	}
	fflush(fp);

	sem_destroy(&ctx->done_sem);
	free(ctx);

	ENTER_WORK_MODE(STAGE2_UART_CHANNEL);

	fprintf(fp, "stage2 sweep complete\n");
	fclose(fp);
	ret = 0;
	return ret;
}

/*
 * Stage 2b: starts the PLL via a3197s_init_chain() (writes the default PLL
 * frequency through a3197s_configure_regs()), then reads PVT telemetry
 * with retries, under the same bounded-timeout / hard-watchdog /
 * CONFIG-mode handling as Stage 2.
 */
#define STAGE2B_TIMEOUT_SEC 60
#define PVT_RETRY_MAX 100
#define PVT_RETRY_DELAY_MS 30

/* Sentinel value returned by temp_decode() for a not-yet-ready reading. */
#define PVT_TEMP_INVALID -273.0

static void *pll_start_and_pvt_worker(void *arg)
{
	struct pvt_sweep_ctx *ctx = arg;

	/* Pulse RST before init. */
	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE2_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);
	a3197s_init_chain(STAGE2_UART_CHANNEL);

	/* Allow the PLL to lock before reading sensors off of it. */
	delay_ms(100);

	/* Retries each PVT read with a delay between attempts, tracking
	 * how many attempts each read took. */
	for (uint16_t chip = 0; chip < STAGE2_CHIP_COUNT; chip++) {
		int attempt;

		for (attempt = 1; attempt <= PVT_RETRY_MAX; attempt++) {
			pvt_tcore_update(STAGE2_UART_CHANNEL, chip);
			ctx->result.temp_c[chip] = pvt_tcore_get(STAGE2_UART_CHANNEL, chip);
			if (ctx->result.temp_c[chip] != PVT_TEMP_INVALID)
				break;
			delay_ms(PVT_RETRY_DELAY_MS);
		}
		ctx->result.temp_attempts[chip] = attempt;

		for (attempt = 1; attempt <= PVT_RETRY_MAX; attempt++) {
			pvt_vcore_update(STAGE2_UART_CHANNEL, chip);
			ctx->result.volt_mv[chip] = pvt_vcore_get(STAGE2_UART_CHANNEL, chip);
			if (ctx->result.volt_mv[chip] != 0.0)
				break;
			delay_ms(PVT_RETRY_DELAY_MS);
		}
		ctx->result.volt_attempts[chip] = attempt;
	}

	sem_post(&ctx->done_sem);
	return NULL;
}

static int pll_start_and_pvt_bounded(struct pvt_sweep_ctx **out)
{
	struct pvt_sweep_ctx *ctx;
	pthread_t tid;
	struct timespec deadline;
	int rc;

	*out = NULL;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	if (pthread_create(&tid, NULL, pll_start_and_pvt_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE2B_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	*out = ctx;
	return 0;
}

static void *stage2b_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE2B_TIMEOUT_SEC + 10) * 1000);
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE2_UART_CHANNEL);
	_exit(3);
	return NULL;
}

static int run_stage2b_startpll_pvt(void)
{
	FILE *fp;
	struct pvt_sweep_ctx *ctx = NULL;
	pthread_t watchdog_tid;

	fp = fopen(SHAREFS_PLLPVT_LOG, "a");
	if (!fp) {
		fprintf(stderr, "failed to write %s: %s\n", SHAREFS_PLLPVT_LOG, strerror(errno));
		return 1;
	}

	fprintf(fp, "stage2b begin: a3197s_init_chain (default PLL, decodes to 100MHz) then pvt read, "
		"channel=%u, %u chips, %ds timeout\n", STAGE2_UART_CHANNEL, STAGE2_CHIP_COUNT, STAGE2B_TIMEOUT_SEC);
	fflush(fp);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE2_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage2b_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);
	else
		fprintf(fp, "WARNING: hard safety watchdog thread failed to start\n");

	if (pll_start_and_pvt_bounded(&ctx) != 0) {
		fprintf(fp, "stage2b TIMEOUT after %ds -- worker cancelled (may still be running; "
			"context intentionally leaked)\n", STAGE2B_TIMEOUT_SEC);
		fclose(fp);
		return 2;
	}

	for (int chip = 0; chip < STAGE2_CHIP_COUNT; chip++) {
		fprintf(fp, "chip=%d temp_c=%.2f (attempts=%d) volt_mv=%.2f (attempts=%d)\n",
			chip, ctx->result.temp_c[chip], ctx->result.temp_attempts[chip],
			ctx->result.volt_mv[chip], ctx->result.volt_attempts[chip]);
	}
	fflush(fp);

	sem_destroy(&ctx->done_sem);
	free(ctx);

	ENTER_WORK_MODE(STAGE2_UART_CHANNEL);

	fprintf(fp, "stage2b sweep complete\n");
	fclose(fp);
	return 0;
}

/*
 * Stage 3: pulses the ASIC chain reset (GPIO31) before the same
 * init+PLL+PVT sequence as Stage 2b.
 */
#define STAGE3_TIMEOUT_SEC 90

static void *stage3_worker(void *arg)
{
	struct pvt_sweep_ctx *ctx = arg;

	/* Reset pulse: deassert, assert, deassert. */
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100); /* let chips finish post-reset boot */

	uart_init(STAGE2_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);
	a3197s_init_chain(STAGE2_UART_CHANNEL);
	delay_ms(100);

	for (uint16_t chip = 0; chip < STAGE2_CHIP_COUNT; chip++) {
		int attempt;

		for (attempt = 1; attempt <= PVT_RETRY_MAX; attempt++) {
			pvt_tcore_update(STAGE2_UART_CHANNEL, chip);
			ctx->result.temp_c[chip] = pvt_tcore_get(STAGE2_UART_CHANNEL, chip);
			if (ctx->result.temp_c[chip] != PVT_TEMP_INVALID)
				break;
			delay_ms(PVT_RETRY_DELAY_MS);
		}
		ctx->result.temp_attempts[chip] = attempt;

		for (attempt = 1; attempt <= PVT_RETRY_MAX; attempt++) {
			pvt_vcore_update(STAGE2_UART_CHANNEL, chip);
			ctx->result.volt_mv[chip] = pvt_vcore_get(STAGE2_UART_CHANNEL, chip);
			if (ctx->result.volt_mv[chip] != 0.0)
				break;
			delay_ms(PVT_RETRY_DELAY_MS);
		}
		ctx->result.volt_attempts[chip] = attempt;
	}

	sem_post(&ctx->done_sem);
	return NULL;
}

static int stage3_bounded(struct pvt_sweep_ctx **out)
{
	struct pvt_sweep_ctx *ctx;
	pthread_t tid;
	struct timespec deadline;
	int rc;

	*out = NULL;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	if (pthread_create(&tid, NULL, stage3_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE3_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	*out = ctx;
	return 0;
}

static void *stage3_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE3_TIMEOUT_SEC + 10) * 1000);
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE2_UART_CHANNEL);
	_exit(3);
	return NULL;
}

static int run_stage3_reset_test(void)
{
	FILE *fp;
	struct pvt_sweep_ctx *ctx = NULL;
	pthread_t watchdog_tid;
	int rst_init_rc;
	int rst_pin_before;

	fp = fopen(SHAREFS_RSTTEST_LOG, "a");
	if (!fp) {
		fprintf(stderr, "failed to write %s: %s\n", SHAREFS_RSTTEST_LOG, strerror(errno));
		return 1;
	}

	fprintf(fp, "stage3 begin: explicit RST pulse (GPIO31) then init+PLL+PVT, channel=%u, %ds timeout\n",
		STAGE2_UART_CHANNEL, STAGE3_TIMEOUT_SEC);
	fflush(fp);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE2_UART_CHANNEL);
	delay_ms(3);

	rst_init_rc = gpio_asic_rst_init();
	fprintf(fp, "gpio_asic_rst_init() rc=%d (0/positive = ioctl succeeded, pin 31 reachable via /dev/gpio; "
		"negative = it did NOT, this whole test is void)\n", rst_init_rc);
	fflush(fp);
	if (rst_init_rc < 0) {
		fprintf(fp, "stage3 aborted: cannot control GPIO31 via this interface -- would need direct "
			"/dev/mem MMIO to Bank A instead, not attempted\n");
		fclose(fp);
		return 3;
	}

	rst_pin_before = gpio_read_pin(ASIC_RST_PIN);
	fprintf(fp, "gpio31 read before touching it: %d (expect 0/LOW if stock asic_miner left it "
		"deasserted as documented; -1 = read not supported by this driver)\n", rst_pin_before);
	fflush(fp);

	if (pthread_create(&watchdog_tid, NULL, stage3_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);
	else
		fprintf(fp, "WARNING: hard safety watchdog thread failed to start\n");

	if (stage3_bounded(&ctx) != 0) {
		fprintf(fp, "stage3 TIMEOUT after %ds -- worker cancelled (may still be running; "
			"context intentionally leaked)\n", STAGE3_TIMEOUT_SEC);
		fclose(fp);
		return 2;
	}

	for (int chip = 0; chip < STAGE2_CHIP_COUNT; chip++) {
		fprintf(fp, "chip=%d temp_c=%.2f (attempts=%d) volt_mv=%.2f (attempts=%d)\n",
			chip, ctx->result.temp_c[chip], ctx->result.temp_attempts[chip],
			ctx->result.volt_mv[chip], ctx->result.volt_attempts[chip]);
	}
	fflush(fp);

	sem_destroy(&ctx->done_sem);
	free(ctx);

	ENTER_WORK_MODE(STAGE2_UART_CHANNEL);

	fprintf(fp, "stage3 sweep complete\n");
	fclose(fp);
	return 0;
}

/*
 * Stage 4: work generation / nonce round-trip test.
 *   1. Builds one dummy a3197s_job_t and loads it via a3197s_set_job().
 *   2. Writes an all-ones target to REG_TARGET_LO/HI so any nonce
 *      passes the chip's local pre-filter.
 *   3. Broadcasts it with a3197s_submit_job().
 *   4. Writes REG_NONCE_UPDATE twice and polls asic_get_nonce() per chip.
 *   5. Checks each returned struct asic_nonce_record: job_id must match
 *      the byte-swapped job id sent, and valid must be set.
 */
#define STAGE4_UART_CHANNEL STAGE2_UART_CHANNEL
#define STAGE4_CHIP_COUNT STAGE2_CHIP_COUNT
#define STAGE4_TIMEOUT_SEC 90
#define STAGE4_POLL_SEC 20
#define STAGE4_MAX_NONCES 64
#define STAGE4_DUMMY_JOB_ID 0xC0FFEE01u
#define STAGE4_COINBASE_LEN 64
#define STAGE4_MERKLE_OFFSET 36
#define STAGE4_HEADER_LOG_BYTES 80 /* version+prevhash+merkle+ntime+nbits */

struct stage4_nonce_rec {
	uint32_t job_id;
	uint32_t nonce2;
	uint32_t nonce;
	uint16_t asic_id;
	uint8_t miner_id;
	uint8_t ntime;
	uint8_t mid_id;
	uint8_t valid;
};

struct stage4_result {
	struct stage4_nonce_rec nonces[STAGE4_MAX_NONCES];
	int nonce_count;
	int job_id_match_count;
	uint8_t header_snapshot[128];
	/* Diagnostic counters for the nonce-poll loop below. */
	int reads_attempted;
	int reads_err;
	int reads_ok_empty;
	int reads_ok_data;
};

struct stage4_ctx {
	struct stage4_result result;
	sem_t done_sem;
};

static void *stage4_worker(void *arg)
{
	struct stage4_ctx *ctx = arg;
	a3197s_job_t job;
	struct asic_nonce_record nonces[NONCE_RECORD_MAX];
	uint8_t nonce_count;
	time_t poll_deadline;
	uint32_t expected_wire_job_id = bswap_32(STAGE4_DUMMY_JOB_ID);

	/* Reset pulse: deassert, assert, deassert. */
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE4_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);
	a3197s_init_chain(STAGE4_UART_CHANNEL);
	delay_ms(100);

	memset(&job, 0, sizeof(job));
	job.job_id = STAGE4_DUMMY_JOB_ID;
	job.coinbase_len = STAGE4_COINBASE_LEN;
	memset(job.coinbase, 0x42, STAGE4_COINBASE_LEN);
	job.nonce2 = 0;
	job.nonce2_offset = 4;
	job.nonce2_size = 4;
	job.merkle_offset = STAGE4_MERKLE_OFFSET;
	job.nmerkles = 0; /* merkle root reduces to sha256d(coinbase) */
	job.header[0] = 0x01; /* version (not part of the wire payload) */
	memset(job.header + 4, 0xAA, 32); /* dummy prevhash */
	*(uint32_t *)(job.header + 68) = 0x5f5e1000; /* dummy ntime */
	*(uint32_t *)(job.header + 72) = 0x1d00ffff; /* dummy nbits */
	job.work_restart = 1;

	a3197s_set_job(&job);
	a3197s_select_chip(STAGE4_UART_CHANNEL, CHIP_ADDR_BROADCAST);

	a3197s_set_reg(STAGE4_UART_CHANNEL, REG_TARGET_LO, 0xffffffff);
	a3197s_set_reg(STAGE4_UART_CHANNEL, REG_TARGET_HI, 0xffffffff);

	memcpy(ctx->result.header_snapshot, job.header, sizeof(job.header));

	/* Broadcasts the job, invoking bitcoin_build_nonce2_job() ->
	 * compute_double_sha256() against the attached chain. */
	a3197s_submit_job(STAGE4_UART_CHANNEL, CHIP_ADDR_BROADCAST, 0);

	/* Recomputes the same merkle hash independently for logging, since
	 * g_active_job is private to asic_engine.c. Mirrors
	 * bitcoin_build_nonce2_job()'s own steps: overwrite
	 * coinbase[nonce2_offset:nonce2_offset+nonce2_size) with nonce2,
	 * hash, then byte-swap each 32-bit word. */
	{
		unsigned char merkle[32];
		int w;

		memset(job.coinbase + job.nonce2_offset, 0, job.nonce2_size); /* nonce2 == 0 */
		compute_double_sha256(job.coinbase, merkle, STAGE4_COINBASE_LEN);
		for (w = 0; w < 8; w++) {
			uint8_t t0 = merkle[w * 4 + 0];
			uint8_t t1 = merkle[w * 4 + 1];
			merkle[w * 4 + 0] = merkle[w * 4 + 3];
			merkle[w * 4 + 1] = merkle[w * 4 + 2];
			merkle[w * 4 + 2] = t1;
			merkle[w * 4 + 3] = t0;
		}
		memcpy(ctx->result.header_snapshot + STAGE4_MERKLE_OFFSET, merkle, 32);
	}

	/* REG_NONCE_UPDATE is written twice per the main mining loop. */
	a3197s_set_reg(STAGE4_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	a3197s_set_reg(STAGE4_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);

	ctx->result.nonce_count = 0;
	ctx->result.job_id_match_count = 0;
	ctx->result.reads_attempted = 0;
	ctx->result.reads_err = 0;
	ctx->result.reads_ok_empty = 0;
	ctx->result.reads_ok_data = 0;

	poll_deadline = time(NULL) + STAGE4_POLL_SEC;
	while (time(NULL) < poll_deadline && ctx->result.nonce_count < STAGE4_MAX_NONCES) {
		uint16_t chip;

		for (chip = 0; chip < STAGE4_CHIP_COUNT && ctx->result.nonce_count < STAGE4_MAX_NONCES; chip++) {
			uint8_t n;
			int get_ret;

			/* Must select the chip before reading its nonce buffer,
			 * since asic_get_nonce()'s response validation requires
			 * the reply's chip id to equal `chip`. */
			a3197s_select_chip(STAGE4_UART_CHANNEL, chip);

			nonce_count = 0;
			ctx->result.reads_attempted++;
			get_ret = asic_get_nonce(STAGE4_UART_CHANNEL, chip, nonces, &nonce_count);
			if (get_ret != 0) {
				/* asic_get_nonce() calls a3197s_soft_reset() on any
				 * nonzero return -- a high reads_err count means the
				 * chain is being reset every iteration, which alone
				 * would suppress nonces regardless of whether the
				 * cores are actually hashing. */
				ctx->result.reads_err++;
				continue;
			}
			if (nonce_count == 0) {
				ctx->result.reads_ok_empty++;
				continue;
			}
			ctx->result.reads_ok_data++;

			for (n = 0; n < nonce_count && ctx->result.nonce_count < STAGE4_MAX_NONCES; n++) {
				struct stage4_nonce_rec *rec = &ctx->result.nonces[ctx->result.nonce_count++];

				rec->job_id = nonces[n].job_id;
				rec->nonce2 = nonces[n].nonce2;
				rec->nonce = nonces[n].nonce;
				rec->asic_id = nonces[n].asic_id;
				rec->miner_id = nonces[n].miner_id;
				rec->ntime = nonces[n].ntime;
				rec->mid_id = nonces[n].mid_id;
				rec->valid = nonces[n].valid;

				if (rec->valid && rec->job_id == expected_wire_job_id)
					ctx->result.job_id_match_count++;
			}
		}
		delay_ms(50);
	}

	sem_post(&ctx->done_sem);
	return NULL;
}

static int stage4_bounded(struct stage4_ctx **out)
{
	struct stage4_ctx *ctx;
	pthread_t tid;
	struct timespec deadline;
	int rc;

	*out = NULL;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	if (pthread_create(&tid, NULL, stage4_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE4_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		/* Detach, never join, never free -- the worker may still be
		 * running and writing to ctx. */
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	*out = ctx;
	return 0;
}

static void *stage4_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE4_TIMEOUT_SEC + 10) * 1000);
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE4_UART_CHANNEL);
	_exit(3);
	return NULL;
}

static int run_stage4_work(void)
{
	FILE *fp;
	struct stage4_ctx *ctx = NULL;
	pthread_t watchdog_tid;
	int i;
	int matched;

	fp = fopen(SHAREFS_WORK4_LOG, "a");
	if (!fp) {
		fprintf(stderr, "failed to write %s: %s\n", SHAREFS_WORK4_LOG, strerror(errno));
		return 1;
	}

	fprintf(fp, "stage4 begin: dummy job (job_id=0x%08x) -> broadcast work packet -> poll nonces, "
		"channel=%u, %ds timeout (%ds poll window), max (trivial) target\n",
		STAGE4_DUMMY_JOB_ID, STAGE4_UART_CHANNEL, STAGE4_TIMEOUT_SEC, STAGE4_POLL_SEC);
	fflush(fp);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE4_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage4_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);
	else
		fprintf(fp, "WARNING: hard safety watchdog thread failed to start\n");

	if (stage4_bounded(&ctx) != 0) {
		fprintf(fp, "stage4 TIMEOUT after %ds -- worker cancelled (may still be running; "
			"context intentionally leaked)\n", STAGE4_TIMEOUT_SEC);
		fclose(fp);
		return 2;
	}

	fprintf(fp, "queued header, offset %d..%d (independently-recomputed merkle root at %d): ",
		0, STAGE4_HEADER_LOG_BYTES - 1, STAGE4_MERKLE_OFFSET);
	for (i = 0; i < STAGE4_HEADER_LOG_BYTES; i++)
		fprintf(fp, "%02x", ctx->result.header_snapshot[i]);
	fprintf(fp, "\n");

	fprintf(fp, "nonce-buffer reads: attempted=%d err(soft-reset)=%d ok_empty=%d ok_data=%d\n",
		ctx->result.reads_attempted, ctx->result.reads_err,
		ctx->result.reads_ok_empty, ctx->result.reads_ok_data);
	fprintf(fp, "nonces received: %d (job_id-matching+valid: %d)\n",
		ctx->result.nonce_count, ctx->result.job_id_match_count);
	for (i = 0; i < ctx->result.nonce_count; i++) {
		struct stage4_nonce_rec *rec = &ctx->result.nonces[i];

		fprintf(fp, "  nonce[%d] job_id=0x%08x nonce2=0x%08x nonce=0x%08x asic_id=%u miner_id=%u "
			"ntime=%u mid_id=%u valid=%u%s\n",
			i, rec->job_id, rec->nonce2, rec->nonce, rec->asic_id, rec->miner_id,
			rec->ntime, rec->mid_id, rec->valid,
			(rec->valid && rec->job_id == bswap_32(STAGE4_DUMMY_JOB_ID)) ? " [MATCH]" : "");
	}
	fflush(fp);

	matched = ctx->result.job_id_match_count;

	sem_destroy(&ctx->done_sem);
	free(ctx);

	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE4_UART_CHANNEL);

	fprintf(fp, "stage4 complete: %s (job_id_match_count=%d)\n",
		matched > 0 ? "PASS -- work/nonce wire round-trip confirmed" : "FAIL -- no matching nonce came back",
		matched);
	fclose(fp);

	return matched > 0 ? 0 : 4;
}

/*
 * SHA test: verifies compute_double_sha256() (a pure-software SHA-256d
 * implementation) against a known test vector, bounded by a timeout.
 */
#define SHATEST_TIMEOUT_SEC 5

struct shatest_ctx {
	unsigned char digest[32];
	sem_t done_sem;
};

static void *shatest_worker(void *arg)
{
	struct shatest_ctx *ctx = arg;
	const unsigned char msg[] = "abc";

	compute_double_sha256((unsigned char *)msg, ctx->digest, 3);

	sem_post(&ctx->done_sem);
	return NULL;
}

static int run_shatest(void)
{
	FILE *fp;
	struct shatest_ctx *ctx;
	pthread_t tid;
	struct timespec deadline;
	int rc;
	/* sha256(sha256("abc")) */
	static const unsigned char expected[32] = {
		0x4f, 0x8b, 0x42, 0xc2, 0x2d, 0xd3, 0x72, 0x9b,
		0x51, 0x9b, 0xa6, 0xf6, 0x8d, 0x2d, 0xa7, 0xcc,
		0x5b, 0x2d, 0x60, 0x6d, 0x05, 0xda, 0xed, 0x5a,
		0xd5, 0x12, 0x8c, 0xc0, 0x3e, 0x6c, 0x63, 0x58,
	};

	fp = fopen(SHAREFS_SHATEST_LOG, "a");
	if (!fp) {
		fprintf(stderr, "failed to write %s: %s\n", SHAREFS_SHATEST_LOG, strerror(errno));
		return 1;
	}

	fprintf(fp, "shatest begin: compute_double_sha256(\"abc\"), pure software, %ds timeout\n", SHATEST_TIMEOUT_SEC);
	fflush(fp);

	ctx = malloc(sizeof(*ctx));
	if (!ctx) {
		fprintf(fp, "shatest: malloc failed\n");
		fclose(fp);
		return 1;
	}
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	if (pthread_create(&tid, NULL, shatest_worker, ctx) != 0) {
		fprintf(fp, "shatest: pthread_create failed\n");
		sem_destroy(&ctx->done_sem);
		free(ctx);
		fclose(fp);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += SHATEST_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		fprintf(fp, "shatest TIMEOUT after %ds -- pure-software compute_double_sha256() "
			"never returned (unexpected; this path has no syscalls/ioctls). "
			"Worker cancelled (may still be running; context intentionally leaked).\n",
			SHATEST_TIMEOUT_SEC);
		pthread_cancel(tid);
		pthread_detach(tid);
		fclose(fp);
		return 2;
	}

	pthread_join(tid, NULL);

	fprintf(fp, "got:      ");
	for (int i = 0; i < 32; i++)
		fprintf(fp, "%02x", ctx->digest[i]);
	fprintf(fp, "\n");

	fprintf(fp, "expected: ");
	for (int i = 0; i < 32; i++)
		fprintf(fp, "%02x", expected[i]);
	fprintf(fp, "\n");

	int match = (memcmp(ctx->digest, expected, 32) == 0);
	fprintf(fp, "shatest %s\n", match ? "PASS" : "FAIL");

	sem_destroy(&ctx->done_sem);
	free(ctx);
	fclose(fp);
	return match ? 0 : 3;
}

/*
 * Stage 5: brings up the IPC link to Mujina (the /dev/ipcm protocol) against a
 * Linux-side test harness. Touches no GPIO/UART. Exercises the base
 * HELLO handshake and one IPC_MSG_SET_VOLTAGE round trip (which drives
 * the I2C-3 @ 0x48 DC/DC write; see IPC_DCDC_* in ipc_protocol.h for the
 * wire format).
 */
#define SHAREFS_IPC5_LOG "/sharefs/ipc5.log"
#define STAGE5_TIMEOUT_SEC 150
#define STAGE5_PORT 300
#define STAGE5_SERVICE_NAME "mujina_svc"
#define STAGE5_RECV_TIMEOUT_MS 5000
/* 3600mV = IPC_DCDC_BASE_MV, offset 0 -- a safe, well-understood test value
 * rather than an arbitrary number, so a PASS here doesn't depend on
 * whatever work mode happens to be active. */
#define STAGE5_TEST_TARGET_MV IPC_DCDC_BASE_MV

struct stage5_result {
	int connect_rc; /* 0 = ipc_link_open() succeeded, -1 = failed */
	/* ipc_link_send_sync() return: 0 = reply received, K_IPCMSG_ETIMEOUT
	 * (see sdk/k_comm_ipcmsg.h) = timed out, -1 = other failure. */
	int hello_rc;
	int32_t hello_ret_val;
	uint16_t hello_reply_len;
	unsigned char hello_reply_raw[16];
	int voltage_rc;
	int32_t voltage_ret_val;
	uint16_t voltage_reply_len;
	unsigned char voltage_reply_raw[16];
	struct ipc_status voltage_reply_status;
	int voltage_reply_status_valid;
};

struct stage5_ctx {
	struct stage5_result result;
	sem_t done_sem;
};

/* Fixed IPC target node for the Linux side. */
#define STAGE5_RTOS_TARGET 0

static void *stage5_worker(void *arg)
{
	struct stage5_ctx *ctx = arg;
	int id;
	struct ipc_set_voltage volt_req;
	uint16_t reply_len;
	/* Sized to K_IPCMSG_MAX_CONTENT_LEN, the wire-protocol ceiling; also
	 * used for the earlier HELLO reply on this same stack frame. */
	unsigned char rbuf[K_IPCMSG_MAX_CONTENT_LEN];

	/* Opens the service and blocks until the connect handshake
	 * completes. */
	id = ipc_link_open(STAGE5_SERVICE_NAME, STAGE5_RTOS_TARGET, STAGE5_PORT);
	ctx->result.connect_rc = (id >= 0) ? 0 : -1;

	if (id < 0) {
		sem_post(&ctx->done_sem);
		return NULL;
	}

	ctx->result.hello_rc = ipc_link_send_sync(id, IPC_MSG_HELLO, NULL, 0,
		rbuf, sizeof(rbuf), &reply_len, &ctx->result.hello_ret_val, STAGE5_RECV_TIMEOUT_MS);
	if (ctx->result.hello_rc == 0) {
		size_t n = reply_len < sizeof(ctx->result.hello_reply_raw) ? reply_len : sizeof(ctx->result.hello_reply_raw);

		ctx->result.hello_reply_len = reply_len;
		memcpy(ctx->result.hello_reply_raw, rbuf, n);
	}

	volt_req.target_mv = STAGE5_TEST_TARGET_MV;
	ctx->result.voltage_rc = ipc_link_send_sync(id, IPC_MSG_SET_VOLTAGE, &volt_req, sizeof(volt_req),
		rbuf, sizeof(rbuf), &reply_len, &ctx->result.voltage_ret_val, STAGE5_RECV_TIMEOUT_MS);
	if (ctx->result.voltage_rc == 0) {
		size_t n = reply_len < sizeof(ctx->result.voltage_reply_raw) ? reply_len : sizeof(ctx->result.voltage_reply_raw);

		ctx->result.voltage_reply_len = reply_len;
		memcpy(ctx->result.voltage_reply_raw, rbuf, n);
		if (reply_len == sizeof(struct ipc_status)) {
			memcpy(&ctx->result.voltage_reply_status, rbuf, sizeof(struct ipc_status));
			ctx->result.voltage_reply_status_valid = 1;
		}
	}

	ipc_link_close(id, STAGE5_SERVICE_NAME);
	sem_post(&ctx->done_sem);
	return NULL;
}

static int stage5_bounded(struct stage5_ctx **out)
{
	struct stage5_ctx *ctx;
	pthread_t tid;
	struct timespec deadline;
	int rc;

	*out = NULL;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	if (pthread_create(&tid, NULL, stage5_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE5_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	*out = ctx;
	return 0;
}

static void *stage5_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE5_TIMEOUT_SEC + 10) * 1000);
	/* No GPIO/UART state to revert -- this stage never touches either. */
	_exit(3);
	return NULL;
}

static int run_stage5_ipc(void)
{
	FILE *fp;
	struct stage5_ctx *ctx = NULL;
	pthread_t watchdog_tid;
	int pass;

	fp = fopen(SHAREFS_IPC5_LOG, "a");
	if (!fp) {
		fprintf(stderr, "failed to write %s: %s\n", SHAREFS_IPC5_LOG, strerror(errno));
		return 1;
	}

	fprintf(fp, "stage5 begin: IPC-link-to-Mujina bring-up vs throwaway Linux test harness, port=%d, %ds timeout\n",
		STAGE5_PORT, STAGE5_TIMEOUT_SEC);
	fflush(fp);

	if (pthread_create(&watchdog_tid, NULL, stage5_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);
	else
		fprintf(fp, "WARNING: hard safety watchdog thread failed to start\n");

	if (stage5_bounded(&ctx) != 0) {
		fprintf(fp, "stage5 TIMEOUT after %ds -- worker cancelled (may still be running; "
			"context intentionally leaked)\n", STAGE5_TIMEOUT_SEC);
		fclose(fp);
		return 2;
	}

	fprintf(fp, "service=%s target=%d port=%d\n", STAGE5_SERVICE_NAME, STAGE5_RTOS_TARGET, STAGE5_PORT);
	fprintf(fp, "connect (add_service+kd_ipcmsg_connect+run) rc=%d\n", ctx->result.connect_rc);
	if (ctx->result.connect_rc == 0) {
		fprintf(fp, "HELLO send_sync rc=%d ret_val=%d len=%u raw=",
			ctx->result.hello_rc, ctx->result.hello_ret_val, ctx->result.hello_reply_len);
		if (ctx->result.hello_rc == 0) {
			size_t n = ctx->result.hello_reply_len < sizeof(ctx->result.hello_reply_raw) ?
				ctx->result.hello_reply_len : sizeof(ctx->result.hello_reply_raw);
			size_t i;

			for (i = 0; i < n; i++)
				fprintf(fp, "%02x ", ctx->result.hello_reply_raw[i]);
		}
		fprintf(fp, "\n");

		fprintf(fp, "SET_VOLTAGE(%dmV) send_sync rc=%d ret_val=%d len=%u raw=",
			STAGE5_TEST_TARGET_MV, ctx->result.voltage_rc,
			ctx->result.voltage_ret_val, ctx->result.voltage_reply_len);
		if (ctx->result.voltage_rc == 0) {
			size_t n = ctx->result.voltage_reply_len < sizeof(ctx->result.voltage_reply_raw) ?
				ctx->result.voltage_reply_len : sizeof(ctx->result.voltage_reply_raw);
			size_t i;

			for (i = 0; i < n; i++)
				fprintf(fp, "%02x ", ctx->result.voltage_reply_raw[i]);
		}
		fprintf(fp, "\n");
		if (ctx->result.voltage_reply_status_valid) {
			struct ipc_status *st = &ctx->result.voltage_reply_status;

			fprintf(fp, "  mujina reports: asics_total=%u ghsmm=%u temp_avg=%.1f temp_max=%.1f voltage_mv=%u\n",
				st->asics_total, st->ghsmm, st->temp_avg, st->temp_max, st->voltage_mv);
		}
	}
	fflush(fp);

	pass = (ctx->result.connect_rc == 0 && ctx->result.hello_rc == 0);

	sem_destroy(&ctx->done_sem);
	free(ctx);

	fprintf(fp, "stage5 complete: %s\n", pass ? "PASS -- IPCM connect+HELLO round-trip confirmed" : "FAIL");
	fclose(fp);

	return pass ? 0 : 5;
}

/*
 * Stage 6: IPC-driven mining loop. Combines Stage 4's work-submission/
 * nonce-polling (now a persistent loop) with Stage 5's IPC transport,
 * and adds: IPC_MSG_JOB (mujina -> rtos, pushed unsolicited),
 * IPC_MSG_NONCE (rtos -> mujina, per found nonce), IPC_MSG_STATUS
 * (rtos -> mujina, periodic telemetry), IPC_MSG_SET_MODE (mujina ->
 * rtos, PLL frequency change).
 *
 * Runs the whole loop in CONFIG mode, since register access for both
 * work submission and PVT reads requires it.
 *
 * Threading: the IPC recv callback (stage6_recv_handler(), invoked from
 * ipc_link's kd_ipcmsg_run() thread) never calls asic_/UART functions
 * directly, since those aren't thread-safe and the main loop calls them
 * concurrently. The callback only copies data into ctx (mutex-protected)
 * and sets a pending flag; all register writes happen from
 * stage6_worker()'s own loop.
 */
#define STAGE6_UART_CHANNEL STAGE2_UART_CHANNEL
#define STAGE6_CHIP_COUNT STAGE2_CHIP_COUNT
/* Mining-loop duration after IPC connects. */
#define STAGE6_RUN_SEC 300
/* Safety backstop against connect() hanging forever. */
#define STAGE6_CONNECT_BUDGET_SEC 180
#define STAGE6_STARTUP_DELAY_SEC 0
/* Interval for rewriting REG_ROLLTIME (0x214) via asic_refresh_rolltime(). */
#define STAGE6_ROLLTIME_REFRESH_MS 1000
#define STAGE6_STATUS_INTERVAL_SEC 15
#define STAGE6_MAX_NONCES_LOGGED 32
/* ipc6.log is written continuously for the whole life of --ipc6loop
 * (STATUS lines every STAGE6_STATUS_INTERVAL_SEC, plus per-event lines
 * for job/mode/autotune changes) with no other rotation -- cap its size
 * so a long-running session can't fill /sharefs. */
#define IPC6_LOG_MAX_BYTES (2 * 1024 * 1024)

/* Truncates /sharefs/ipc6.log in place once it exceeds IPC6_LOG_MAX_BYTES.
 * Called from the STATUS interval, so checked at most once every
 * STAGE6_STATUS_INTERVAL_SEC.
 *
 * Deliberately never closes/reopens fp: stage6_recv_handler() writes to
 * this same FILE* from the IPC library's own callback thread, with no
 * lock around those fprintf() calls (matching the rest of this file's
 * existing, tolerated risk level for interleaved log lines) -- closing
 * the stream here could race a concurrent write into a use-after-close.
 * ftruncate() + fseek() truncate the underlying file and reset the
 * write position without ever invalidating the stream, so a concurrent
 * fprintf() from the other thread stays safe (worst case, one line
 * lands right at the truncation point and reads oddly -- never a
 * crash). Relies on every fprintf(ctx->fp, ...) call site pairing with
 * an immediate fflush(), already true throughout this file, so there's
 * no unflushed buffered data to land at the wrong offset afterward. */
static void ipc6_log_cap_size(FILE *fp)
{
	struct stat st;

	if (!fp)
		return;

	/* fstat(), not ftell(): live-tested on real hardware (2026-08-08)
	 * and ftell() on this musl build did not reflect the file's true
	 * on-disk size here, so the cap silently never fired. fstat()
	 * asks the kernel for the actual current size directly, with no
	 * dependency on the stream's own buffered/cached position. */
	if (fstat(fileno(fp), &st) != 0) {
		fprintf(fp, "ipc6_log_cap_size: fstat failed, errno=%d (%s)\n", errno, strerror(errno));
		fflush(fp);
		return;
	}
	if (st.st_size < IPC6_LOG_MAX_BYTES)
		return;

	if (ftruncate(fileno(fp), 0) == 0) {
		fseek(fp, 0, SEEK_SET);
		fprintf(fp, "ipc6.log truncated at %lld bytes (cap=%d)\n",
			(long long)st.st_size, IPC6_LOG_MAX_BYTES);
		fflush(fp);
	} else {
		/* Truncation failed -- can't write "at the start" of a file we
		 * couldn't shrink, so this note just appends normally instead,
		 * which itself demonstrates whether appends still work. */
		fprintf(fp, "ipc6_log_cap_size: ftruncate failed at %lld bytes, errno=%d (%s)\n",
			(long long)st.st_size, errno, strerror(errno));
		fflush(fp);
	}
}

/* Periodic frequency auto-tune: steps PLL frequency up or down based on
 * measured fail rate (spd_dh), holding voltage fixed. Frequency-only;
 * voltage is not adjusted by this loop. */
#define STAGE6_AUTOTUNE_ENABLED 0
#define STAGE6_AUTOTUNE_CHECK_MS (180 * 1000)
#define STAGE6_AUTOTUNE_STEP_MHZ 6
#define STAGE6_AUTOTUNE_SPD_DH_LOW 25.0  /* step up when fail rate is at/below this */
#define STAGE6_AUTOTUNE_SPD_DH_HIGH 40.0 /* step down when fail rate is at/above this */
#define STAGE6_AUTOTUNE_MIN_FREQ_MHZ 100
#define STAGE6_AUTOTUNE_MAX_FREQ_MHZ 280

struct stage6_result {
	int connect_rc;
	int jobs_received;
	int jobs_applied;
	int nonces_found;
	int nonces_sent;
	int status_sent;
	int set_mode_received;
	uint32_t last_job_id;
	uint32_t nonce_job_ids[STAGE6_MAX_NONCES_LOGGED];
	uint32_t nonce_values[STAGE6_MAX_NONCES_LOGGED];
};

struct stage6_ctx {
	struct stage6_result result;
	pthread_mutex_t lock; /* protects everything below, shared with stage6_recv_handler() */
	a3197s_job_t pending_job;
	int have_pending_job; /* 0 = none, 1 = applied already, 2 = new, apply on next loop tick */
	uint32_t pending_pll_freq[4];
	int have_pending_mode;
	uint32_t pending_rolltime_raw; /* IPC_MSG_SET_ROLLTIME_RAW override */
	int have_pending_rolltime_raw;
	int32_t pending_voltage_raw_mv; /* IPC_MSG_SET_VOLTAGE_RAW override */
	int have_pending_voltage_raw;
	int have_pending_spdlog_reset; /* IPC_MSG_RESET_SPDLOG_RAW request */
	int rolltime_raw_override_active; /* Set while a raw rolltime
					    * override is active; a new
					    * SET_MODE clears it. */
	uint32_t last_applied_pll_freq[4]; /* reported via IPC_MSG_STATUS */
	uint32_t last_applied_voltage_mv;  /* reported via IPC_MSG_STATUS */
	int ipc_id;
	FILE *fp;
	FILE *wiretrace_fp;
	sem_t done_sem; /* posted when the bounded run window ends (success or failure) */
	/* Set when a received cmd isn't IPC_MSG_JOB or IPC_MSG_SET_MODE,
	 * indicating the connected peer isn't speaking this protocol. */
	int wrong_peer;
	/* Set once CORE_RST+noncemask have been configured at least once. */
	int noncemask_done;
	/* pause_requested/resume_requested are set by stage6_recv_handler()
	 * and consumed by stage6_worker()'s loop; paused is the resulting
	 * state, reported via IPC_MSG_STATUS. */
	int pause_requested;
	int resume_requested;
	int paused;
};

/* Single active Stage 6 instance; ipc_link's recv-handler callback
 * signature carries no user-data pointer, so this is referenced via a
 * file-scope pointer instead. */
static struct stage6_ctx *g_stage6_ctx;

/* Persists across --ipc6loop cycle boundaries (unlike ctx->paused, which
 * resets each time stage6_bounded() allocates a fresh ctx). When set,
 * stage6_bounded() skips all hardware bring-up (RST pulse, enum, baud
 * switch, TARGETL/H, SET_VOLTAGE) on the next cycle and goes straight to
 * ctx->paused=1, so the chain stays idle across cycle boundaries for as
 * long as the pause lasts. Set/cleared by stage6_recv_handler() on
 * IPC_MSG_PAUSE/RESUME. */
static volatile int g_stage6_persist_paused;

/* stage6_worker()'s mining loop runs unbounded (for(;;)), exiting only on
 * a real condition (wrong_peer). Progress is tracked via
 * g_stage6_last_progress for the heartbeat watchdog below. */
static volatile time_t g_stage6_last_progress;
/* Stall bound for the heartbeat watchdog. */
#define STAGE6_STALL_SEC 900

static void stage6_recv_handler(uint16_t cmd, const void *body, uint16_t len)
{
	struct stage6_ctx *ctx = g_stage6_ctx;

	if (!ctx)
		return;

	if (cmd == IPC_MSG_JOB) {
		struct ipc_job job;

		if (ipc_job_unpack(body, len, &job) != 0) {
			fprintf(ctx->fp, "[recv] JOB unpack failed (len=%u)\n", (unsigned)len);
			fflush(ctx->fp);
			return;
		}

		pthread_mutex_lock(&ctx->lock);
		a3197s_job_from_ipc(&ctx->pending_job, &job);
		ctx->have_pending_job = 2;
		ctx->result.jobs_received++;
		ctx->result.last_job_id = job.job_id;
		pthread_mutex_unlock(&ctx->lock);

		fprintf(ctx->fp, "[recv] JOB job_id=0x%08x coinbase_len=%u nmerkles=%d\n",
			job.job_id, (unsigned)job.coinbase_len, job.nmerkles);
		fflush(ctx->fp);
	} else if (cmd == IPC_MSG_SET_MODE) {
		struct ipc_set_mode sm;

		if (len != sizeof(sm)) {
			fprintf(ctx->fp, "[recv] SET_MODE bad len=%u (want %u)\n",
				(unsigned)len, (unsigned)sizeof(sm));
			fflush(ctx->fp);
			return;
		}
		memcpy(&sm, body, sizeof(sm));

		pthread_mutex_lock(&ctx->lock);
		memcpy(ctx->pending_pll_freq, sm.pll_freq, sizeof(ctx->pending_pll_freq));
		ctx->have_pending_mode = 1;
		ctx->result.set_mode_received++;
		pthread_mutex_unlock(&ctx->lock);

		fprintf(ctx->fp, "[recv] SET_MODE pll_freq=[%u,%u,%u,%u] work_mode=%u\n",
			sm.pll_freq[0], sm.pll_freq[1], sm.pll_freq[2], sm.pll_freq[3],
			(unsigned)sm.work_mode);
		fflush(ctx->fp);
	} else if (cmd == IPC_MSG_PAUSE) {
		pthread_mutex_lock(&ctx->lock);
		ctx->pause_requested = 1;
		pthread_mutex_unlock(&ctx->lock);
		/* Survives past this cycle -- see g_stage6_persist_paused's
		 * declaration comment. */
		g_stage6_persist_paused = 1;
		fprintf(ctx->fp, "[recv] PAUSE\n");
		fflush(ctx->fp);
	} else if (cmd == IPC_MSG_RESUME) {
		pthread_mutex_lock(&ctx->lock);
		ctx->resume_requested = 1;
		pthread_mutex_unlock(&ctx->lock);
		g_stage6_persist_paused = 0;
		fprintf(ctx->fp, "[recv] RESUME\n");
		fflush(ctx->fp);
	} else if (cmd == IPC_MSG_SET_ROLLTIME_RAW) {
		struct ipc_set_rolltime_raw rt;

		if (len != sizeof(rt)) {
			fprintf(ctx->fp, "[recv] SET_ROLLTIME_RAW bad len=%u (want %u)\n",
				(unsigned)len, (unsigned)sizeof(rt));
			fflush(ctx->fp);
			return;
		}
		memcpy(&rt, body, sizeof(rt));

		pthread_mutex_lock(&ctx->lock);
		ctx->pending_rolltime_raw = rt.value;
		ctx->have_pending_rolltime_raw = 1;
		pthread_mutex_unlock(&ctx->lock);

		fprintf(ctx->fp, "[recv] SET_ROLLTIME_RAW value=%u\n", rt.value);
		fflush(ctx->fp);
	} else if (cmd == IPC_MSG_SET_VOLTAGE_RAW) {
		struct ipc_set_voltage_raw vr;

		if (len != sizeof(vr)) {
			fprintf(ctx->fp, "[recv] SET_VOLTAGE_RAW bad len=%u (want %u)\n",
				(unsigned)len, (unsigned)sizeof(vr));
			fflush(ctx->fp);
			return;
		}
		memcpy(&vr, body, sizeof(vr));

		pthread_mutex_lock(&ctx->lock);
		ctx->pending_voltage_raw_mv = vr.target_mv;
		ctx->have_pending_voltage_raw = 1;
		pthread_mutex_unlock(&ctx->lock);

		fprintf(ctx->fp, "[recv] SET_VOLTAGE_RAW target_mv=%d\n", vr.target_mv);
		fflush(ctx->fp);
	} else if (cmd == IPC_MSG_RESET_SPDLOG_RAW) {
		pthread_mutex_lock(&ctx->lock);
		ctx->have_pending_spdlog_reset = 1;
		pthread_mutex_unlock(&ctx->lock);

		fprintf(ctx->fp, "[recv] RESET_SPDLOG_RAW\n");
		fflush(ctx->fp);
	} else {
		pthread_mutex_lock(&ctx->lock);
		ctx->wrong_peer = 1;
		pthread_mutex_unlock(&ctx->lock);
		fprintf(ctx->fp, "[recv] unexpected cmd=%u len=%u -- likely connected to the real "
			"stock miner, not our test harness; aborting this attempt\n", (unsigned)cmd, (unsigned)len);
		fflush(ctx->fp);
	}
}

/* Re-brings-up the chain after IPC_MSG_PAUSE asserted RST and idled it;
 * called from stage6_worker() on IPC_MSG_RESUME. A smaller, self-contained
 * duplicate of stage6_bounded()'s bring-up sequence (RST pulse, enum,
 * a3197s_init_chain, baud switch, TARGET placeholder), skipping
 * gpio_init()/gpio_asic_rst_init()/ENTER_CONFIG_MODE (already done for
 * this process) and diagnostic probes. Returns the enumerated chip
 * count, falling back to STAGE6_CHIP_COUNT if out of range. */
static uint32_t stage6_resume_bringup(struct stage6_ctx *ctx)
{
	uint32_t chip_count = STAGE6_CHIP_COUNT;
	uint32_t reenum_count;

	fprintf(ctx->fp, "stage6: RESUME -- re-bringing up chain\n");
	fflush(ctx->fp);

	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE6_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);

	reenum_count = a3197s_enumerate(STAGE6_UART_CHANNEL);
	fprintf(ctx->fp, "stage6: RESUME re-enum chip_count=%u\n", reenum_count);
	fflush(ctx->fp);
	if (reenum_count > 0 && reenum_count <= STAGE6_CHIP_COUNT)
		chip_count = reenum_count;

	a3197s_init_chain(STAGE6_UART_CHANNEL);
	delay_ms(100);

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_uart_baud(STAGE6_UART_CHANNEL, UART_HIGH_BAUD_RATE);
	delay_ms(1000);

	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0xffffffff);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, 0xffffffff);

	/* Re-enum resets the chip's real PLL frequency to 100MHz; reset the
	 * software-tracked last_applied_pll_freq to match, so the next
	 * SET_MODE computes a real ramp instead of a zero-delta no-op in
	 * asic_ramp_pll_freq(). */
	ctx->last_applied_pll_freq[0] = 100;
	ctx->last_applied_pll_freq[1] = 100;
	ctx->last_applied_pll_freq[2] = 100;
	ctx->last_applied_pll_freq[3] = 100;

	fprintf(ctx->fp, "stage6: RESUME bring-up complete, chip_count=%u\n", chip_count);
	fflush(ctx->fp);

	return chip_count;
}

/* Monotonic milliseconds, for sub-second interval pacing. */
static int64_t monotonic_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *stage6_worker(void *arg)
{
	struct stage6_ctx *ctx = arg;
	time_t next_status_time;
	int64_t next_rolltime_refresh_ms;
	int64_t next_autotune_check_ms; /* see STAGE6_AUTOTUNE_CHECK_MS */
	int wiretrace_status_count = 0;
	uint16_t rr_chip = 0;
	int rr_active = 0;      /* 0 until a job has been applied at least once */
	int rr_nonce_armed = 0; /* 0 until the first full chip cycle completes */

	/* Hardware bring-up (RST, uart_init, enum, baud switch, TARGET
	 * writes, SET_VOLTAGE) happens in stage6_bounded() before this
	 * thread is spawned, after the IPC connect succeeds. This function
	 * starts directly at the steady-state mining loop, using
	 * ctx->ipc_id. */
	fprintf(ctx->fp, "stage6: entering mining loop, no fixed duration -- runs until a real "
		"error (wrong_peer) or the process exits (ipc_id=%d)\n", ctx->ipc_id);
	fflush(ctx->fp);

	g_stage6_last_progress = time(NULL);
	next_status_time = time(NULL); /* send one right away */
	next_rolltime_refresh_ms = monotonic_ms() + STAGE6_ROLLTIME_REFRESH_MS;
	next_autotune_check_ms = monotonic_ms() + STAGE6_AUTOTUNE_CHECK_MS;

	for (;;) {
		a3197s_job_t job_copy;
		uint32_t pll_copy[4];
		uint32_t rolltime_raw_copy = 0;
		int32_t voltage_raw_copy = 0;
		int have_new_job = 0, have_new_mode = 0, have_new_rolltime_raw = 0,
		    have_new_voltage_raw = 0, have_new_spdlog_reset = 0, wrong_peer;
		uint16_t chip;

		g_stage6_last_progress = time(NULL);

		pthread_mutex_lock(&ctx->lock);
		wrong_peer = ctx->wrong_peer;
		pthread_mutex_unlock(&ctx->lock);
		if (wrong_peer) {
			fprintf(ctx->fp, "stage6: aborting -- connected to the wrong peer (see [recv] "
				"line above), not running the full mining loop against it\n");
			fflush(ctx->fp);
			break;
		}

		/* Pause/resume, checked before anything else in the loop
		 * body. Does not `continue` immediately after applying a
		 * transition, so a pause and an already-queued resume in
		 * the same tick both take effect in order. */
		{
			int do_pause, do_resume, is_paused;

			pthread_mutex_lock(&ctx->lock);
			do_pause = ctx->pause_requested;
			ctx->pause_requested = 0;
			do_resume = ctx->resume_requested;
			ctx->resume_requested = 0;
			pthread_mutex_unlock(&ctx->lock);

			if (do_pause && !ctx->paused) {
				a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
				gpio_asic_rst_assert();
				ctx->paused = 1;
				rr_active = 0; /* re-arm on resume, matches cold start */
				rr_nonce_armed = 0; /* re-arm the NONCE_UPDATE arm after resume's re-enum */
				fprintf(ctx->fp, "stage6: PAUSED -- RST asserted, chain idle\n");
				fflush(ctx->fp);
			}
			if (do_resume && ctx->paused) {
				uint32_t new_chip_count = stage6_resume_bringup(ctx);

				(void)new_chip_count; /* logged inside stage6_resume_bringup() */
				ctx->paused = 0;
				ctx->noncemask_done = 0; /* re-arm first-job noncemask setup, like cold start */
				fprintf(ctx->fp, "stage6: RESUMED -- mining continues\n");
				fflush(ctx->fp);
			}

			pthread_mutex_lock(&ctx->lock);
			is_paused = ctx->paused;
			pthread_mutex_unlock(&ctx->lock);

			if (is_paused) {
				/* Skip job-apply/PLL-ramp/nonce-polling while
				 * paused; still send periodic STATUS (paused=1). */
				if (time(NULL) >= next_status_time) {
					struct ipc_status st;

					memset(&st, 0, sizeof(st));
					st.asics_total = 0;
					st.paused = 1;
					memcpy(st.pll_freq, ctx->last_applied_pll_freq, sizeof(st.pll_freq));
					st.voltage_mv = ctx->last_applied_voltage_mv;
					if (ipc_link_send_only(ctx->ipc_id, IPC_MSG_STATUS, &st, sizeof(st)) == 0)
						ctx->result.status_sent++;
					next_status_time = time(NULL) + STAGE6_STATUS_INTERVAL_SEC;
				}
				delay_ms(200);
				continue;
			}
		}

		pthread_mutex_lock(&ctx->lock);
		if (ctx->have_pending_job == 2) {
			job_copy = ctx->pending_job;
			have_new_job = 1;
			ctx->have_pending_job = 1;
		}
		if (ctx->have_pending_mode) {
			memcpy(pll_copy, ctx->pending_pll_freq, sizeof(pll_copy));
			have_new_mode = 1;
			ctx->have_pending_mode = 0;
		}
		if (ctx->have_pending_rolltime_raw) {
			rolltime_raw_copy = ctx->pending_rolltime_raw;
			have_new_rolltime_raw = 1;
			ctx->have_pending_rolltime_raw = 0;
		}
		if (ctx->have_pending_voltage_raw) {
			voltage_raw_copy = ctx->pending_voltage_raw_mv;
			have_new_voltage_raw = 1;
			ctx->have_pending_voltage_raw = 0;
		}
		if (ctx->have_pending_spdlog_reset) {
			have_new_spdlog_reset = 1;
			ctx->have_pending_spdlog_reset = 0;
		}
		pthread_mutex_unlock(&ctx->lock);

		/* PLL ramp, CORE_RST=1 pulse, and noncemask reconfiguration
		 * happen before the target/vmask/work burst for a job.
		 * asic_ramp_pll_freq() ramps gradually from the last applied
		 * frequency (or the 100MHz bring-up default). domain_interval
		 * is derived from pll_freq[1]-pll_freq[0]. */
		if (have_new_mode) {
			int32_t interval = (int32_t)pll_copy[1] - (int32_t)pll_copy[0];

			asic_ramp_pll_freq(STAGE6_UART_CHANNEL, (uint16_t)ctx->last_applied_pll_freq[0],
				(uint16_t)pll_copy[0], (uint16_t)interval);
			memcpy(ctx->last_applied_pll_freq, pll_copy, sizeof(pll_copy));
			fprintf(ctx->fp, "applied SET_MODE pll_freq=[%u,%u,%u,%u] (real gradual ramp)\n",
				pll_copy[0], pll_copy[1], pll_copy[2], pll_copy[3]);
			fflush(ctx->fp);

			/* CORE_RST=1 pulse + noncemask reconfiguration, matching the
			 * real capture's exact position right after a PLL ramp,
			 * before the next job's target/vmask/work. */
			a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
			a3197s_set_reg(STAGE6_UART_CHANNEL, REG_CORE_RST, 1);
			asic_set_noncemask(STAGE6_UART_CHANNEL, 0x00000001);
			ctx->noncemask_done = 1;
			fprintf(ctx->fp, "CORE_RST pulse + noncemask reconfigured after ramp\n");
			fflush(ctx->fp);
			ctx->rolltime_raw_override_active = 0;
		}

		/* Applied after have_new_mode's block so it always wins. */
		if (have_new_rolltime_raw) {
			a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
			a3197s_set_reg(STAGE6_UART_CHANNEL, REG_ROLLTIME, rolltime_raw_copy);
			ctx->rolltime_raw_override_active = 1;
			fprintf(ctx->fp, "applied SET_ROLLTIME_RAW value=%u (debug override)\n",
				rolltime_raw_copy);
			fflush(ctx->fp);
		}

		/* Live voltage tuning, applied at runtime via IPC_MSG_SET_VOLTAGE_RAW. */
		if (have_new_voltage_raw) {
			struct ipc_set_voltage volt_req;
			/* Sized to K_IPCMSG_MAX_CONTENT_LEN -- see the rbuf comment
			 * near stage5_worker()'s HELLO/STATUS reply above for why a
			 * fixed size smaller than the real wire ceiling keeps
			 * needing to grow. */
			uint8_t rbuf[K_IPCMSG_MAX_CONTENT_LEN];
			uint16_t reply_len = 0;
			int32_t ret_val = 0;
			int rc2;

			volt_req.target_mv = voltage_raw_copy;
			rc2 = ipc_link_send_sync(ctx->ipc_id, IPC_MSG_SET_VOLTAGE, &volt_req, sizeof(volt_req),
				rbuf, sizeof(rbuf), &reply_len, &ret_val, 5000);
			fprintf(ctx->fp, "applied SET_VOLTAGE_RAW(%dmV) send_sync rc=%d ret_val=%d "
				"reply_len=%u (debug override)\n",
				volt_req.target_mv, rc2, ret_val, (unsigned)reply_len);
			fflush(ctx->fp);
			ctx->last_applied_voltage_mv = (uint32_t)volt_req.target_mv;
		}

		/* Debug override, see IPC_MSG_RESET_SPDLOG_RAW. Same two
		 * writes a3197s_configure_regs() makes once at init, re-issued
		 * on demand to force a genuinely fresh SmartSpeed sampling
		 * window instead of relying on a3197s_read_smartspeed_log()'s own
		 * exact-equality timer check to eventually re-arm itself. */
		if (have_new_spdlog_reset) {
			a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
			a3197s_set_reg(STAGE6_UART_CHANNEL, REG_SS_LOG_RESET, 1);
			a3197s_set_reg(STAGE6_UART_CHANNEL, REG_SS_LOG_TIMER, SS_TIMER_DEFAULT_VALUE);
			fprintf(ctx->fp, "applied RESET_SPDLOG_RAW (debug override)\n");
			fflush(ctx->fp);
		}

		if (have_new_job) {
			uint32_t targetl, targeth;

			/* Real firmware never configures noncemask during early
			 * init (see asic_set_noncemask()'s comment) -- if this is
			 * the very first job and no SET_MODE has ever arrived to
			 * trigger the block above, do it once here so the chip
			 * still gets it before any job is ever applied. */
			if (!ctx->noncemask_done) {
				a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
				a3197s_set_reg(STAGE6_UART_CHANNEL, REG_CORE_RST, 1);
				asic_set_noncemask(STAGE6_UART_CHANNEL, 0x00000001);
				ctx->noncemask_done = 1;
				fprintf(ctx->fp, "CORE_RST pulse + noncemask configured before first job "
					"(no SET_MODE received yet)\n");
				fflush(ctx->fp);
			}

			a3197s_set_job(&job_copy);
			a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);

			/* job_copy.target[0:8) holds the low 8 bytes of a
			 * 256-bit LE target: bytes [0:4) = TARGETL, [4:8) =
			 * TARGETH. */
			memcpy(&targetl, job_copy.target, 4);
			memcpy(&targeth, job_copy.target + 4, 4);

			a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, targetl);
			a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, targeth);

			/* Sends work as a per-chip addressed loop rather than one
			 * broadcast, since a3197s_submit_job() only affects whatever
			 * chip is currently selected. Each call also increments
			 * g_active_job.nonce2 and recomputes the merkle root for
			 * that chip's own nonce2. */
			{
				uint16_t wchip;

				for (wchip = 0; wchip < STAGE6_CHIP_COUNT; wchip++) {
					a3197s_select_chip(STAGE6_UART_CHANNEL, wchip);
					a3197s_submit_job(STAGE6_UART_CHANNEL, wchip, 0);
				}
				a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
			}

			/* REG_NONCE_UPDATE is not written here; it is only
			 * touched at the round-robin's own wraparound below, so
			 * job application never disturbs nonces already
			 * buffered. Each chip picks up the new job on its next
			 * round-robin visit via a3197s_submit_job(). */
			rr_active = 1;
			ctx->result.jobs_applied++;
			fprintf(ctx->fp, "applied job_id=0x%08x to chain, per-chip addressed "
				"(targetl=0x%08x targeth=0x%08x written)\n",
				job_copy.job_id, targetl, targeth);
			fflush(ctx->fp);
		}

		/* Periodic REG_ROLLTIME refresh, independent of job/work
		 * state. Uses ctx->last_applied_pll_freq[3], the frequency
		 * domain a3197s_pll_apply() uses for this register. */
		if (!ctx->paused && !ctx->rolltime_raw_override_active &&
		    monotonic_ms() >= next_rolltime_refresh_ms) {
			asic_refresh_rolltime(STAGE6_UART_CHANNEL, (uint16_t)ctx->last_applied_pll_freq[3]);
			next_rolltime_refresh_ms = monotonic_ms() + STAGE6_ROLLTIME_REFRESH_MS;
		}

		/* Periodic frequency auto-tune, on its own cadence. Skipped
		 * while paused or before get_ghsmm() reports any telemetry. */
		if (STAGE6_AUTOTUNE_ENABLED && !ctx->paused && monotonic_ms() >= next_autotune_check_ms) {
			next_autotune_check_ms = monotonic_ms() + STAGE6_AUTOTUNE_CHECK_MS;

			if (get_ghsmm() == 0) {
				fprintf(ctx->fp, "autotune: skipped, no real telemetry yet "
					"(ghsmm=0, SmartSpeed window hasn't closed this boot)\n");
				fflush(ctx->fp);
			} else {
				double spd_dh = get_spd_dh();
				int32_t cur_base = (int32_t)ctx->last_applied_pll_freq[0];
				int32_t interval = (int32_t)ctx->last_applied_pll_freq[1] - cur_base;
				int32_t new_base = cur_base;
				const char *action = "hold";

				if (spd_dh <= STAGE6_AUTOTUNE_SPD_DH_LOW &&
				    cur_base + STAGE6_AUTOTUNE_STEP_MHZ <= STAGE6_AUTOTUNE_MAX_FREQ_MHZ) {
					new_base = cur_base + STAGE6_AUTOTUNE_STEP_MHZ;
					action = "step UP";
				} else if (spd_dh >= STAGE6_AUTOTUNE_SPD_DH_HIGH &&
				           cur_base - STAGE6_AUTOTUNE_STEP_MHZ >= STAGE6_AUTOTUNE_MIN_FREQ_MHZ) {
					new_base = cur_base - STAGE6_AUTOTUNE_STEP_MHZ;
					action = "step DOWN";
				}

				fprintf(ctx->fp, "autotune: spd_dh=%.2f%% cur_base=%dMHz -> %s (new_base=%dMHz)\n",
					spd_dh, cur_base, action, new_base);
				fflush(ctx->fp);

				if (new_base != cur_base) {
					uint32_t new_pll[4];
					uint8_t k;

					for (k = 0; k < PLL_DOMAIN_COUNT; k++)
						new_pll[k] = (uint32_t)(new_base + interval * (int32_t)k);

					asic_ramp_pll_freq(STAGE6_UART_CHANNEL, (uint16_t)cur_base,
						(uint16_t)new_base, (uint16_t)interval);
					memcpy(ctx->last_applied_pll_freq, new_pll, sizeof(new_pll));

					/* Same CORE_RST+noncemask sequence as have_new_mode's block. */
					a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
					a3197s_set_reg(STAGE6_UART_CHANNEL, REG_CORE_RST, 1);
					asic_set_noncemask(STAGE6_UART_CHANNEL, 0x00000001);

					fprintf(ctx->fp, "autotune: applied pll_freq=[%u,%u,%u,%u] "
						"(real gradual ramp + CORE_RST + noncemask)\n",
						new_pll[0], new_pll[1], new_pll[2], new_pll[3]);
					fflush(ctx->fp);
				}
			}
		}

		/* Round-robin steady-state work+nonce cycling: one chip is
		 * serviced per main-loop iteration. If the previous cycle's
		 * nonce buffers are armed, read this chip's nonce buffer;
		 * send this chip fresh work; advance; on wraparound, re-arm
		 * the chain's nonce buffers. */
		if (rr_active && !ctx->paused) {
			a3197s_select_chip(STAGE6_UART_CHANNEL, rr_chip);

			if (rr_nonce_armed) {
				struct asic_nonce_record nonces[NONCE_RECORD_MAX];
				uint8_t nonce_count = 0;
				uint8_t n;

				if (asic_get_nonce(STAGE6_UART_CHANNEL, rr_chip, nonces, &nonce_count) == 0) {
					for (n = 0; n < nonce_count; n++) {
						struct ipc_nonce out;

						if (!nonces[n].valid)
							continue;

						ctx->result.nonces_found++;
						if (ctx->result.nonces_found <= STAGE6_MAX_NONCES_LOGGED) {
							int i = ctx->result.nonces_found - 1;

							ctx->result.nonce_job_ids[i] = nonces[n].job_id;
							ctx->result.nonce_values[i] = nonces[n].nonce;
						}

						out.job_id = nonces[n].job_id;
						out.nonce2 = nonces[n].nonce2;
						out.nonce = nonces[n].nonce;
						out.asic_id = (uint16_t)nonces[n].asic_id;
						out.miner_id = (uint8_t)nonces[n].miner_id;
						out.ntime = (uint8_t)nonces[n].ntime;
						out.mid_id = (uint8_t)nonces[n].mid_id;

						if (ipc_link_send_only(ctx->ipc_id, IPC_MSG_NONCE, &out, sizeof(out)) == 0)
							ctx->result.nonces_sent++;
					}
				}
			}

			a3197s_submit_job(STAGE6_UART_CHANNEL, rr_chip, 0);

			rr_chip = (uint16_t)((rr_chip + 1) % STAGE6_CHIP_COUNT);
			if (rr_chip == 0) {
				/* REG_NONCE_UPDATE (0x200) is written only once per
				 * session, on the first wraparound; the chip streams
				 * nonces continuously afterward without re-arming. */
				if (!rr_nonce_armed) {
					a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
					a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
					a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
				}
				rr_nonce_armed = 1;
			}
		}

		/* Periodic STATUS: full PVT sweep plus the SmartSpeed/PLLCNT1-
		 * derived hashrate (get_ghsmm()). */
		if (time(NULL) >= next_status_time) {
			struct ipc_status st;
			double temp_sum = 0, temp_max = -1000;
			int temp_n = 0;

			memset(&st, 0, sizeof(st));

			/* Fills struct ipc_status.chips[] per chip from
			 * a3197s_read_smartspeed_counts/spdlog, pvt_t/vcore_update,
			 * asic_get_pll_detail(), and asic_get_nonce_chip_stat(). */
			st.chip_count = (uint8_t)(STAGE6_CHIP_COUNT > IPC_STATUS_MAX_CHIPS ?
				IPC_STATUS_MAX_CHIPS : STAGE6_CHIP_COUNT);
			for (chip = 0; chip < STAGE6_CHIP_COUNT && chip < IPC_STATUS_MAX_CHIPS; chip++) {
				struct ipc_chip_status *cs = &st.chips[chip];
				double t, v;
				uint16_t cnt[4];
				uint32_t freq[4];
				uint32_t ctmo, chb, cdata;
				int k;

				a3197s_read_smartspeed_counts(STAGE6_UART_CHANNEL, chip);
				a3197s_read_smartspeed_log(STAGE6_UART_CHANNEL, chip);

				pvt_tcore_update(STAGE6_UART_CHANNEL, chip);
				t = pvt_tcore_get(STAGE6_UART_CHANNEL, chip);
				if (t > -273.0) { /* -273.0 = invalid-read sentinel, see pvt.c */
					temp_sum += t;
					temp_n++;
					if (t > temp_max)
						temp_max = t;
				}
				cs->temp_c = (float)t;

				pvt_vcore_update(STAGE6_UART_CHANNEL, chip);
				v = pvt_vcore_get(STAGE6_UART_CHANNEL, chip);
				cs->volt_mv = (float)v;

				asic_get_pll_detail(STAGE6_UART_CHANNEL, (uint16_t)chip, cnt, freq);
				for (k = 0; k < 4; k++) {
					cs->pll_cnt[k] = (uint8_t)cnt[k];
					cs->pll_freq[k] = (uint16_t)freq[k];
				}

				asic_get_nonce_chip_stat(STAGE6_UART_CHANNEL, (uint16_t)chip, &ctmo, &chb, &cdata);
				cs->nonce_timeout = ctmo;
				cs->nonce_heartbeat = chb;
				cs->nonce_data = cdata;

				asic_get_spd_chip_stat(STAGE6_UART_CHANNEL, (uint16_t)chip, &cs->ghsspd, &cs->spd_dh);
			}

			st.asics_total = STAGE6_CHIP_COUNT;
			st.ghsmm = get_ghsmm();
			st.spd_dh = (float)get_spd_dh();
			st.ghsspd = get_ghsspd();
			st.temp_avg = (float)(temp_n > 0 ? temp_sum / temp_n : 0.0);
			st.temp_max = (float)(temp_n > 0 ? temp_max : 0.0);
			/* Commanded frequency, not read back. */
			memcpy(st.pll_freq, ctx->last_applied_pll_freq, sizeof(st.pll_freq));
			st.err_crc = a3197s_get_errcnt(STAGE6_UART_CHANNEL);
			st.voltage_mv = ctx->last_applied_voltage_mv;
			asic_get_errcnt_detail(STAGE6_UART_CHANNEL, &st.nonce_read_err,
				&st.nonce_bad_len, &st.nonce_overflow, &st.regread_err);

			if (ipc_link_send_only(ctx->ipc_id, IPC_MSG_STATUS, &st, sizeof(st)) == 0)
				ctx->result.status_sent++;

			/* One compact chain-wide summary line; per-chip detail
			 * travels over IPC in struct ipc_status.chips[]. */
			fprintf(ctx->fp, "STATUS ghsmm=%u ghsspd=%u spd_dh=%.2f%% temp_avg=%.1f temp_max=%.1f "
				"err_crc=%u (nre=%u nbl=%u nov=%u rre=%u)\n",
				st.ghsmm, st.ghsspd, st.spd_dh, st.temp_avg, st.temp_max, st.err_crc,
				st.nonce_read_err, st.nonce_bad_len, st.nonce_overflow, st.regread_err);
			fflush(ctx->fp);
			ipc6_log_cap_size(ctx->fp);

			/* Bound the wire trace to the first few STATUS intervals. */
			wiretrace_status_count++;
			if (wiretrace_status_count >= 3 && ctx->wiretrace_fp) {
				uart_set_wiretrace_log(NULL);
				fclose(ctx->wiretrace_fp);
				ctx->wiretrace_fp = NULL;
			}

			next_status_time = time(NULL) + STAGE6_STATUS_INTERVAL_SEC;
		}

		delay_ms(1);
	}

	ipc_link_close(ctx->ipc_id, STAGE5_SERVICE_NAME);
	sem_post(&ctx->done_sem);
	return NULL;
}

static int stage6_bounded(struct stage6_ctx **out)
{
	struct stage6_ctx *ctx;
	pthread_t tid;
	int rc;
	uint32_t chip_count = STAGE6_CHIP_COUNT;

	*out = NULL;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	/* Baseline PLL state (a3197s_init_chain()'s bring-up default) before
	 * any SET_MODE has arrived, so the first SET_MODE ramps from the
	 * true current frequency. */
	ctx->last_applied_pll_freq[0] = 100;
	ctx->last_applied_pll_freq[1] = 100;
	ctx->last_applied_pll_freq[2] = 100;
	ctx->last_applied_pll_freq[3] = 100;
	pthread_mutex_init(&ctx->lock, NULL);
	sem_init(&ctx->done_sem, 0, 0);
	ctx->fp = fopen("/sharefs/ipc6.log", "a");
	if (!ctx->fp) {
		pthread_mutex_destroy(&ctx->lock);
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}
	{
		/* Diagnostic hook (disabled): logs which register/chip
		 * accounts for regread_err. */
		/*
		FILE *regread_fp = fopen("/sharefs/regread_debug.log", "a");
		if (regread_fp)
			asic_set_debug_log(regread_fp);
		*/
	}
	{
		/* Diagnostic hook (disabled): PLL LOCK/LOCK_IP read-only check. */
		/*
		FILE *lock_diag_fp = fopen("/sharefs/pll_lock_diag.log", "a");
		if (lock_diag_fp)
			asic_set_lock_diag_log(lock_diag_fp);
		*/
	}
	/* Worklog disabled by default; enable via asic_set_worklog() for a
	 * short capture window. */
	asic_set_worklog(NULL);

	/* Set before ipc_link_open() (which could deliver a message the
	 * instant it connects), so stage6_recv_handler() never sees a
	 * NULL/stale g_stage6_ctx. */
	g_stage6_ctx = ctx;

	/* Connects before touching RST/enum/hardware. ipc_link_open() blocks
	 * until a peer connects. */
	ipc_link_set_recv_handler(stage6_recv_handler);
	fprintf(ctx->fp, "stage6: waiting for IPC connect (this IS the power_en/RST sync signal -- "
		"blocks until the harness connects) before touching RST/enum\n");
	fflush(ctx->fp);
	{
		int id = ipc_link_open(STAGE5_SERVICE_NAME, STAGE5_RTOS_TARGET, STAGE5_PORT);

		fprintf(ctx->fp, "stage6: ipc_link_open() returned id=%d\n", id);
		fflush(ctx->fp);
		ctx->result.connect_rc = (id >= 0) ? 0 : -1;
		if (id < 0) {
			g_stage6_ctx = NULL;
			fclose(ctx->fp);
			pthread_mutex_destroy(&ctx->lock);
			sem_destroy(&ctx->done_sem);
			free(ctx);
			return 1;
		}
		ctx->ipc_id = id;

		/* power_en must be set for at least ~3.4s before RST is
		 * touched, to let the DC/DC voltage rail stabilize. Added
		 * after connect() rather than before, since connect()
		 * succeeding is the only available lower bound on "power_en
		 * is now set". Bracketed with monotonic timestamps so its
		 * actual duration is visible in ipc6.log. */
		{
			struct timespec t0, t1;
			long elapsed_ms;

			clock_gettime(CLOCK_MONOTONIC, &t0);
			delay_ms(3400);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
			fprintf(ctx->fp, "stage6: post-connect settle delay_ms(3400) actually took %ldms\n",
				elapsed_ms);
			fflush(ctx->fp);
		}
	}

	/* This used
	 * to call try_enum_channel_bounded() here -- a throwaway probe
	 * (channel_probe_worker(), its own thread) that does a FULL RST
	 * pulse + uart_init + ENTER_CONFIG_MODE + a3197s_enumerate() of its own,
	 * BEFORE this function's own, separate RST pulse + re-enum below.
	 * That meant the real --ipc6 path reset and enumerated the chain
	 * TWICE every run -- once in the probe, once for real -- while every
	 * other path that has ever gotten full chip responsiveness
	 * (--stage6replay, --stage6iso, --stage6iso2) resets and enumerates
	 * exactly ONCE. Removed entirely (not disabled) so this path now
	 * matches that single-cycle pattern too -- gpio_init() is still
	 * needed here before ENTER_CONFIG_MODE and the real RST pulse below. */
	if (g_stage6_persist_paused) {
		/* See g_stage6_persist_paused's declaration comment -- skip
		 * ALL hardware bring-up below (RST pulse, enum, baud switch,
		 * TARGETL/H, SET_VOLTAGE) and go straight into the paused
		 * STATUS-only loop, so a persisted operator PAUSE survives
		 * this cycle boundary without ever touching RST/UART. */
		ctx->paused = 1;
		fprintf(ctx->fp, "stage6: resuming a persisted PAUSE across the cycle "
			"boundary -- skipping bring-up entirely, staying idle\n");
		fflush(ctx->fp);
		goto spawn_worker;
	}

	gpio_init(HBOARD_COUNT);

	ENTER_CONFIG_MODE(STAGE6_UART_CHANNEL);
	delay_ms(3);

	/* Hardware bring-up (RST pulse through SET_VOLTAGE) runs here, in
	 * the caller's thread, after connect() and before stage6_worker()
	 * is spawned. */

	/* gpio_asic_rst_init() puts ASIC_RST_PIN into GPIO_DM_OUTPUT mode;
	 * without it, gpio_asic_rst_assert()/deassert() below write to a
	 * pin that was never configured as an output. */
	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE6_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);

	/* Enum must run before a3197s_init_chain()'s broadcast CFG/ECC/PVT-init,
	 * since a broadcast (0x3FF) sent before the chain is enumerated
	 * only reaches chip 0 -- downstream chips only begin relaying UART
	 * traffic once they've received their own address via enum. */
	{
		uint32_t reenum_count = a3197s_enumerate(STAGE6_UART_CHANNEL);

		fprintf(ctx->fp, "stage6: post-reset re-enum chip_count=%u\n", reenum_count);
		fflush(ctx->fp);
		if (reenum_count > 0 && reenum_count <= STAGE6_CHIP_COUNT)
			chip_count = reenum_count;
	}

	a3197s_init_chain(STAGE6_UART_CHANNEL);
	delay_ms(100);

	/* Reads REG_UART_STATUS (0x3CA0), individually addressed to every
	 * chip, logging each chip's a3197s_read_fifo() return code
	 * (0=success, 1=failure, 2=silent) separately. */
	{
		int addr_ret[STAGE6_CHIP_COUNT];
		uint32_t addr_chipid[STAGE6_CHIP_COUNT];
		uint32_t c;

		asic_enum_verify_alladdr(STAGE6_UART_CHANNEL, chip_count, addr_ret, addr_chipid);
		for (c = 0; c < chip_count; c++) {
			fprintf(ctx->fp, "stage6: addr-check chip=%u ret=%d chipid=%u (expected %u)%s\n",
				c, addr_ret[c], addr_chipid[c], c,
				(addr_ret[c] == 0 && addr_chipid[c] == c) ? " MATCH" : "");
		}
		fflush(ctx->fp);
	}

	/* Probes a single register read on chip 0, confirming the read path
	 * is alive before the rest of the mining loop relies on it. */
	{
		uint32_t before = a3197s_get_errcnt(STAGE6_UART_CHANNEL);
		uint32_t probe_val;
		uint32_t after;

		a3197s_select_chip(STAGE6_UART_CHANNEL, 0);
		probe_val = a3197s_get_reg(STAGE6_UART_CHANNEL, 0x0);
		after = a3197s_get_errcnt(STAGE6_UART_CHANNEL);
		fprintf(ctx->fp, "stage6: cold-start read probe chip=0 reg=0x0 val=0x%08x "
			"errcnt_before=%u errcnt_after=%u (%s)\n",
			probe_val, before, after, (after == before) ? "OK" : "FAILED");
		fflush(ctx->fp);
	}

	/* Switches from the 115200 baud used for enum/setup to
	 * UART_HIGH_BAUD_RATE (4.8Mbaud host->chip / ~5.0Mbaud chip->host)
	 * for mining. a3197s_set_uart_baud() sends the wire baud-change
	 * command and reconfigures the local UART to match; it does not
	 * rewrite REG_UART_CFG1. Selects broadcast first so every chip
	 * receives the baud-change command, not just chip 0 from the probe
	 * above. */
	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);

	fprintf(ctx->fp, "stage6: switching UART baud 115200 -> %d\n", UART_HIGH_BAUD_RATE);
	fflush(ctx->fp);
	a3197s_set_uart_baud(STAGE6_UART_CHANNEL, UART_HIGH_BAUD_RATE);
	delay_ms(1000);
	{
		uint32_t before = a3197s_get_errcnt(STAGE6_UART_CHANNEL);
		uint32_t probe_val;
		uint32_t after;

		a3197s_select_chip(STAGE6_UART_CHANNEL, 0);
		probe_val = a3197s_get_reg(STAGE6_UART_CHANNEL, 0x0);
		after = a3197s_get_errcnt(STAGE6_UART_CHANNEL);
		fprintf(ctx->fp, "stage6: post-baud-switch read probe chip=0 reg=0x0 val=0x%08x "
			"errcnt_before=%u errcnt_after=%u (%s)\n",
			probe_val, before, after, (after == before) ? "OK" : "FAILED");
		fflush(ctx->fp);
	}

	/* All-ones default until a real job sets the target from
	 * ipc_job.target, so early nonce polling doesn't run against an
	 * uninitialized target. */
	fprintf(ctx->fp, "stage6: pre-TARGETL write\n");
	fflush(ctx->fp);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0xffffffff);
	fprintf(ctx->fp, "stage6: post-TARGETL pre-TARGETH write\n");
	fflush(ctx->fp);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, 0xffffffff);
	fprintf(ctx->fp, "stage6: post-TARGETH write\n");
	fflush(ctx->fp);

	/* Sets a known core voltage explicitly, since the DC/DC regulator
	 * is otherwise left at whatever value it last had. */
	{
		struct ipc_set_voltage volt_req;
		/* Sized to K_IPCMSG_MAX_CONTENT_LEN. */
		uint8_t rbuf[K_IPCMSG_MAX_CONTENT_LEN];
		uint16_t reply_len = 0;
		int32_t ret_val = 0;
		int rc2;

		/* 3496mV pairs with nano3s.rs's LOW-mode frequency
		 * table (210-270MHz, as opposed to the 338-398MHz MED table --
		 * see NANO3S_PLL_FREQ_TARGET). 3496mV is this project's
		 * long-validated LOW-mode production default (nearly halves
		 * fail rate vs. LOW's own factory-stock 3392mV, with the
		 * SS_CTRL/0x4c4/0x4cc fixes in place). This is the
		 * production value for voltage; revert nano3s.rs's
		 * frequency table instead if MED mode (338-398MHz) is preferred. */
		volt_req.target_mv = 3496;
		rc2 = ipc_link_send_sync(ctx->ipc_id, IPC_MSG_SET_VOLTAGE, &volt_req, sizeof(volt_req),
			rbuf, sizeof(rbuf), &reply_len, &ret_val, 5000);
		fprintf(ctx->fp, "stage6: SET_VOLTAGE(%dmV) send_sync rc=%d ret_val=%d reply_len=%u\n",
			volt_req.target_mv, rc2, ret_val, (unsigned)reply_len);
		fflush(ctx->fp);

		/* Track what we commanded, same
		 * "commanded not read back" reasoning as last_applied_pll_freq
		 * -- see struct stage6_ctx's declaration comment. */
		ctx->last_applied_voltage_mv = (uint32_t)volt_req.target_mv;
	}

spawn_worker:
	if (pthread_create(&tid, NULL, stage6_worker, ctx) != 0) {
		g_stage6_ctx = NULL;
		fclose(ctx->fp);
		pthread_mutex_destroy(&ctx->lock);
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	/* Unbounded wait: stage6_worker() has no time limit, so this just
	 * waits for it to finish (wrong_peer or process exit). A genuine
	 * stall is caught separately by stage6_hard_watchdog()'s heartbeat
	 * check. */
	do {
		rc = sem_wait(&ctx->done_sem);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		/* Detach, never join, never free. g_stage6_ctx is left
		 * pointing at ctx since the recv thread may still be alive
		 * and dereferencing it. */
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	*out = ctx;
	return 0;
}

static void *stage6_hard_watchdog(void *unused)
{
	(void)unused;

	/* Stall detector: polls g_stage6_last_progress (stamped by
	 * stage6_worker() every loop iteration) and fires only after zero
	 * progress for STAGE6_STALL_SEC. */
	for (;;) {
		delay_ms(5000);
		if (time(NULL) - g_stage6_last_progress < STAGE6_STALL_SEC)
			continue;

		{
			FILE *fp = fopen("/sharefs/ipc6.log", "a");

			if (fp) {
				fprintf(fp, "stage6_hard_watchdog: genuine stall detected (no "
					"progress for >=%ds) -- forcing exit(3), device needs a "
					"reboot to recover (nothing relaunches init.sh)\n",
					STAGE6_STALL_SEC);
				fclose(fp);
			}
		}
		gpio_asic_rst_deassert();
		ENTER_WORK_MODE(STAGE6_UART_CHANNEL);
		_exit(3);
	}
	return NULL;
}

static int run_stage6_ipc_mining(void)
{
	struct stage6_ctx *ctx = NULL;
	pthread_t watchdog_tid;
	int pass;
	int i;
	FILE *summary;

	/* Hardware bring-up runs inside stage6_bounded(), after IPC
	 * connect() succeeds. */

	/* This watchdog thread is explicitly cancelled on every return
	 * path below, so it can't survive past the cycle it was armed for
	 * (relevant when this function is called repeatedly, as with
	 * --ipc6loop). */
	/* Stamped here, before the connect attempt starts, so the
	 * watchdog's budget covers the connect handshake too. */
	g_stage6_last_progress = time(NULL);

	if (pthread_create(&watchdog_tid, NULL, stage6_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);
	else
		watchdog_tid = 0;

	if (stage6_bounded(&ctx) != 0) {
		if (watchdog_tid)
			pthread_cancel(watchdog_tid);
		summary = fopen("/sharefs/ipc6.log", "a");
		if (summary) {
			fprintf(summary, "stage6 TIMEOUT -- worker cancelled (may still be running; "
				"context intentionally leaked)\n");
			fclose(summary);
		}
		return 2;
	}

	/* Cancel this cycle's watchdog now, so it can't fire against a
	 * later --ipc6loop cycle. */
	if (watchdog_tid)
		pthread_cancel(watchdog_tid);

	pass = (ctx->result.connect_rc == 0 && ctx->result.jobs_applied > 0 && !ctx->wrong_peer);

	fprintf(ctx->fp, "stage6 summary: connect_rc=%d wrong_peer=%d jobs_received=%d jobs_applied=%d "
		"nonces_found=%d nonces_sent=%d status_sent=%d set_mode_received=%d last_job_id=0x%08x\n",
		ctx->result.connect_rc, ctx->wrong_peer, ctx->result.jobs_received, ctx->result.jobs_applied,
		ctx->result.nonces_found, ctx->result.nonces_sent, ctx->result.status_sent,
		ctx->result.set_mode_received, ctx->result.last_job_id);
	for (i = 0; i < ctx->result.nonces_found && i < STAGE6_MAX_NONCES_LOGGED; i++)
		fprintf(ctx->fp, "  nonce[%d] job_id=0x%08x nonce=0x%08x\n",
			i, ctx->result.nonce_job_ids[i], ctx->result.nonce_values[i]);
	fprintf(ctx->fp, "stage6 complete: %s\n",
		pass ? "PASS -- at least one JOB applied to real hardware over IPC" : "FAIL");
	fflush(ctx->fp);
	fclose(ctx->fp);

	if (ctx->wiretrace_fp) {
		uart_set_wiretrace_log(NULL);
		fclose(ctx->wiretrace_fp);
		ctx->wiretrace_fp = NULL;
	}

	g_stage6_ctx = NULL;
	pthread_mutex_destroy(&ctx->lock);
	sem_destroy(&ctx->done_sem);
	free(ctx);

	/* Don't undo a persisted PAUSE on the way out; RESUME or the next
	 * cycle's skip-bringup path handles RST correctly instead. */
	if (!g_stage6_persist_paused) {
		gpio_asic_rst_deassert();
		ENTER_WORK_MODE(STAGE6_UART_CHANNEL);
	}

	return pass ? 0 : 6;
}

/* Diagnostic-only: reproduces stage6_worker()'s pre-mining-loop sequence
 * (reset, uart_init, a3197s_init_chain, asic_enum_verify, cold-start probe,
 * baud switch, TARGETL/TARGETH writes, startup delay) with no IPC. Runs
 * a PVT sweep afterward, logging regread_err's delta around the sweep. */
#define STAGE6PVT_TIMEOUT_SEC 120
#define STAGE6PVT_LOG "/sharefs/stage6pvt.log"

static void *stage6pvt_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE6PVT_TIMEOUT_SEC + 10) * 1000);
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);
	_exit(3);
	return NULL;
}

static void *stage6pvt_worker(void *arg)
{
	struct pvt_sweep_ctx *ctx = arg;
	FILE *fp = fopen(STAGE6PVT_LOG, "a");

	if (!fp)
		return NULL;

	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE6_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);
	a3197s_init_chain(STAGE6_UART_CHANNEL);
	delay_ms(100);

	{
		int verify_fail_count = asic_enum_verify(STAGE6_UART_CHANNEL, STAGE6_CHIP_COUNT);
		fprintf(fp, "stage6pvt: post-enum-verify fail_count=%d\n", verify_fail_count);
		fflush(fp);
	}

	{
		uint32_t before = a3197s_get_errcnt(STAGE6_UART_CHANNEL);
		uint32_t probe_val;
		uint32_t after;

		a3197s_select_chip(STAGE6_UART_CHANNEL, 0);
		probe_val = a3197s_get_reg(STAGE6_UART_CHANNEL, 0x0);
		after = a3197s_get_errcnt(STAGE6_UART_CHANNEL);
		fprintf(fp, "stage6pvt: cold-start probe chip=0 reg=0x0 val=0x%08x errcnt_before=%u "
			"errcnt_after=%u (%s)\n", probe_val, before, after, (after == before) ? "OK" : "FAILED");
		fflush(fp);
	}

	/* Restore broadcast selection before the baud switch (the
	 * cold-start probe above left chip 0 selected). */
	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);

	fprintf(fp, "stage6pvt: switching UART baud 115200 -> %d\n", UART_HIGH_BAUD_RATE);
	fflush(fp);
	a3197s_set_uart_baud(STAGE6_UART_CHANNEL, UART_HIGH_BAUD_RATE);
	delay_ms(1000);

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0xffffffff);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, 0xffffffff);
	fprintf(fp, "stage6pvt: TARGETL/TARGETH written, starting %ds delay (matches Stage 6's "
		"STAGE6_STARTUP_DELAY_SEC)\n", STAGE6_STARTUP_DELAY_SEC);
	fflush(fp);
	delay_ms(STAGE6_STARTUP_DELAY_SEC * 1000);

	fprintf(fp, "stage6pvt: delay done, starting PVT sweep (regread_err delta logged per chip)\n");
	fflush(fp);

	for (uint16_t chip = 0; chip < STAGE6_CHIP_COUNT; chip++) {
		uint32_t nre0, nbl0, nov0, rre0;
		uint32_t nre1, nbl1, nov1, rre1;
		int attempt;

		asic_get_errcnt_detail(STAGE6_UART_CHANNEL, &nre0, &nbl0, &nov0, &rre0);

		for (attempt = 1; attempt <= PVT_RETRY_MAX; attempt++) {
			pvt_tcore_update(STAGE6_UART_CHANNEL, chip);
			ctx->result.temp_c[chip] = pvt_tcore_get(STAGE6_UART_CHANNEL, chip);
			if (ctx->result.temp_c[chip] != PVT_TEMP_INVALID)
				break;
			delay_ms(PVT_RETRY_DELAY_MS);
		}
		ctx->result.temp_attempts[chip] = attempt;

		for (attempt = 1; attempt <= PVT_RETRY_MAX; attempt++) {
			pvt_vcore_update(STAGE6_UART_CHANNEL, chip);
			ctx->result.volt_mv[chip] = pvt_vcore_get(STAGE6_UART_CHANNEL, chip);
			if (ctx->result.volt_mv[chip] != 0.0)
				break;
			delay_ms(PVT_RETRY_DELAY_MS);
		}
		ctx->result.volt_attempts[chip] = attempt;

		asic_get_errcnt_detail(STAGE6_UART_CHANNEL, &nre1, &nbl1, &nov1, &rre1);
		fprintf(fp, "stage6pvt: chip=%d temp_c=%.2f (attempts=%d) volt_mv=%.2f (attempts=%d) "
			"regread_err_delta=%u\n", chip, ctx->result.temp_c[chip], ctx->result.temp_attempts[chip],
			ctx->result.volt_mv[chip], ctx->result.volt_attempts[chip], rre1 - rre0);
		fflush(fp);
	}

	fprintf(fp, "stage6pvt: sweep complete\n");
	fclose(fp);
	sem_post(&ctx->done_sem);
	return NULL;
}

static int run_stage6ctx_pvt(void)
{
	struct pvt_sweep_ctx *ctx;
	pthread_t tid, watchdog_tid;
	struct timespec deadline;
	int rc;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE6_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage6pvt_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);

	if (pthread_create(&tid, NULL, stage6pvt_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE6PVT_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	sem_destroy(&ctx->done_sem);
	free(ctx);

	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);

	return 0;
}

/* Diagnostic-only: tests nonces_found>0 isolated from IPC. Reproduces
 * stage6_worker()'s setup (reset, a3197s_init_chain, baud switch,
 * TARGETL/TARGETH, startup delay), submits one dummy job with a real
 * vmask table, and polls the nonce buffer. */
#define STAGE6NONCE_POLL_SEC 60
#define STAGE6NONCE_TIMEOUT_SEC (STAGE6_STARTUP_DELAY_SEC + STAGE6NONCE_POLL_SEC + 30)
#define STAGE6NONCE_LOG "/sharefs/stage6nonce.log"
#define STAGE6NONCE_DUMMY_JOB_ID 0xC0FFEE03u
#define STAGE6NONCE_COINBASE_LEN 64
#define STAGE6NONCE_MERKLE_OFFSET 36
#define STAGE6NONCE_MAX_NONCES 64

struct stage6nonce_ctx {
	sem_t done_sem;
	int nonces_found;
};

static void *stage6nonce_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE6NONCE_TIMEOUT_SEC + 10) * 1000);
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);
	_exit(3);
	return NULL;
}

static void *stage6nonce_worker(void *arg)
{
	struct stage6nonce_ctx *ctx = arg;
	a3197s_job_t job;
	FILE *fp = fopen(STAGE6NONCE_LOG, "a");
	time_t poll_deadline;
	uint32_t reads_attempted = 0, reads_err = 0, reads_ok_empty = 0, reads_ok_data = 0;

	if (!fp)
		return NULL;

	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE6_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);
	a3197s_init_chain(STAGE6_UART_CHANNEL);
	delay_ms(100);

	fprintf(fp, "stage6nonce: switching UART baud 115200 -> %d\n", UART_HIGH_BAUD_RATE);
	fflush(fp);
	a3197s_set_uart_baud(STAGE6_UART_CHANNEL, UART_HIGH_BAUD_RATE);
	delay_ms(1000);

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0xffffffff);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, 0xffffffff);
	fprintf(fp, "stage6nonce: TARGETL/TARGETH written, starting %ds delay\n",
		STAGE6_STARTUP_DELAY_SEC);
	fflush(fp);
	delay_ms(STAGE6_STARTUP_DELAY_SEC * 1000);

	memset(&job, 0, sizeof(job));
	job.job_id = STAGE6NONCE_DUMMY_JOB_ID;
	job.coinbase_len = STAGE6NONCE_COINBASE_LEN;
	memset(job.coinbase, 0x42, STAGE6NONCE_COINBASE_LEN);
	job.nonce2 = 0;
	job.nonce2_offset = 4;
	job.nonce2_size = 4;
	job.merkle_offset = STAGE6NONCE_MERKLE_OFFSET;
	job.nmerkles = 0;
	job.header[0] = 0x01;
	memset(job.header + 4, 0xAA, 32);
	*(uint32_t *)(job.header + 68) = 0x5f5e1000;
	*(uint32_t *)(job.header + 72) = 0x1d00ffff;
	job.work_restart = 1;
	job.vmask[0] = 0x00000020;
	job.vmask[1] = 0x00E0FF3F;
	job.vmask[2] = 0x00800020;
	job.vmask[3] = 0x00000120;
	job.vmask[4] = 0x00000220;
	job.vmask[5] = 0x00000420;
	job.vmask[6] = 0x00000820;
	job.vmask[7] = 0x00001020;

	a3197s_set_job(&job);
	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_submit_job(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST, 0);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	fprintf(fp, "stage6nonce: job applied job_id=0x%08x, polling nonce buffer for %ds\n",
		STAGE6NONCE_DUMMY_JOB_ID, STAGE6NONCE_POLL_SEC);
	fflush(fp);

	poll_deadline = time(NULL) + STAGE6NONCE_POLL_SEC;
	while (time(NULL) < poll_deadline && ctx->nonces_found < STAGE6NONCE_MAX_NONCES) {
		uint16_t chip;

		for (chip = 0; chip < STAGE6_CHIP_COUNT && ctx->nonces_found < STAGE6NONCE_MAX_NONCES; chip++) {
			struct asic_nonce_record nonces[NONCE_RECORD_MAX];
			uint8_t nonce_count = 0;
			int get_ret;

			a3197s_select_chip(STAGE6_UART_CHANNEL, chip);
			reads_attempted++;
			get_ret = asic_get_nonce(STAGE6_UART_CHANNEL, chip, nonces, &nonce_count);
			if (get_ret != 0) {
				reads_err++;
			} else if (nonce_count == 0) {
				reads_ok_empty++;
			} else {
				uint8_t n;

				reads_ok_data++;
				for (n = 0; n < nonce_count && ctx->nonces_found < STAGE6NONCE_MAX_NONCES; n++) {
					ctx->nonces_found++;
					fprintf(fp, "stage6nonce: NONCE#%d chip=%u job_id=0x%08x nonce2=0x%08x "
						"nonce=0x%08x asic_id=%u miner_id=%u mid_id=%u valid=%u\n",
						ctx->nonces_found, chip, nonces[n].job_id, nonces[n].nonce2,
						nonces[n].nonce, (unsigned)nonces[n].asic_id,
						(unsigned)nonces[n].miner_id, (unsigned)nonces[n].mid_id,
						(unsigned)nonces[n].valid);
					fflush(fp);
				}
			}
			delay_ms(30); /* matches Stage 6's real mining-loop cadence */
		}
	}

	fprintf(fp, "stage6nonce: poll done. nonces_found=%d reads_attempted=%u reads_err=%u "
		"reads_ok_empty=%u reads_ok_data=%u\n",
		ctx->nonces_found, reads_attempted, reads_err, reads_ok_empty, reads_ok_data);
	fclose(fp);
	sem_post(&ctx->done_sem);
	return NULL;
}

static int run_stage6ctx_nonce(void)
{
	struct stage6nonce_ctx *ctx;
	pthread_t tid, watchdog_tid;
	struct timespec deadline;
	int rc;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE6_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage6nonce_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);

	if (pthread_create(&tid, NULL, stage6nonce_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE6NONCE_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	sem_destroy(&ctx->done_sem);
	free(ctx);

	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);

	return 0;
}

/* Diagnostic-only: replays a fixed, real 12-chip job burst plus a fixed
 * target (TARGETL=0x00000000, TARGETH=0x000FFFF0), bypassing
 * a3197s_set_job()/a3197s_submit_job()'s job-to-bytes translation via
 * asic_raw_write_words(), so this tests the transport/chip
 * independently of that translation logic. */
#define STAGE6REPLAY_POLL_SEC 100
/* Ramp budget for asic_ramp_pll_freq() (100->336MHz, 10MHz/~1s steps). */
#define STAGE6REPLAY_RAMP_BUDGET_SEC 45
#define STAGE6REPLAY_TIMEOUT_SEC (STAGE6_STARTUP_DELAY_SEC + STAGE6REPLAY_RAMP_BUDGET_SEC + STAGE6REPLAY_POLL_SEC + 30)
#define STAGE6REPLAY_LOG "/sharefs/stage6replay.log"
#define STAGE6REPLAY_MAX_NONCES 64

static const uint32_t g_real_job_words[12][23] = {
	{ 0x00000000, 0x5F48D27B, 0xC80E6067, 0xFA970217, 0x089A11CC, 0x98A8CF9F, 0x89E070EC, 0x9247B109, 0x038A8BFB, 0x6678071E, 0xC2B43D81, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x0B000000, 0x00002439, 0x00000000, 0x0003E5C0 },
	{ 0x00000000, 0x46CF071A, 0xC80E6067, 0xFA970217, 0x4273497A, 0x0808E8A8, 0xAB2D73B1, 0x87D22D31, 0x737DF4BF, 0x3CC60F85, 0x38A1E2F2, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x0C000000, 0x00002439, 0x01000000, 0x0003E5C0 },
	{ 0x00000000, 0x4AAC9C8A, 0xC80E6067, 0xFA970217, 0x7D027DC5, 0x3269AF19, 0xE33A88B6, 0x42B5202A, 0xBBC45697, 0x91C34BD6, 0xF219CBAD, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x0D000000, 0x00002439, 0x02000000, 0x0003E5C0 },
	{ 0x00000000, 0x9736BCD3, 0xC80E6067, 0xFA970217, 0x32B4A325, 0x594C73EF, 0xB9C5474A, 0xD5B99CAB, 0x0BADC77A, 0x8B411BA7, 0xDE8D3C1A, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x0E000000, 0x00002439, 0x03000000, 0x0003E5C0 },
	{ 0x00000000, 0x8E1ACC41, 0xC80E6067, 0xFA970217, 0x61D848EB, 0xDF0AB6B0, 0xE05F9488, 0xC4923D8E, 0xD2A0915C, 0x6F600ED4, 0xFEA0CDB8, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x0F000000, 0x00002439, 0x04000000, 0x0003E5C0 },
	{ 0x00000000, 0x35EF304E, 0xC80E6067, 0xFA970217, 0x12228983, 0x4B12E77E, 0x0E63B653, 0x57D25048, 0x7072A189, 0xB0815818, 0xD1F80E88, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x10000000, 0x00002439, 0x05000000, 0x0003E5C0 },
	{ 0x00000000, 0x1F69A294, 0xC80E6067, 0xFA970217, 0x5A4E7DC7, 0xE36C36A3, 0xDE813ED0, 0xA1CDF225, 0x74A105E4, 0x266124B6, 0x9CB6B954, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x11000000, 0x00002439, 0x06000000, 0x0003E5C0 },
	{ 0x00000000, 0x79E9F233, 0xC80E6067, 0xFA970217, 0x613DDF80, 0xB626A692, 0x8B4347CF, 0xF0251268, 0x6ABD4D74, 0xB4B6F5CE, 0x03A5D3C3, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x12000000, 0x00002439, 0x07000000, 0x0003E5C0 },
	{ 0x00000000, 0x997D717D, 0xC80E6067, 0xFA970217, 0xC3A6D0F9, 0xA03486EA, 0x7EC20AE0, 0x24CF75A6, 0x93F31EE5, 0x71C0309F, 0xB868FF53, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x13000000, 0x00002439, 0x08000000, 0x0003E5C0 },
	{ 0x00000000, 0xCB639C22, 0xC80E6067, 0xFA970217, 0x6C37A47F, 0xED797DCC, 0xF4D14170, 0xE65B00B5, 0x353A3237, 0x13DF4B6C, 0xB9DF2BEA, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x14000000, 0x00002439, 0x09000000, 0x0003E5C0 },
	{ 0x00000000, 0x03E0EFB3, 0xC80E6067, 0xFA970217, 0x50B098BF, 0xEC01983D, 0xAE02140E, 0x1FF84A87, 0x1204EAF6, 0xE3F62478, 0xAEA6E8E8, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x15000000, 0x00002439, 0x0A000000, 0x0003E5C0 },
	{ 0x00000000, 0x62E41A68, 0xC80E6067, 0xFA970217, 0x9C9228F9, 0x4C222A91, 0xEB85DF25, 0x9B912BA7, 0xBA7A5B50, 0xDC02B383, 0x55F68B6C, 0x00000000, 0x00000000, 0xB9070200, 0x6E1A2F29, 0xC42CBD7B, 0x562C2C6E, 0x51CB630A, 0x347AB651, 0x16000000, 0x00002439, 0x0B000000, 0x0003E5C0 },
};

struct stage6replay_ctx {
	sem_t done_sem;
	int nonces_found;
};

static void *stage6replay_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE6REPLAY_TIMEOUT_SEC + 10) * 1000);
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);
	_exit(3);
	return NULL;
}

static void *stage6replay_worker(void *arg)
{
	struct stage6replay_ctx *ctx = arg;
	FILE *fp = fopen(STAGE6REPLAY_LOG, "a");
	FILE *wiretrace_fp = NULL;
	time_t poll_deadline;
	uint32_t reads_attempted = 0, reads_err = 0, reads_ok_empty = 0, reads_ok_data = 0;
	static const uint32_t vmask_table[8] = {
		0x00000020, 0x00E0FF3F, 0x00800020, 0x00000120,
		0x00000220, 0x00000420, 0x00000820, 0x00001020,
	};

	if (!fp)
		return NULL;

	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE6_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);

	/* Full wire trace, disabled a few seconds into the nonce-poll loop
	 * below to bound the file size. */
	wiretrace_fp = fopen("/sharefs/wiretrace_replay.log", "w");
	if (wiretrace_fp)
		uart_set_wiretrace_log(wiretrace_fp);

	/* a3197s_enumerate() assigns each physical chip its address in the daisy
	 * chain; it must run before a3197s_init_chain(), since a3197s_init_chain()'s
	 * broadcast CFG/ECC/PVT-init otherwise only reaches chip 0. */
	{
		uint32_t chip_count = a3197s_enumerate(STAGE6_UART_CHANNEL);

		fprintf(fp, "stage6replay: a3197s_enumerate() chip_count=%u\n", chip_count);
		fflush(fp);
	}

	a3197s_init_chain(STAGE6_UART_CHANNEL);
	delay_ms(100);

	fprintf(fp, "stage6replay: switching UART baud 115200 -> %d\n", UART_HIGH_BAUD_RATE);
	fflush(fp);
	a3197s_set_uart_baud(STAGE6_UART_CHANNEL, UART_HIGH_BAUD_RATE);
	delay_ms(1000);

	fprintf(fp, "stage6replay: starting %ds delay (operator window to kill the stock miner over SSH)\n",
		STAGE6_STARTUP_DELAY_SEC);
	fflush(fp);
	delay_ms(STAGE6_STARTUP_DELAY_SEC * 1000);

	/* Sequence: PLL ramp -> CORE_RST pulse -> noncemask -> TARGET/vmask
	 * -> work -> NONCE_UPDATE. Targets LOW mode only (domain0=220MHz,
	 * domain1=240, domain2=260, domain3=280MHz); voltage must
	 * independently already be at LOW's 3392mV, set via i2cset on the
	 * Linux side. */
	{
		uint32_t pllcnt_before, passcnt_before, failcnt_before;
		uint32_t pllcnt_after, passcnt_after, failcnt_after;

		a3197s_select_chip(STAGE6_UART_CHANNEL, 0);
		pllcnt_before = a3197s_get_reg(STAGE6_UART_CHANNEL, REG_SS_CORE_DIST);
		passcnt_before = a3197s_get_reg(STAGE6_UART_CHANNEL, REG_SS_LOG_PASS);
		failcnt_before = a3197s_get_reg(STAGE6_UART_CHANNEL, REG_SS_LOG_FAIL);
		fprintf(fp, "stage6replay: pre-ramp (chip=0, ~100MHz) PLLCNT1=0x%08x "
			"SPDLOG_PASSCNT=0x%08x SPDLOG_FAILCNT=0x%08x\n",
			pllcnt_before, passcnt_before, failcnt_before);
		fflush(fp);

		fprintf(fp, "stage6replay: starting real gradual PLL ramp "
			"(domain0 100->220MHz [LOW mode, domain3=280MHz], 10MHz/step, "
			"domain interval=20, ~1s/step) -- voltage must already be at "
			"LOW's 3392mV (operator-set via i2cset, not this binary)\n");
		fflush(fp);
		asic_ramp_pll_freq(STAGE6_UART_CHANNEL, 100, 220, 20);
		fprintf(fp, "stage6replay: ramp complete, waiting 5s for a self-test window\n");
		fflush(fp);
		delay_ms(5000);

		a3197s_select_chip(STAGE6_UART_CHANNEL, 0);
		pllcnt_after = a3197s_get_reg(STAGE6_UART_CHANNEL, REG_SS_CORE_DIST);
		passcnt_after = a3197s_get_reg(STAGE6_UART_CHANNEL, REG_SS_LOG_PASS);
		failcnt_after = a3197s_get_reg(STAGE6_UART_CHANNEL, REG_SS_LOG_FAIL);
		fprintf(fp, "stage6replay: post-ramp (chip=0, ~MED) PLLCNT1=0x%08x "
			"SPDLOG_PASSCNT=0x%08x SPDLOG_FAILCNT=0x%08x\n",
			pllcnt_after, passcnt_after, failcnt_after);
		fflush(fp);
	}

	/* CORE_RST=1 pulse right after the ramp, before reconfiguring
	 * noncemask. */
	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_CORE_RST, 1);

	asic_set_noncemask(STAGE6_UART_CHANNEL, 0x00000001);

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0x00000000);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, 0x000FFFF0);

	{
		int i;

		for (i = 0; i < 8; i++)
			a3197s_set_reg(STAGE6_UART_CHANNEL, REG_VMASK_TABLE + i * 4, vmask_table[i]);
	}

	/* Per-chip addressed work write, with per-chip nonce2 values and an
	 * immediate log+fflush after each so a hang is localized to a
	 * specific chip. */
	{
		uint16_t chip;

		for (chip = 0; chip < STAGE6_CHIP_COUNT; chip++) {
			fprintf(fp, "stage6replay: addressed work write chip=%u starting\n", chip);
			fflush(fp);
			a3197s_select_chip(STAGE6_UART_CHANNEL, chip);
			asic_raw_write_words(STAGE6_UART_CHANNEL, REG_WORK, g_real_job_words[chip], 23);
			fprintf(fp, "stage6replay: addressed work write chip=%u returned\n", chip);
			fflush(fp);
		}
	}

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	fprintf(fp, "stage6replay: real per-chip job replayed (job_id wire=0x00002439), ground truth: "
		"chip=1 nonce2=0x24030000 nonce=0xB23F1709\n");
	fflush(fp);

	/* Read-only poll for the whole window; no fabricated nonce2
	 * increments (the chip's own nonce2 rolls internally). A returned
	 * nonce is verifiable against a captured ground-truth merkle root
	 * only if its nonce2 matches the chip's original base value. */
	fprintf(fp, "stage6replay: polling nonce buffer for %ds, READ-ONLY (no fabricated "
		"nonce2 increments) -- letting the chip's own internal nonce2 rolling do the work "
		"against each chip's real captured job\n", STAGE6REPLAY_POLL_SEC);
	fflush(fp);

	poll_deadline = time(NULL) + STAGE6REPLAY_POLL_SEC;
	{
		time_t wiretrace_stop_time = time(NULL) + 15;

		while (time(NULL) < poll_deadline && ctx->nonces_found < STAGE6REPLAY_MAX_NONCES) {
		uint16_t chip;

		if (wiretrace_fp && time(NULL) >= wiretrace_stop_time) {
			uart_set_wiretrace_log(NULL);
			fclose(wiretrace_fp);
			wiretrace_fp = NULL;
		}

		for (chip = 0; chip < STAGE6_CHIP_COUNT && ctx->nonces_found < STAGE6REPLAY_MAX_NONCES; chip++) {
			struct asic_nonce_record nonces[NONCE_RECORD_MAX];
			uint8_t nonce_count = 0;
			int get_ret;

			a3197s_select_chip(STAGE6_UART_CHANNEL, chip);
			reads_attempted++;
			get_ret = asic_get_nonce(STAGE6_UART_CHANNEL, chip, nonces, &nonce_count);
			if (get_ret != 0) {
				reads_err++;
			} else if (nonce_count == 0) {
				reads_ok_empty++;
			} else {
				uint8_t n;

				reads_ok_data++;
				for (n = 0; n < nonce_count && ctx->nonces_found < STAGE6REPLAY_MAX_NONCES; n++) {
					ctx->nonces_found++;
					fprintf(fp, "stage6replay: NONCE#%d chip=%u job_id=0x%08x nonce2=0x%08x "
						"nonce=0x%08x asic_id=%u miner_id=%u mid_id=%u valid=%u%s\n",
						ctx->nonces_found, chip, nonces[n].job_id, nonces[n].nonce2,
						nonces[n].nonce, (unsigned)nonces[n].asic_id,
						(unsigned)nonces[n].miner_id, (unsigned)nonces[n].mid_id,
						(unsigned)nonces[n].valid,
						(nonces[n].nonce2 == g_real_job_words[chip][19]) ?
							" <<< matches this chip's real captured base nonce2 -- "
							"verifiable against the real captured header" :
							" (rolled nonce2 -- no captured merkle root, not verifiable)");
					fflush(fp);
				}
			}
			delay_ms(30);
		}
		}
	}

	if (wiretrace_fp) {
		uart_set_wiretrace_log(NULL);
		fclose(wiretrace_fp);
		wiretrace_fp = NULL;
	}

	fprintf(fp, "stage6replay: poll done. nonces_found=%d reads_attempted=%u reads_err=%u "
		"reads_ok_empty=%u reads_ok_data=%u\n",
		ctx->nonces_found, reads_attempted, reads_err, reads_ok_empty, reads_ok_data);
	fclose(fp);
	sem_post(&ctx->done_sem);
	return NULL;
}

static int run_stage6ctx_replay(void)
{
	struct stage6replay_ctx *ctx;
	pthread_t tid, watchdog_tid;
	struct timespec deadline;
	int rc;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	/* Settle delay before this stage's own RST pulse. */
	delay_ms(3000);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE6_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage6replay_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);

	if (pthread_create(&tid, NULL, stage6replay_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE6REPLAY_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	sem_destroy(&ctx->done_sem);
	free(ctx);

	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);

	return 0;
}

/* Isolation test: runs the same RST/enum/config/ramp/CORE_RST/
 * noncemask/TARGET/vmask/per-chip-WORK sequence as --stage6replay, polls
 * the nonce buffer with no IPC connection open (phase 1), then opens a
 * real IPC connection and polls again (phase 2), to isolate whether an
 * active IPC connection affects nonce polling. */
#define STAGE6ISO_LOG "/sharefs/stage6iso.log"
#define STAGE6ISO_PHASE_POLL_SEC 12
#define STAGE6ISO_CONNECT_BUDGET_SEC 30
#define STAGE6ISO_TIMEOUT_SEC (STAGE6REPLAY_RAMP_BUDGET_SEC + STAGE6ISO_PHASE_POLL_SEC * 2 + \
	STAGE6ISO_CONNECT_BUDGET_SEC + 30)

struct stage6iso_ctx {
	sem_t done_sem;
};

static void *stage6iso_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE6ISO_TIMEOUT_SEC + 10) * 1000);
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);
	_exit(3);
	return NULL;
}

static void stage6iso_poll_phase(FILE *fp, const char *phase_label, int seconds)
{
	time_t deadline = time(NULL) + seconds;
	uint32_t before_tmo[STAGE6_CHIP_COUNT], before_hb[STAGE6_CHIP_COUNT], before_data[STAGE6_CHIP_COUNT];
	uint16_t c;

	for (c = 0; c < STAGE6_CHIP_COUNT; c++)
		asic_get_nonce_chip_stat(STAGE6_UART_CHANNEL, c, &before_tmo[c], &before_hb[c], &before_data[c]);

	while (time(NULL) < deadline) {
		uint16_t chip;

		for (chip = 0; chip < STAGE6_CHIP_COUNT; chip++) {
			struct asic_nonce_record nonces[NONCE_RECORD_MAX];
			uint8_t nonce_count = 0;

			a3197s_select_chip(STAGE6_UART_CHANNEL, chip);
			(void)asic_get_nonce(STAGE6_UART_CHANNEL, chip, nonces, &nonce_count);
			delay_ms(30);
		}
	}

	fprintf(fp, "stage6iso: phase=%s breakdown (T=timeout H=heartbeat D=data, this phase only):", phase_label);
	for (c = 0; c < STAGE6_CHIP_COUNT; c++) {
		uint32_t after_tmo, after_hb, after_data;

		asic_get_nonce_chip_stat(STAGE6_UART_CHANNEL, c, &after_tmo, &after_hb, &after_data);
		fprintf(fp, " chip%u=T%u,H%u,D%u", c,
			after_tmo - before_tmo[c], after_hb - before_hb[c], after_data - before_data[c]);
	}
	fprintf(fp, "\n");
	fflush(fp);
}

static void *stage6iso_worker(void *arg)
{
	struct stage6iso_ctx *ctx = arg;
	FILE *fp = fopen(STAGE6ISO_LOG, "a");
	static const uint32_t vmask_table[8] = {
		0x00000020, 0x00E0FF3F, 0x00800020, 0x00000120,
		0x00000220, 0x00000420, 0x00000820, 0x00001020,
	};
	int ipc_id;

	if (!fp)
		return NULL;

	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE6_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);

	{
		uint32_t chip_count = a3197s_enumerate(STAGE6_UART_CHANNEL);

		fprintf(fp, "stage6iso: a3197s_enumerate() chip_count=%u\n", chip_count);
		fflush(fp);
	}

	a3197s_init_chain(STAGE6_UART_CHANNEL);
	delay_ms(100);

	fprintf(fp, "stage6iso: switching UART baud 115200 -> %d\n", UART_HIGH_BAUD_RATE);
	fflush(fp);
	a3197s_set_uart_baud(STAGE6_UART_CHANNEL, UART_HIGH_BAUD_RATE);
	delay_ms(1000);

	fprintf(fp, "stage6iso: starting real gradual PLL ramp (domain0 100->220MHz [LOW mode])\n");
	fflush(fp);
	asic_ramp_pll_freq(STAGE6_UART_CHANNEL, 100, 220, 20);
	delay_ms(5000);

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_CORE_RST, 1);
	asic_set_noncemask(STAGE6_UART_CHANNEL, 0x00000001);

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0x00000000);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, 0x000FFFF0);
	{
		int i;

		for (i = 0; i < 8; i++)
			a3197s_set_reg(STAGE6_UART_CHANNEL, REG_VMASK_TABLE + i * 4, vmask_table[i]);
	}

	{
		uint16_t chip;

		for (chip = 0; chip < STAGE6_CHIP_COUNT; chip++) {
			a3197s_select_chip(STAGE6_UART_CHANNEL, chip);
			asic_raw_write_words(STAGE6_UART_CHANNEL, REG_WORK, g_real_job_words[chip], 23);
		}
	}

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	fprintf(fp, "stage6iso: real per-chip job applied (same as --stage6replay), "
		"entering phase 1 (no IPC connection at all)\n");
	fflush(fp);

	stage6iso_poll_phase(fp, "1_no_ipc", STAGE6ISO_PHASE_POLL_SEC);

	fprintf(fp, "stage6iso: phase 1 done, opening IPC connection (harness must already be "
		"running and retrying its own connect)\n");
	fflush(fp);

	ipc_id = ipc_link_open(STAGE5_SERVICE_NAME, STAGE5_RTOS_TARGET, STAGE5_PORT);
	fprintf(fp, "stage6iso: ipc_link_open() returned id=%d\n", ipc_id);
	fflush(fp);

	if (ipc_id >= 0) {
		fprintf(fp, "stage6iso: entering phase 2 (IPC connected, recv thread live)\n");
		fflush(fp);
		stage6iso_poll_phase(fp, "2_ipc_connected", STAGE6ISO_PHASE_POLL_SEC);
		ipc_link_close(ipc_id, STAGE5_SERVICE_NAME);
	} else {
		fprintf(fp, "stage6iso: IPC connect failed/timed out, skipping phase 2\n");
		fflush(fp);
	}

	fprintf(fp, "stage6iso: done\n");
	fclose(fp);
	sem_post(&ctx->done_sem);
	return NULL;
}

static int run_stage6ctx_iso(void)
{
	struct stage6iso_ctx *ctx;
	pthread_t tid, watchdog_tid;
	struct timespec deadline;
	int rc;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	delay_ms(3000);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE6_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage6iso_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);

	if (pthread_create(&tid, NULL, stage6iso_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE6ISO_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	sem_destroy(&ctx->done_sem);
	free(ctx);

	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);

	return 0;
}

/* Same as stage6iso_worker() except the per-chip WORK write uses
 * a3197s_submit_job() (the function stage6_worker() calls) with a
 * synthetic job, instead of asic_raw_write_words()+g_real_job_words[]. */
static void *stage6iso2_worker(void *arg)
{
	struct stage6iso_ctx *ctx = arg;
	FILE *fp = fopen(STAGE6ISO_LOG, "a");
	a3197s_job_t job;
	int ipc_id;

	if (!fp)
		return NULL;

	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE6_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);

	{
		uint32_t chip_count = a3197s_enumerate(STAGE6_UART_CHANNEL);

		fprintf(fp, "stage6iso2: a3197s_enumerate() chip_count=%u\n", chip_count);
		fflush(fp);
	}

	a3197s_init_chain(STAGE6_UART_CHANNEL);
	delay_ms(100);

	fprintf(fp, "stage6iso2: switching UART baud 115200 -> %d\n", UART_HIGH_BAUD_RATE);
	fflush(fp);
	a3197s_set_uart_baud(STAGE6_UART_CHANNEL, UART_HIGH_BAUD_RATE);
	delay_ms(1000);

	fprintf(fp, "stage6iso2: starting real gradual PLL ramp (domain0 100->220MHz [LOW mode])\n");
	fflush(fp);
	asic_ramp_pll_freq(STAGE6_UART_CHANNEL, 100, 220, 20);
	delay_ms(5000);

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_CORE_RST, 1);
	asic_set_noncemask(STAGE6_UART_CHANNEL, 0x00000001);

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0xffffffff);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, 0xffffffff);

	/* Synthetic job: valid header/coinbase/vmask, maximally-easy
	 * target, consumed by a3197s_submit_job() via a3197s_set_job(). */
	memset(&job, 0, sizeof(job));
	job.job_id = STAGE6NONCE_DUMMY_JOB_ID;
	job.coinbase_len = STAGE6NONCE_COINBASE_LEN;
	memset(job.coinbase, 0x42, STAGE6NONCE_COINBASE_LEN);
	job.nonce2 = 0;
	job.nonce2_offset = 4;
	job.nonce2_size = 4;
	job.merkle_offset = STAGE6NONCE_MERKLE_OFFSET;
	job.nmerkles = 0;
	job.header[0] = 0x01;
	memset(job.header + 4, 0xAA, 32);
	*(uint32_t *)(job.header + 68) = 0x5f5e1000;
	*(uint32_t *)(job.header + 72) = 0x1d00ffff;
	job.work_restart = 1;
	job.vmask[0] = 0x00000020;
	job.vmask[1] = 0x00E0FF3F;
	job.vmask[2] = 0x00800020;
	job.vmask[3] = 0x00000120;
	job.vmask[4] = 0x00000220;
	job.vmask[5] = 0x00000420;
	job.vmask[6] = 0x00000820;
	job.vmask[7] = 0x00001020;
	a3197s_set_job(&job);

	{
		uint16_t chip;

		for (chip = 0; chip < STAGE6_CHIP_COUNT; chip++) {
			a3197s_select_chip(STAGE6_UART_CHANNEL, chip);
			a3197s_submit_job(STAGE6_UART_CHANNEL, chip, 0);
		}
	}

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_NONCE_UPDATE, 0x80000000);
	fprintf(fp, "stage6iso2: job applied via a3197s_submit_job() per-chip (dummy job_id=0x%08x), "
		"entering phase 1 (no IPC connection at all)\n", job.job_id);
	fflush(fp);

	stage6iso_poll_phase(fp, "1_no_ipc_tsw", STAGE6ISO_PHASE_POLL_SEC);

	fprintf(fp, "stage6iso2: phase 1 done, opening IPC connection (harness must already be "
		"running and retrying its own connect)\n");
	fflush(fp);

	ipc_id = ipc_link_open(STAGE5_SERVICE_NAME, STAGE5_RTOS_TARGET, STAGE5_PORT);
	fprintf(fp, "stage6iso2: ipc_link_open() returned id=%d\n", ipc_id);
	fflush(fp);

	if (ipc_id >= 0) {
		fprintf(fp, "stage6iso2: entering phase 2 (IPC connected, recv thread live)\n");
		fflush(fp);
		stage6iso_poll_phase(fp, "2_ipc_connected_tsw", STAGE6ISO_PHASE_POLL_SEC);
		ipc_link_close(ipc_id, STAGE5_SERVICE_NAME);
	} else {
		fprintf(fp, "stage6iso2: IPC connect failed/timed out, skipping phase 2\n");
		fflush(fp);
	}

	fprintf(fp, "stage6iso2: done\n");
	fclose(fp);
	sem_post(&ctx->done_sem);
	return NULL;
}

static int run_stage6ctx_iso2(void)
{
	struct stage6iso_ctx *ctx;
	pthread_t tid, watchdog_tid;
	struct timespec deadline;
	int rc;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	delay_ms(3000);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE6_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage6iso_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);

	if (pthread_create(&tid, NULL, stage6iso2_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE6ISO_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	sem_destroy(&ctx->done_sem);
	free(ctx);

	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);

	return 0;
}

/* Diagnostic-only: write-then-readback audit of the registers job/nonce
 * tests depend on (TARGETL/TARGETH, 0x4E0, REG_WORK), to check whether
 * writes to these registers actually stick. No IPC dependency. */
#define STAGE6WCHECK_LOG "/sharefs/stage6wcheck.log"
#define STAGE6WCHECK_TIMEOUT_SEC 30

static void *stage6wcheck_hard_watchdog(void *unused)
{
	(void)unused;
	delay_ms((STAGE6WCHECK_TIMEOUT_SEC + 10) * 1000);
	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);
	_exit(3);
	return NULL;
}

struct stage6wcheck_ctx {
	sem_t done_sem;
};

static void *stage6wcheck_worker(void *arg)
{
	struct stage6wcheck_ctx *ctx = arg;
	FILE *fp = fopen(STAGE6WCHECK_LOG, "a");
	uint32_t rb;
	int i;

	if (!fp)
		return NULL;

	gpio_asic_rst_init();
	gpio_asic_rst_deassert();
	delay_ms(5);
	gpio_asic_rst_assert();
	delay_ms(10);
	gpio_asic_rst_deassert();
	delay_ms(100);

	uart_init(STAGE6_UART_CHANNEL, UART_DEFAULT_BAUD_RATE);
	a3197s_init_chain(STAGE6_UART_CHANNEL);
	delay_ms(100);

	fprintf(fp, "stage6wcheck: switching UART baud 115200 -> %d\n", UART_HIGH_BAUD_RATE);
	fflush(fp);
	a3197s_set_uart_baud(STAGE6_UART_CHANNEL, UART_HIGH_BAUD_RATE);
	delay_ms(1000);

	/* Settle delay after the baud switch. */
	fprintf(fp, "stage6wcheck: %ds settle delay (matches other stages' post-baud-switch wait)\n",
		STAGE6_STARTUP_DELAY_SEC);
	fflush(fp);
	delay_ms(STAGE6_STARTUP_DELAY_SEC * 1000);

	/* Baseline sanity check: PLLCNT1 on chip 0. */
	{
		int ret;

		a3197s_select_chip(STAGE6_UART_CHANNEL, 0);
		ret = a3197s_get_reg_ex(STAGE6_UART_CHANNEL, REG_SS_CORE_DIST, &rb);
		fprintf(fp, "stage6wcheck: BASELINE PLLCNT1 chip=0 read=0x%08x ret=%d "
			"(sanity check -- this register is known to read successfully "
			"at this point in --stage6replay runs)\n", rb, ret);
		fflush(fp);
	}

	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);

	/* Test 1: TARGETL/TARGETH write-then-readback. Uses
	 * a3197s_get_reg_ex() so a failed read (ret != 0) is distinguishable
	 * from a read that returned a mismatching value. */
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0x00000000);
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_HI, 0x000FFFF0);
	{
		int ret = a3197s_get_reg_ex(STAGE6_UART_CHANNEL, REG_TARGET_LO, &rb);
		fprintf(fp, "stage6wcheck: TARGETL wrote=0x00000000 read=0x%08x ret=%d %s\n",
			rb, ret, (ret == 0 && rb == 0x00000000) ? "MATCH" : "MISMATCH");
		ret = a3197s_get_reg_ex(STAGE6_UART_CHANNEL, REG_TARGET_HI, &rb);
		fprintf(fp, "stage6wcheck: TARGETH wrote=0x000FFFF0 read=0x%08x ret=%d %s\n",
			rb, ret, (ret == 0 && rb == 0x000FFFF0) ? "MATCH" : "MISMATCH");
		fflush(fp);
	}

	/* Test 1b: a distinguishing bit pattern, to rule out
	 * a3197s_get_reg_ex() echoing a fixed/stale value. */
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0xDEADBEEF);
	{
		int ret = a3197s_get_reg_ex(STAGE6_UART_CHANNEL, REG_TARGET_LO, &rb);
		fprintf(fp, "stage6wcheck: TARGETL wrote=0xDEADBEEF read=0x%08x ret=%d %s\n",
			rb, ret, (ret == 0 && rb == 0xDEADBEEF) ? "MATCH" : "MISMATCH");
		fflush(fp);
	}
	a3197s_set_reg(STAGE6_UART_CHANNEL, REG_TARGET_LO, 0x00000000);

	/* Test 2: 0x4E0 write/readback, chip-0-addressed and broadcast,
	 * logging the raw returned word and transport ret code. */
	{
		uint32_t tmp32 = 0xFFFFFFFE;
		int ret;

		a3197s_select_chip(STAGE6_UART_CHANNEL, 0);
		a3197s_set_reg(STAGE6_UART_CHANNEL, 0x4e0, tmp32);
		ret = a3197s_get_reg_ex(STAGE6_UART_CHANNEL, 0x4e0, &rb);
		fprintf(fp, "stage6wcheck: 0x4E0 chip=0 wrote=0x%08x read=0x%08x ret=%d %s\n",
			tmp32, rb, ret, (ret == 0 && rb == tmp32) ? "MATCH" : "MISMATCH");
		fflush(fp);

		a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
		a3197s_set_reg(STAGE6_UART_CHANNEL, 0x4e0, tmp32);
		ret = a3197s_get_reg_ex(STAGE6_UART_CHANNEL, 0x4e0, &rb);
		fprintf(fp, "stage6wcheck: 0x4E0 broadcast wrote=0x%08x read=0x%08x ret=%d %s\n",
			tmp32, rb, ret, (ret == 0 && rb == tmp32) ? "MATCH" : "MISMATCH");
		fflush(fp);

		tmp32 = 0xFFFFFFFF;
		a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
		a3197s_set_reg(STAGE6_UART_CHANNEL, 0x4e0, tmp32);
	}

	/* Test 3: real job burst write-then-readback via
	 * asic_raw_write_words() to REG_WORK, reading back the first 3
	 * words and comparing against what was sent. */
	a3197s_select_chip(STAGE6_UART_CHANNEL, CHIP_ADDR_BROADCAST);
	asic_raw_write_words(STAGE6_UART_CHANNEL, REG_WORK, g_real_job_words[1], 23);
	for (i = 0; i < 3; i++) {
		int ret = a3197s_get_reg_ex(STAGE6_UART_CHANNEL, REG_WORK + i * 4, &rb);
		fprintf(fp, "stage6wcheck: WORK[%d] wrote=0x%08x read=0x%08x ret=%d %s\n",
			i, g_real_job_words[1][i], rb, ret,
			(ret == 0 && rb == g_real_job_words[1][i]) ? "MATCH" : "MISMATCH");
		fflush(fp);
	}

	fprintf(fp, "stage6wcheck: audit complete\n");
	fclose(fp);
	sem_post(&ctx->done_sem);
	return NULL;
}

static int run_stage6ctx_wcheck(void)
{
	struct stage6wcheck_ctx *ctx;
	pthread_t tid, watchdog_tid;
	struct timespec deadline;
	int rc;

	ctx = malloc(sizeof(*ctx));
	if (!ctx)
		return 1;
	memset(ctx, 0, sizeof(*ctx));
	sem_init(&ctx->done_sem, 0, 0);

	gpio_init(HBOARD_COUNT);
	ENTER_CONFIG_MODE(STAGE6_UART_CHANNEL);
	delay_ms(3);

	if (pthread_create(&watchdog_tid, NULL, stage6wcheck_hard_watchdog, NULL) == 0)
		pthread_detach(watchdog_tid);

	if (pthread_create(&tid, NULL, stage6wcheck_worker, ctx) != 0) {
		sem_destroy(&ctx->done_sem);
		free(ctx);
		return 1;
	}

	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += STAGE6WCHECK_TIMEOUT_SEC;

	do {
		rc = sem_timedwait(&ctx->done_sem, &deadline);
	} while (rc != 0 && errno == EINTR);

	if (rc != 0) {
		pthread_cancel(tid);
		pthread_detach(tid);
		return 1;
	}

	pthread_join(tid, NULL);
	sem_destroy(&ctx->done_sem);
	free(ctx);

	gpio_asic_rst_deassert();
	ENTER_WORK_MODE(STAGE6_UART_CHANNEL);

	return 0;
}

int main(int argc, char **argv)
{
	int ret;
	int stage1 = 0;
	int stage2 = 0;
	int stage2b = 0;
	int stage3 = 0;
	int stage4 = 0;
	int stage5 = 0;
	int stage6 = 0;
	int stage6loop = 0;
	int stage6pvt = 0;
	int stage6nonce = 0;
	int stage6replay = 0;
	int stage6iso = 0;
	int stage6iso2 = 0;
	int stage6wcheck = 0;
	int shatest = 0;
	int safeidle = 0;

	disable_kernel_watchdog();

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--safeidle"))
			safeidle = 1;
		if (!strcmp(argv[i], "--enum"))
			stage1 = 1;
		if (!strcmp(argv[i], "--pvt"))
			stage2 = 1;
		if (!strcmp(argv[i], "--startpll"))
			stage2b = 1;
		if (!strcmp(argv[i], "--rsttest"))
			stage3 = 1;
		if (!strcmp(argv[i], "--work4"))
			stage4 = 1;
		if (!strcmp(argv[i], "--ipc5"))
			stage5 = 1;
		if (!strcmp(argv[i], "--ipc6"))
			stage6 = 1;
		if (!strcmp(argv[i], "--ipc6loop"))
			stage6loop = 1;
		if (!strcmp(argv[i], "--stage6pvt"))
			stage6pvt = 1;
		if (!strcmp(argv[i], "--stage6nonce"))
			stage6nonce = 1;
		if (!strcmp(argv[i], "--stage6replay"))
			stage6replay = 1;
		if (!strcmp(argv[i], "--stage6iso"))
			stage6iso = 1;
		if (!strcmp(argv[i], "--stage6iso2"))
			stage6iso2 = 1;
		if (!strcmp(argv[i], "--stage6wcheck"))
			stage6wcheck = 1;
		if (!strcmp(argv[i], "--shatest"))
			shatest = 1;
		if (!strcmp(argv[i], "--predelaysec") && i + 1 < argc)
			g_predelay_sec = atoi(argv[++i]);
	}

	/* --predelaysec=N: delays startup by N seconds via delay_ms(). */
	if (g_predelay_sec > 0)
		delay_ms(g_predelay_sec * 1000);

	/* --safeidle asserts RST and exits, never combined with another stage. */
	if (safeidle)
		return run_stage_safeidle();

#ifdef ENABLE_STAGE1_ENUM
	stage1 = 1;
#endif
#ifdef ENABLE_STAGE2_PVT
	stage2 = 1;
#endif
#ifdef ENABLE_STAGE2B_STARTPLL
	stage2b = 1;
#endif
#ifdef ENABLE_STAGE3_RSTTEST
	stage3 = 1;
#endif
#ifdef ENABLE_STAGE4_WORK
	stage4 = 1;
#endif
#ifdef ENABLE_STAGE5_IPC
	stage5 = 1;
#endif
#ifdef ENABLE_STAGE6_MINING
	stage6 = 1;
#endif
#ifdef ENABLE_SHATEST
	shatest = 1;
#endif

	ret = run_stage0();
	if (ret != 0)
		return ret;

	if (stage1) {
		ret = run_stage1_enum();
		if (ret != 0)
			return ret;
	}

	if (stage2) {
		ret = run_stage2_pvt();
		if (ret != 0)
			return ret;
	}

	if (stage2b) {
		ret = run_stage2b_startpll_pvt();
		if (ret != 0)
			return ret;
	}

	if (stage3) {
		ret = run_stage3_reset_test();
		if (ret != 0)
			return ret;
	}

	if (stage4) {
		ret = run_stage4_work();
		if (ret != 0)
			return ret;
	}

	if (stage5) {
		/* Retries run_stage5_ipc() for a few minutes, giving a manual-
		 * intervention window instead of a single race against boot
		 * timing. */
		time_t stage5_deadline = time(NULL) + 240;

		do {
			ret = run_stage5_ipc();
			if (ret == 0)
				break;
			sleep(5);
		} while (time(NULL) < stage5_deadline);
		if (ret != 0)
			return ret;
	}

	if (stage6) {
		/* Single attempt: a Stage 6 attempt can itself take minutes,
		 * so it is not wrapped in a retry loop. */
		ret = run_stage6_ipc_mining();
		if (ret != 0)
			return ret;
	}

	if (stage6loop) {
		/* Re-attempts run_stage6_ipc_mining() only after it returns
		 * (a healthy run does not return, since mining continues
		 * indefinitely). Runs forever; a short delay between cycles
		 * avoids hammering the pool/IPC rendezvous on persistent
		 * failure. */
		for (;;) {
			ret = run_stage6_ipc_mining();
			fprintf(stderr, "--ipc6loop: cycle ended, ret=%d, restarting in 5s\n", ret);
			delay_ms(5000);
		}
	}

	if (stage6pvt) {
		ret = run_stage6ctx_pvt();
		if (ret != 0)
			return ret;
	}

	if (stage6nonce) {
		ret = run_stage6ctx_nonce();
		if (ret != 0)
			return ret;
	}

	if (stage6replay) {
		ret = run_stage6ctx_replay();
		if (ret != 0)
			return ret;
	}

	if (stage6iso) {
		ret = run_stage6ctx_iso();
		if (ret != 0)
			return ret;
	}

	if (stage6iso2) {
		ret = run_stage6ctx_iso2();
		if (ret != 0)
			return ret;
	}

	if (stage6wcheck) {
		ret = run_stage6ctx_wcheck();
		if (ret != 0)
			return ret;
	}

	if (shatest)
		return run_shatest();

	return 0;
}

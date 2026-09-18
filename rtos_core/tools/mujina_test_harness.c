/*
 * Linux-side hardware-control harness for power_en (GPIO34) and the fan
 * (pwmchip0/pwm4). Polls HARNESS_CONTROL_FILE for pause/resume/fan
 * commands and drives the GPIO/PWM peripherals accordingly. Also contains
 * an optional C port of the LCD "nano3s" rendering pages, enabled via
 * HARNESS_RENDER_ENABLE=1.
 */
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* GPIO pin controlling chip power enable. */
#define POWER_EN_GPIO 34

/* Fan PWM channel: pwmchip0/pwm3, NOT pwm4. Corrected 2026-08-09: the
 * byte-identical stock mm_miner binary's fan_init() (verified via
 * disassembly -- extracted rodata format strings "/sys/class/pwm/
 * pwmchip%d/pwm%d/..." plus a full register trace of the sprintf/write
 * call sites) targets chip=0, channel=3, real duty=10000/40000=25% at
 * startup, non-inverted. A live duty sweep on pwm3 at fixed 25kHz with
 * pwm4 frozen out confirmed genuine proportional response (25%->1620rpm,
 * 50%->3390rpm, 75%->5010rpm, 90%->5970rpm -- matches a real user memory
 * of "50% -> ~3000rpm" under stock cgminer almost exactly), while the
 * equivalent sweep on pwm4 is flat regardless of duty. pwm3 was
 * previously believed to be the LCD backlight (backlight_bringup.sh);
 * that attribution is now suspect -- see the comment there. */
#define FAN_PWMCHIP "/sys/class/pwm/pwmchip0"
#define FAN_PWM_N   3
/* PWM period in nanoseconds (25kHz), fixed -- matches stock's real
 * FAN_PERIOD. Duty cycle is the real, confirmed proportional actuator on
 * this channel; period never changes after being set once here. */
#define FAN_PID_PERIOD_NS 40000
/* Floor on commanded duty, applied in fan_apply_duty_percent(). Set to 0
 * (no floor) per explicit user instruction (2026-09-14), after a prior
 * sweep attempt (manual override, 25%->20%->15%..., floor removed) was
 * followed by an unexplained device reboot and a temporarily degraded
 * chain (err_crc spiked to 19003, temp readings stuck at 0.0) that only
 * cleared after a full reboot. That test wasn't conclusive -- an extended
 * chain pause was also in effect at the same time, so low duty specifically
 * was never isolated as the cause -- but it's real enough to record here.
 * The vendor's own FAN_DUTY_MIN (fan.h) is 25; going below that is
 * genuinely uncharacterized on this unit. fan_apply_duty_percent()'s full
 * disable->enable re-init cycle on every change (see its own doc comment)
 * is the main mitigation against a silent stall at low duty. */
#define FAN_DUTY_MIN_PCT 0

/* Fallback duty percent (full speed) used briefly after power-on, before
 * the first PID tick. */
#define FAN_STARTUP_DUTY_PERCENT 100

#define HARNESS_CONTROL_FILE "/tmp/harness_control"
#define POLL_INTERVAL_SEC 1

/* Fan tachometer: a raw pulse-counting timer capture device, not a sysfs
 * RPM node (none exists on this unit). timer5 is this fan's real tach
 * input (timer4 reads nothing -- confirmed against both by a one-off probe
 * tool during bring-up). TMIOC_SET_TIMEOUT arms a capture window in
 * seconds; each read() after sleeping that long returns the pulse count
 * accumulated during the window. Two pulses per revolution is standard
 * for a 4-pin PC fan, so with a 1s window, rpm = pulses * 60 / 2. */
#define FAN_TACH_DEV "/dev/timer5"
#define TMIOC_SET_TIMEOUT _IOW('T', 0x20, int)
#define FAN_TACH_WINDOW_SEC 1
/* Latest fan RPM + duty, written here once per tach window for
 * mujina-minerd to read. Pure Linux-side file (both this harness and
 * mujina-minerd run as ordinary Linux processes on the little core), so
 * /tmp is fine -- unlike rtos_core.elf's status files, nothing here
 * crosses into RT-Smart's separate filesystem namespace. */
#define FAN_STATUS_FILE "/tmp/fan_status"

/* Board/outlet NTC thermistor sysfs node; temp1_input reads millidegrees C.
 * Drives the fan curve's baseline duty (see fan_reactive_check()) as of
 * 2026-09-14. A 2026-08 soak test found it sat at a flat 50.0C while chip
 * temp_max climbed past 80C with the fan still at its floor -- it tracks
 * board ambient, not chip temp, so it's deliberately NOT the only signal:
 * chip temp still gates a hard escalation to full speed independent of
 * what this reads, which is what makes using it as a baseline safe. */
#define OUTLET_TEMP_HWMON "/sys/class/hwmon/hwmon1/temp1_input"

/* Status file mujina-minerd writes every status poll (~200ms) for the LCD
 * renderer, key=value lines including temp_max and the commanded base PLL
 * frequency (pll0). Fan control reads this directly for real chip
 * temperature -- defined here (rather than down with the rest of the LCD
 * rendering code that also uses it) because fan_reactive_check() needs it
 * before that code appears in the file. */
#define NANO3S_LIVE_FILE "/tmp/nano3s_live.txt"

/* Real per-mode temperature limits from this device's own
 * /data/factory/hashrate_cali.ini (format:
 * max_pout_W-temp_limit_C-volt_mV-pll_start_MHz-pll_interval_MHz):
 *   mode0 (LOW):  62-80-3392-210-20  -> pll0=210, limit=80C
 *   mode1 (MED):  95-85-3496-338-20  -> pll0=338, limit=85C
 *   mode2 (HIGH): 133-90-3704-420-20 -> pll0=420, limit=90C
 * Matched to the nearest known pll0 rather than requiring an exact match,
 * so a custom/in-between frequency still gets a sane limit instead of
 * silently getting no protection at all. */
static double nano3s_temp_limit_for_pll0(double pll0)
{
	static const struct { double pll0; double limit_c; } modes[] = {
		{ 210.0, 80.0 },
		{ 338.0, 85.0 },
		{ 420.0, 90.0 },
	};
	size_t i, best = 0;
	double best_dist = 1e18;

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		double dist = pll0 - modes[i].pll0;

		if (dist < 0)
			dist = -dist;
		if (dist < best_dist) {
			best_dist = dist;
			best = i;
		}
	}
	return modes[best].limit_c;
}

/* Reads NANO3S_LIVE_FILE for temp_max and pll0. Returns 1 if both were
 * found, 0 otherwise (file missing/stale, e.g. mujina-minerd not up yet).
 * A small standalone parser rather than the shared kv_store one used by
 * the LCD renderer further down this file -- that machinery isn't defined
 * yet at this point in the file, and fan control only ever needs these
 * two numeric fields. */
static int read_chip_temp_and_pll0(double *temp_max, double *pll0)
{
	FILE *f = fopen(NANO3S_LIVE_FILE, "r");
	char line[128];
	int have_temp = 0, have_pll0 = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		double v;

		if (sscanf(line, "temp_max=%lf", &v) == 1) {
			*temp_max = v;
			have_temp = 1;
		} else if (sscanf(line, "pll0=%lf", &v) == 1) {
			*pll0 = v;
			have_pll0 = 1;
		}
	}
	fclose(f);
	return have_temp && have_pll0;
}

/* Reads OUTLET_TEMP_HWMON, returns degrees C, or -273.0 as an
 * invalid-read sentinel on failure (missing file, unparseable content).
 * Drives the fan curve's baseline (see fan_reactive_check()) -- chip temp
 * separately gates a hard escalation on top of whatever this returns. */
static double read_outlet_temp_c(void)
{
	FILE *f = fopen(OUTLET_TEMP_HWMON, "r");
	long millideg;

	if (!f)
		return -273.0;
	if (fscanf(f, "%ld", &millideg) != 1) {
		fclose(f);
		return -273.0;
	}
	fclose(f);
	return (double)millideg / 1000.0;
}

static void write_sysfs(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");

	if (!f) {
		fprintf(stderr, "[harness] failed to open %s for write\n", path);
		return;
	}
	fputs(val, f);
	fclose(f);
}

static void power_en_set(int on)
{
	char path[64];

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", POWER_EN_GPIO);
	if (access(path, F_OK) != 0) {
		char numbuf[8];

		snprintf(numbuf, sizeof(numbuf), "%d", POWER_EN_GPIO);
		write_sysfs("/sys/class/gpio/export", numbuf);
	}
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", POWER_EN_GPIO);
	write_sysfs(path, "out");
	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", POWER_EN_GPIO);
	write_sysfs(path, on ? "1" : "0");
	fprintf(stderr, "[harness] power_en (GPIO%d) set %s\n", POWER_EN_GPIO, on ? "HIGH" : "LOW");
}

/* Fan power state; speed while ON is modulated continuously by the PID
 * controller below rather than being a discrete level. */
enum fan_power {
	FAN_OFF = 0,
	FAN_ON,
};

static enum fan_power g_fan_power = FAN_OFF;

/* Writes a logical 0-100 duty_cycle percent using an inverted formula
 * (0 raw duty = full speed, ~period raw duty = off) on the WRONG channel
 * (FAN_PWM_N was pwm4 when this was written; the real fan is pwm3, see
 * fan_apply_duty_percent()) with an inverted formula stock's own real
 * fan_init() doesn't use. No longer used -- kept for reference/rollback
 * only. */
__attribute__((unused))
static void fan_set_duty_percent(int logical_percent)
{
	char path[96], pwm_dir[64], numbuf[16];
	int duty_ns;

	if (logical_percent < 0)
		logical_percent = 0;
	if (logical_percent > 100)
		logical_percent = 100;

	snprintf(pwm_dir, sizeof(pwm_dir), "%s/pwm%d", FAN_PWMCHIP, FAN_PWM_N);
	duty_ns = (int)((100.0 - (double)logical_percent) * FAN_PID_PERIOD_NS / 100.0);

	snprintf(path, sizeof(path), "%s/duty_cycle", pwm_dir);
	snprintf(numbuf, sizeof(numbuf), "%d", duty_ns);
	write_sysfs(path, numbuf);
}

/* This fan's real RPM is governed by duty cycle on the CORRECT channel
 * (pwm3, see FAN_PWM_N above) -- confirmed 2026-08-09 by a clean sweep
 * with pwm4 frozen out: 25%->1620rpm, 50%->3390rpm, 75%->5010rpm,
 * 90%->5970rpm, genuinely monotonic. Non-inverted (matches stock's real
 * fan_init(), which writes duty=10000/40000=25% directly, no inversion).
 * Period stays fixed at FAN_PID_PERIOD_NS; duty_cycle is the real,
 * continuous control variable, driven by a proportional+integral
 * controller on chip temp error -- no more discrete steps needed now
 * that the actuator itself is genuinely proportional. */

/* Current commanded duty percent, so fan_tach_loop() (a separate thread)
 * can report it alongside measured RPM without the two loops needing to
 * coordinate directly. Plain int: both sides only ever do whole-word
 * reads/writes, same informal concurrency style already used for
 * g_fan_power etc. elsewhere in this file. -1 means "not yet set". */
static volatile int g_fan_current_duty_pct = -1;

/* Applies a real 0-100 duty percent (clamped to FAN_DUTY_MIN_PCT..100).
 * Always does a full disable -> zero duty -> period -> real duty ->
 * enable sequence, even though enable was already 1 -- a live incident
 * during a HIGH-mode soak test (2026-08-09) showed this fan can silently
 * settle at 0 RPM after a large commanded change made with enable left
 * untouched throughout; a full re-init cycle is what reliably recovered
 * it, so every real change pays that cost rather than risk a silent
 * stall. No-ops if the percent hasn't actually changed, so steady state
 * doesn't re-kick the fan every tick. */
static void fan_apply_duty_percent(int duty_percent)
{
	char path[96], pwm_dir[64], numbuf[16];
	int duty_ns;

	if (duty_percent < FAN_DUTY_MIN_PCT)
		duty_percent = FAN_DUTY_MIN_PCT;
	if (duty_percent > 100)
		duty_percent = 100;

	if (duty_percent == g_fan_current_duty_pct)
		return;

	duty_ns = FAN_PID_PERIOD_NS * duty_percent / 100;

	snprintf(pwm_dir, sizeof(pwm_dir), "%s/pwm%d", FAN_PWMCHIP, FAN_PWM_N);

	snprintf(path, sizeof(path), "%s/enable", pwm_dir);
	write_sysfs(path, "0");

	snprintf(path, sizeof(path), "%s/duty_cycle", pwm_dir);
	write_sysfs(path, "0");

	snprintf(path, sizeof(path), "%s/period", pwm_dir);
	snprintf(numbuf, sizeof(numbuf), "%d", FAN_PID_PERIOD_NS);
	write_sysfs(path, numbuf);

	snprintf(path, sizeof(path), "%s/duty_cycle", pwm_dir);
	snprintf(numbuf, sizeof(numbuf), "%d", duty_ns);
	write_sysfs(path, numbuf);

	snprintf(path, sizeof(path), "%s/enable", pwm_dir);
	write_sysfs(path, "1");

	g_fan_current_duty_pct = duty_percent;
}

/* User-editable outlet-temp -> duty% curve, the fan's baseline in normal
 * operation (see fan_reactive_check()). Points are kept sorted ascending
 * by temp_c so fan_curve_lookup() can linearly interpolate without
 * re-sorting; fan_curve_apply_command() enforces the sort on every write.
 * A separate, independent check in fan_reactive_check() escalates to
 * 100% whenever chip temp crosses the active mode's real tuning limit,
 * regardless of what this curve says -- see that function for why an
 * outlet-only baseline is safe with that escalation in place. */
#define FAN_CURVE_MAX_POINTS 8
struct fan_curve_point {
	double temp_c;
	double duty_pct;
};

/* Defined below, alongside control_poll_loop()'s other CSV parsing. */
static char *csv_next_field(char **cursor);
static struct fan_curve_point g_fan_curve[FAN_CURVE_MAX_POINTS];
static int g_fan_curve_count = 0;

#define FAN_CURVE_CONF_PATH "/data/userconfig/fan_curve.conf"

/* Built-in default, used until the user saves their own via the
 * dashboard. Outlet sits around 50C under normal load on this unit (see
 * OUTLET_TEMP_HWMON's doc comment) -- ramps through the 20-60C range
 * covers the realistic operating band, reaching full speed by 60C well
 * before the escalation path would ever need to intervene. */
static const struct fan_curve_point FAN_CURVE_DEFAULT[] = {
	{ 20.0, 0.0 },
	{ 35.0, 15.0 },
	{ 45.0, 35.0 },
	{ 55.0, 65.0 },
	{ 60.0, 100.0 },
};
#define FAN_CURVE_DEFAULT_COUNT \
	(int)(sizeof(FAN_CURVE_DEFAULT) / sizeof(FAN_CURVE_DEFAULT[0]))

static void fan_curve_load_default(void)
{
	int i;

	for (i = 0; i < FAN_CURVE_DEFAULT_COUNT; i++)
		g_fan_curve[i] = FAN_CURVE_DEFAULT[i];
	g_fan_curve_count = FAN_CURVE_DEFAULT_COUNT;
}

static void fan_curve_save(void)
{
	FILE *f = fopen(FAN_CURVE_CONF_PATH, "w");
	int i;

	if (!f) {
		fprintf(stderr, "[harness] fan curve: failed to open %s for write\n",
			FAN_CURVE_CONF_PATH);
		return;
	}
	for (i = 0; i < g_fan_curve_count; i++)
		fprintf(f, "%.1f,%.1f\n", g_fan_curve[i].temp_c, g_fan_curve[i].duty_pct);
	fclose(f);
}

/* Loads the persisted curve at startup, or falls back to (and persists)
 * the built-in default if no saved file exists yet or it's unparseable. */
static void fan_curve_load(void)
{
	FILE *f = fopen(FAN_CURVE_CONF_PATH, "r");
	char line[64];
	int count = 0;

	if (!f) {
		fan_curve_load_default();
		fan_curve_save();
		fprintf(stderr, "[harness] fan curve: no saved curve at %s -- using built-in default (%d points)\n",
			FAN_CURVE_CONF_PATH, g_fan_curve_count);
		return;
	}
	while (count < FAN_CURVE_MAX_POINTS && fgets(line, sizeof(line), f)) {
		double t, d;

		if (sscanf(line, "%lf,%lf", &t, &d) == 2) {
			g_fan_curve[count].temp_c = t;
			g_fan_curve[count].duty_pct = d;
			count++;
		}
	}
	fclose(f);
	if (count == 0) {
		fan_curve_load_default();
		fprintf(stderr, "[harness] fan curve: %s unparseable -- using built-in default\n",
			FAN_CURVE_CONF_PATH);
	} else {
		g_fan_curve_count = count;
		fprintf(stderr, "[harness] fan curve: loaded %d point(s) from %s\n",
			count, FAN_CURVE_CONF_PATH);
	}
}

/* Linear interpolation over g_fan_curve. Below the first point's temp,
 * clamps to the first point's duty; above the last point's temp, clamps
 * to the last point's duty -- never extrapolates past the configured
 * range. */
static double fan_curve_lookup(double temp_c)
{
	int i;

	if (g_fan_curve_count == 0)
		return 100.0; /* shouldn't happen -- fail safe if it ever does */
	if (temp_c <= g_fan_curve[0].temp_c)
		return g_fan_curve[0].duty_pct;
	if (temp_c >= g_fan_curve[g_fan_curve_count - 1].temp_c)
		return g_fan_curve[g_fan_curve_count - 1].duty_pct;

	for (i = 0; i < g_fan_curve_count - 1; i++) {
		double t0 = g_fan_curve[i].temp_c, t1 = g_fan_curve[i + 1].temp_c;

		if (temp_c >= t0 && temp_c <= t1) {
			double d0 = g_fan_curve[i].duty_pct, d1 = g_fan_curve[i + 1].duty_pct;
			double frac = (t1 > t0) ? (temp_c - t0) / (t1 - t0) : 0.0;

			return d0 + frac * (d1 - d0);
		}
	}
	return g_fan_curve[g_fan_curve_count - 1].duty_pct;
}

/* Parses "<t1>:<d1>,<t2>:<d2>,..." (2-FAN_CURVE_MAX_POINTS points),
 * validates and sorts them ascending by temp_c, then replaces the live
 * curve and persists it. Malformed input or an out-of-range value leaves
 * the current curve untouched entirely (no partial apply). */
static void fan_curve_apply_command(char *spec)
{
	struct fan_curve_point points[FAN_CURVE_MAX_POINTS];
	int count = 0;
	char *cursor = spec;

	while (count < FAN_CURVE_MAX_POINTS && *cursor) {
		char *field = csv_next_field(&cursor);
		char *colon = strchr(field, ':');
		double t, d;

		if (!colon) {
			fprintf(stderr, "[harness] fan curve: malformed point '%s' -- ignoring whole command\n", field);
			return;
		}
		*colon = '\0';
		t = atof(field);
		d = atof(colon + 1);
		if (t < 0.0 || t > 150.0 || d < 0.0 || d > 100.0) {
			fprintf(stderr, "[harness] fan curve: point out of range (t=%.1f d=%.1f) -- ignoring whole command\n", t, d);
			return;
		}
		points[count].temp_c = t;
		points[count].duty_pct = d;
		count++;
	}
	if (count < 2) {
		fprintf(stderr, "[harness] fan curve: need at least 2 points -- ignoring\n");
		return;
	}

	/* Insertion sort ascending by temp_c -- count is tiny (<=8). */
	{
		int i, j;

		for (i = 1; i < count; i++) {
			struct fan_curve_point key = points[i];

			j = i - 1;
			while (j >= 0 && points[j].temp_c > key.temp_c) {
				points[j + 1] = points[j];
				j--;
			}
			points[j + 1] = key;
		}
	}

	memcpy(g_fan_curve, points, sizeof(points[0]) * (size_t)count);
	g_fan_curve_count = count;
	fan_curve_save();
	fprintf(stderr, "[harness] fan curve: updated (%d point(s)), saved to %s (via dashboard/API)\n",
		count, FAN_CURVE_CONF_PATH);
}

static void fan_set(enum fan_power power)
{
	char path[96];
	char pwm_dir[64];

	snprintf(pwm_dir, sizeof(pwm_dir), "%s/pwm%d", FAN_PWMCHIP, FAN_PWM_N);
	if (access(pwm_dir, F_OK) != 0) {
		char numbuf[8];

		snprintf(numbuf, sizeof(numbuf), "%d", FAN_PWM_N);
		snprintf(path, sizeof(path), "%s/export", FAN_PWMCHIP);
		write_sysfs(path, numbuf);
	}

	if (power == FAN_ON) {
		snprintf(path, sizeof(path), "%s/enable", pwm_dir);
		write_sysfs(path, "1");

		/* Start at full speed until the first chip-temp reading
		 * arrives -- "start safe" intent. g_fan_current_duty_pct is
		 * forced back to -1 first so fan_apply_duty_percent() doesn't
		 * treat this as a no-op if the fan was already at 100% duty
		 * before being switched off. */
		g_fan_current_duty_pct = -1;
		fan_apply_duty_percent(100);
	} else {
		snprintf(path, sizeof(path), "%s/enable", pwm_dir);
		write_sysfs(path, "0");
		g_fan_current_duty_pct = -1;
	}
	fprintf(stderr, "[harness] fan (pwm%d) set %s\n", FAN_PWM_N,
		power == FAN_ON ? "ON (temp-controlled)" : "OFF");
	g_fan_power = power;
}

static void harness_apply_idle(int idle)
{
	if (idle) {
		power_en_set(0);
		fan_set(FAN_OFF);
	} else {
		power_en_set(1);
		/* Start at full speed on resume, before the first PID tick
		 * has a sensor reading to react to. */
		fan_set(FAN_ON);
	}
}

/* Manual fan override state, set via `fan:<mode>,<duty>` on
 * HARNESS_CONTROL_FILE. When active, fan_reactive_check() ramps toward
 * g_fan_manual_duty_percent (0-100) -- see FAN_MANUAL_RAMP_STEP_PCT --
 * skipping both the curve baseline and the chip-limit escalation
 * entirely. Checked even while the chain is idle (g_fan_power ==
 * FAN_OFF): previously the FAN_OFF early-return in fan_reactive_check()
 * ran first and silently discarded manual duty whenever the chain was
 * paused, live-observed 2026-09-17 during an outlet-sensor diagnostic
 * test (commanded 100% manual while idle, telemetry stayed at 0%). */
static int g_fan_manual_override = 0;
static int g_fan_manual_duty_percent = 100;
/* Ramping state for the manual override -- see FAN_MANUAL_RAMP_STEP_PCT. */
static int g_fan_manual_ramp_duty_pct = -1;

/* Manual duty changes ramp at this many percentage points per
 * fan_reactive_check() tick (1s cadence) instead of snapping straight to
 * the commanded value -- explicit user request ("cool the chips
 * slowly") after a manual 0->100% command produced an instant jump.
 * 5%/tick reaches full swing in ~20s, matching the gradual feel of the
 * chip-temp PID's own output rather than the escalation path's
 * deliberately-instant 100%. */
#define FAN_MANUAL_RAMP_STEP_PCT 5

/* Chip-temp PID controller, replacing a binary jump-to-100% escalation.
 * Original behavior: the instant temp_max crossed the active mode's
 * real cali.ini limit (nano3s_temp_limit_for_pll0()), duty went
 * straight to 100% -- live-observed 2026-09-17 that this overshoots,
 * cooling chips well below where they need to be (down to ~70C) instead
 * of holding a comfortable steady state. An intermediate fixed-step
 * bang-bang controller replaced that, but still needed several rounds
 * of live tuning (a stale-read gate, then a slower step cadence) to
 * stop oscillating/overcooling -- replaced here with a real PID at
 * explicit user request, targeting 90C to match both that request and
 * the user's own statement that these chips perform better hot (see
 * project_chips_perform_better_at_90c memory). Settable via
 * `fan_chip_temp_target:<C>` on HARNESS_CONTROL_FILE; persisted so it
 * survives reboots, same pattern as the fan curve. */
#define FAN_THERMOSTAT_CONF_PATH "/data/userconfig/fan_thermostat.conf"
/* PID gains -- deliberately conservative starting point (small Kp,
 * tiny Ki, moderate Kd) given this session's live history of the
 * bang-bang predecessor overcooling/oscillating; biases toward a slow,
 * smooth approach rather than an aggressive one. Duty is a 0-100 scale
 * and error is in degrees C, so Kp=3 means roughly +3% duty per 1C over
 * target. */
#define FAN_PID_KP 3.0
#define FAN_PID_KI 0.05
#define FAN_PID_KD 6.0
/* Anti-windup: caps how much the integral term alone can contribute to
 * duty, so a long stretch above target can't leave the integral so
 * large that the controller overshoots badly once temp finally comes
 * back down. */
#define FAN_PID_INTEGRAL_CLAMP 40.0
/* PID math runs every this-many fan_reactive_check() ticks (still
 * called every POLL_INTERVAL_SEC=1s so manual override/logging/the
 * hard escalation stay responsive every second) rather than every
 * single tick. Same reasoning as the bang-bang controller's own
 * step-interval fix: NANO3S_LIVE_FILE's temp_max can read the exact
 * same stale value across many consecutive 1s ticks (mujina-minerd
 * writes it slower than the harness was polling), and integrating or
 * differentiating a duplicated reading multiple times would distort
 * both the I and D terms. */
#define FAN_PID_UPDATE_INTERVAL_TICKS 5
/* True emergency escalation ceiling, independent of
 * nano3s_temp_limit_for_pll0()'s per-mode nominal limit (80/85/90C from
 * hashrate_cali.ini). Originally tied to that per-mode limit directly,
 * which -- with the PID target now also at 90C, matching HIGH mode's
 * own nominal limit -- left literally zero room for the PID to hold
 * steady before the separate hard escalation also fired, live-verified
 * 2026-09-17 to cause exactly the abrupt duty jump to 100% the user
 * flagged twice ("fans too fast", "ramp up graduatlly"). Raised to a
 * flat 100C at explicit user request ("keep emergency to 100C now"),
 * above both the vendor's own 90C HIGH-mode limit and
 * mujina-minerd's TEMP_TARGET_EMERGENCY_C (92C) -- a deliberate,
 * explicit override of those, not an oversight. */
#define FAN_EMERGENCY_TEMP_C 100.0
static double g_fan_chip_thermostat_target_c = 90.0;
static double g_fan_pid_integral = 0.0;
static double g_fan_pid_prev_error = 0.0;
static int g_fan_pid_have_prev = 0;
static int g_fan_pid_update_countdown = 0;
/* Running controller output -- persists tick to tick (not recomputed
 * from scratch each time) so duty changes gradually instead of
 * snapping. */
static int g_fan_pid_duty_pct = 50;

static void fan_thermostat_save(void)
{
	FILE *f = fopen(FAN_THERMOSTAT_CONF_PATH, "w");

	if (!f) {
		fprintf(stderr, "[harness] fan thermostat: failed to open %s for write\n",
			FAN_THERMOSTAT_CONF_PATH);
		return;
	}
	fprintf(f, "%.1f\n", g_fan_chip_thermostat_target_c);
	fclose(f);
}

static void fan_thermostat_load(void)
{
	FILE *f = fopen(FAN_THERMOSTAT_CONF_PATH, "r");
	double t;

	if (!f) {
		fan_thermostat_save();
		fprintf(stderr, "[harness] fan thermostat: no saved target at %s -- using default %.1fC\n",
			FAN_THERMOSTAT_CONF_PATH, g_fan_chip_thermostat_target_c);
		return;
	}
	if (fscanf(f, "%lf", &t) == 1) {
		g_fan_chip_thermostat_target_c = t;
		fprintf(stderr, "[harness] fan thermostat: loaded target %.1fC from %s\n",
			t, FAN_THERMOSTAT_CONF_PATH);
	}
	fclose(f);
}

/* Called once per control_poll_loop() iteration (1s cadence). Only acts
 * while the chain is powered (g_fan_power == FAN_ON).
 *
 * Two independent signals, not one blended controller:
 *  - Baseline: outlet air temp (read_outlet_temp_c()) looked up against
 *    the user-editable curve (g_fan_curve/fan_curve_lookup()) -- this is
 *    what normally sets duty, and can legitimately be 0% when outlet is
 *    cool.
 *  - Chip PID: real chip temp_max (via NANO3S_LIVE_FILE) against
 *    g_fan_chip_thermostat_target_c -- a real PID controller (see that
 *    variable's doc comment) that holds temp_max near the target,
 *    taking over from the curve baseline whenever it would call for
 *    more cooling. This is what makes an outlet-only baseline safe:
 *    outlet temp alone can lag chip heat badly (a real soak test saw it
 *    sit flat at 50C while chip temp_max passed 80C -- see
 *    OUTLET_TEMP_HWMON's doc comment), but this reacts to the chips
 *    directly and doesn't depend on outlet tracking them. A true
 *    emergency (temp_max reaching FAN_EMERGENCY_TEMP_C, independent of
 *    the active mode's own nominal cali.ini limit -- see that
 *    constant's doc comment) forces real 100%, bypassing the
 *    controller entirely.
 *
 * If chip temp is unreadable/stale, or outlet temp is unreadable, fails
 * open to max cooling rather than guessing -- cooling too much is always
 * safe, cooling too little while a signal is unknown isn't. Logs only
 * when the applied duty percent actually changes, to limit log volume. */
static void fan_reactive_check(void)
{
	static int last_logged_duty = -1;
	double temp_max = 0.0, pll0 = 0.0, limit_c, outlet_c;
	int duty_percent, baseline_duty;
	int escalated;

	if (g_fan_manual_override) {
		/* Ramp toward the commanded duty rather than snapping to it --
		 * checked even while the chain is idle (g_fan_power ==
		 * FAN_OFF), see this override's own doc comment. */
		if (g_fan_manual_ramp_duty_pct < 0)
			g_fan_manual_ramp_duty_pct = g_fan_current_duty_pct >= 0 ? g_fan_current_duty_pct : 0;
		if (g_fan_manual_ramp_duty_pct < g_fan_manual_duty_percent) {
			g_fan_manual_ramp_duty_pct += FAN_MANUAL_RAMP_STEP_PCT;
			if (g_fan_manual_ramp_duty_pct > g_fan_manual_duty_percent)
				g_fan_manual_ramp_duty_pct = g_fan_manual_duty_percent;
		} else if (g_fan_manual_ramp_duty_pct > g_fan_manual_duty_percent) {
			g_fan_manual_ramp_duty_pct -= FAN_MANUAL_RAMP_STEP_PCT;
			if (g_fan_manual_ramp_duty_pct < g_fan_manual_duty_percent)
				g_fan_manual_ramp_duty_pct = g_fan_manual_duty_percent;
		}

		duty_percent = g_fan_manual_ramp_duty_pct;
		fan_apply_duty_percent(duty_percent);
		if (last_logged_duty != duty_percent) {
			fprintf(stderr, "[harness] fan MANUAL: duty=%d%% -> target %d%% (curve/escalation suspended)\n",
				duty_percent, g_fan_manual_duty_percent);
			last_logged_duty = duty_percent;
		}
		return;
	}

	if (g_fan_power == FAN_OFF) {
		g_fan_current_duty_pct = 0;
		return;
	}

	/* temp_max < 20.0 is treated the same as "unreadable": rtos_core's own
	 * status struct legitimately reports exactly 0.0 on any cycle where
	 * its sample count is momentarily zero (main.c: temp_n > 0 ? temp_max
	 * : 0.0) -- a real value seen live, not a hypothetical. No real
	 * indoor ambient reads that low, so trusting a sub-20C reading here
	 * would mean occasionally believing the chain is ice-cold and
	 * dropping straight to QUIET while it's actually hot. */
	if (!read_chip_temp_and_pll0(&temp_max, &pll0) || temp_max < 20.0) {
		fan_apply_duty_percent(100);
		if (last_logged_duty != 100) {
			fprintf(stderr, "[harness] fan: %s unreadable or implausible -- failing open to MAX cooling\n",
				NANO3S_LIVE_FILE);
			last_logged_duty = 100;
		}
		return;
	}

	limit_c = nano3s_temp_limit_for_pll0(pll0);
	outlet_c = read_outlet_temp_c();
	escalated = temp_max >= FAN_EMERGENCY_TEMP_C;

	if (escalated) {
		/* True emergency -- chips at/over the mode's real safety
		 * limit despite the PID. Always full blast (bypasses the PID
		 * entirely -- a real emergency doesn't wait for the next
		 * update tick), and resets the PID's running state (output,
		 * integral, derivative history) so recovery starts clean
		 * afterward instead of carrying over a distorted integral
		 * from the emergency itself. */
		duty_percent = 100;
		g_fan_pid_duty_pct = 100;
		g_fan_pid_integral = 0.0;
		g_fan_pid_have_prev = 0;
		g_fan_pid_update_countdown = 0;
	} else {
		baseline_duty = (outlet_c <= -273.0)
			? 100 /* outlet sensor unreadable -- same fail-open reasoning as the chip-temp case above */
			: (int)(fan_curve_lookup(outlet_c) + 0.5);

		if (g_fan_pid_update_countdown > 0) {
			g_fan_pid_update_countdown--;
		} else {
			/* error > 0 means too hot -- more duty needed. */
			double error = temp_max - g_fan_chip_thermostat_target_c;
			double derivative = g_fan_pid_have_prev ? (error - g_fan_pid_prev_error) : 0.0;
			double output;

			g_fan_pid_integral += error;
			if (g_fan_pid_integral > FAN_PID_INTEGRAL_CLAMP)
				g_fan_pid_integral = FAN_PID_INTEGRAL_CLAMP;
			else if (g_fan_pid_integral < -FAN_PID_INTEGRAL_CLAMP)
				g_fan_pid_integral = -FAN_PID_INTEGRAL_CLAMP;

			output = FAN_PID_KP * error + FAN_PID_KI * g_fan_pid_integral + FAN_PID_KD * derivative;
			g_fan_pid_duty_pct = baseline_duty + (int)(output + 0.5);
			if (g_fan_pid_duty_pct > 100)
				g_fan_pid_duty_pct = 100;
			if (g_fan_pid_duty_pct < baseline_duty)
				g_fan_pid_duty_pct = baseline_duty;

			g_fan_pid_prev_error = error;
			g_fan_pid_have_prev = 1;
			g_fan_pid_update_countdown = FAN_PID_UPDATE_INTERVAL_TICKS;
		}
		duty_percent = g_fan_pid_duty_pct > baseline_duty ? g_fan_pid_duty_pct : baseline_duty;
	}

	fan_apply_duty_percent(duty_percent);

	if (last_logged_duty != g_fan_current_duty_pct) {
		fprintf(stderr,
			"[harness] fan: chip_temp_max=%.1fC target=%.1fC limit=%.1fC outlet_c=%.1fC (pll0=%.0f) -> duty=%d%%%s\n",
			temp_max, g_fan_chip_thermostat_target_c, limit_c, outlet_c, pll0, g_fan_current_duty_pct,
			escalated ? " [EMERGENCY: chips at/over 100C]" : "");
		last_logged_duty = g_fan_current_duty_pct;
	}
}

/* Reads the fan tachometer once per FAN_TACH_WINDOW_SEC and writes the
 * latest RPM + current duty to FAN_STATUS_FILE for mujina-minerd to pick
 * up. Runs on its own thread, decoupled from control_poll_loop(), so a
 * stuck/missing tach device can never delay pause/resume or PID control.
 * If the device can't be opened or armed, logs once and exits -- no RPM
 * ever gets reported rather than looping on a broken fd. */
static void *fan_tach_loop(void *arg)
{
	int fd;
	int window = FAN_TACH_WINDOW_SEC;

	(void)arg;

	fd = open(FAN_TACH_DEV, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "[harness] fan tach: failed to open %s -- RPM reporting disabled\n",
			FAN_TACH_DEV);
		return NULL;
	}
	if (ioctl(fd, TMIOC_SET_TIMEOUT, &window) < 0) {
		fprintf(stderr, "[harness] fan tach: TMIOC_SET_TIMEOUT failed -- RPM reporting disabled\n");
		close(fd);
		return NULL;
	}

	for (;;) {
		unsigned int pulses = 0;
		int rpm = -1;
		FILE *f;

		sleep(window);
		if (read(fd, &pulses, sizeof(pulses)) == (ssize_t)sizeof(pulses))
			rpm = (int)(pulses * 60 / 2);

		f = fopen(FAN_STATUS_FILE, "w");
		if (f) {
			fprintf(f, "rpm=%d\nduty=%d\n", rpm, g_fan_current_duty_pct);
			fclose(f);
		}
	}
	return NULL;
}

/* ==========================================================================
 * LCD rendering. Opt-in only, see the top-of-file comment.
 * ========================================================================== */

#define SCR_W 240
#define SCR_H 240
#define FB_DEV "/dev/fb0"
#define PAGE_FILE "/mntapp/release/linux/app/fb_page"
#define RENDER_INTERVAL_SEC 2

/* Color palette used by the rendered pages, as RGB888 hex strings. */
#define MUJINA_BG      "0e0f12"
#define MUJINA_TEXT_1  "e7e8ec"
#define MUJINA_TEXT_2  "b5b7bf"
#define MUJINA_TEXT_3  "999ba4"
#define MUJINA_DIVIDER "272931"
#define MUJINA_ACCENT  "d05555"

/* 8x8 bitmap font, printable ASCII 0x20..0x7e. Read bit0=leftmost pixel
 * (see screen_draw_char()). */
static const uint8_t FONT[96][8] = {
	/* 0x20 space */
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
	/* 0x21 ! */
	{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
	/* 0x22 " */
	{0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
	/* 0x23 # */
	{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
	/* 0x24 $ */
	{0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},
	/* 0x25 % */
	{0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
	/* 0x26 & */
	{0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},
	/* 0x27 ' */
	{0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
	/* 0x28 ( */
	{0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
	/* 0x29 ) */
	{0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
	/* 0x2a * */
	{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
	/* 0x2b + */
	{0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
	/* 0x2c , */
	{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},
	/* 0x2d - */
	{0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
	/* 0x2e . */
	{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},
	/* 0x2f / */
	{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
	/* 0x30 0 */
	{0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},
	/* 0x31 1 */
	{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
	/* 0x32 2 */
	{0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},
	/* 0x33 3 */
	{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
	/* 0x34 4 */
	{0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
	/* 0x35 5 */
	{0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
	/* 0x36 6 */
	{0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},
	/* 0x37 7 */
	{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
	/* 0x38 8 */
	{0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},
	/* 0x39 9 */
	{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
	/* 0x3a : */
	{0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},
	/* 0x3b ; */
	{0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
	/* 0x3c < */
	{0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},
	/* 0x3d = */
	{0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
	/* 0x3e > */
	{0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
	/* 0x3f ? */
	{0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
	/* 0x40 @ */
	{0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
	/* 0x41 A */
	{0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
	/* 0x42 B */
	{0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},
	/* 0x43 C */
	{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
	/* 0x44 D */
	{0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},
	/* 0x45 E */
	{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
	/* 0x46 F */
	{0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
	/* 0x47 G */
	{0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
	/* 0x48 H */
	{0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},
	/* 0x49 I */
	{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
	/* 0x4a J */
	{0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},
	/* 0x4b K */
	{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
	/* 0x4c L */
	{0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
	/* 0x4d M */
	{0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
	/* 0x4e N */
	{0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},
	/* 0x4f O */
	{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
	/* 0x50 P */
	{0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},
	/* 0x51 Q */
	{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
	/* 0x52 R */
	{0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
	/* 0x53 S */
	{0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
	/* 0x54 T */
	{0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
	/* 0x55 U */
	{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
	/* 0x56 V */
	{0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},
	/* 0x57 W */
	{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
	/* 0x58 X */
	{0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},
	/* 0x59 Y */
	{0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
	/* 0x5a Z */
	{0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
	/* 0x5b [ */
	{0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
	/* 0x5c backslash */
	{0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},
	/* 0x5d ] */
	{0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
	/* 0x5e ^ */
	{0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
	/* 0x5f _ */
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
	/* 0x60 ` */
	{0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
	/* 0x61 a */
	{0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
	/* 0x62 b */
	{0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},
	/* 0x63 c */
	{0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
	/* 0x64 d */
	{0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00},
	/* 0x65 e */
	{0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00},
	/* 0x66 f */
	{0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00},
	/* 0x67 g */
	{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
	/* 0x68 h */
	{0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},
	/* 0x69 i */
	{0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
	/* 0x6a j */
	{0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
	/* 0x6b k */
	{0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
	/* 0x6c l */
	{0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
	/* 0x6d m */
	{0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
	/* 0x6e n */
	{0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},
	/* 0x6f o */
	{0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
	/* 0x70 p */
	{0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
	/* 0x71 q */
	{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
	/* 0x72 r */
	{0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},
	/* 0x73 s */
	{0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
	/* 0x74 t */
	{0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},
	/* 0x75 u */
	{0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
	/* 0x76 v */
	{0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
	/* 0x77 w */
	{0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
	/* 0x78 x */
	{0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},
	/* 0x79 y */
	{0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
	/* 0x7a z */
	{0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},
	/* 0x7b { */
	{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
	/* 0x7c | */
	{0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
	/* 0x7d } */
	{0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
	/* 0x7e ~ */
	{0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
	/* 0x7f (DEL / fallback) */
	{0xFF,0x81,0xBD,0xA5,0xBD,0x81,0xFF,0x00},
};

/* Converts 8-bit-per-channel RGB to 16-bit RGB565. */
static uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* Parses a "RRGGBB" hex string into an RGB565 color. */
static uint16_t parse_color(const char *s)
{
	unsigned long v = strtoul(s, NULL, 16);

	return rgb888_to_565((uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)(v & 0xFF));
}

struct screen {
	uint16_t buf[SCR_W * SCR_H];
};

static void screen_blank(struct screen *scr)
{
	memset(scr->buf, 0, sizeof(scr->buf));
}

/* Writes the framebuffer to FB_DEV. */
static void screen_flush(const struct screen *scr)
{
	FILE *f = fopen(FB_DEV, "wb");

	if (!f) {
		fprintf(stderr, "[harness] render: open %s failed\n", FB_DEV);
		return;
	}
	fseek(f, 0, SEEK_SET);
	fwrite(scr->buf, 1, sizeof(scr->buf), f);
	fclose(f);
}

static void screen_px(struct screen *scr, int x, int y, uint16_t c)
{
	if (x >= 0 && x < SCR_W && y >= 0 && y < SCR_H)
		scr->buf[y * SCR_W + x] = c;
}

static void screen_clear(struct screen *scr, uint16_t c)
{
	int i;

	for (i = 0; i < SCR_W * SCR_H; i++)
		scr->buf[i] = c;
}

static void screen_rect(struct screen *scr, int x, int y, int w, int h, uint16_t c)
{
	int row, col;
	int y_end = (y + h < SCR_H) ? y + h : SCR_H;
	int x_end = (x + w < SCR_W) ? x + w : SCR_W;

	for (row = y; row < y_end; row++)
		for (col = x; col < x_end; col++)
			screen_px(scr, col, row, c);
}

static void screen_hline(struct screen *scr, int x, int y, int len, uint16_t c)
{
	screen_rect(scr, x, y, len, 1, c);
}

/* Unused by the current pages but kept as part of the primitive set. */
__attribute__((unused))
static void screen_vline(struct screen *scr, int x, int y, int len, uint16_t c)
{
	screen_rect(scr, x, y, 1, len, c);
}

/* Draws one glyph from FONT, scaled by `scale`. Bit0 of each row byte is
 * the leftmost pixel. */
static void screen_draw_char(struct screen *scr, int px, int py, char ch, int scale, uint16_t fg, uint16_t bg)
{
	int idx = (unsigned char)ch - 0x20;
	const uint8_t *glyph;
	int row, col, sy, sx;

	if (idx < 0)
		idx = 0;
	if (idx >= 96)
		idx = 95;
	glyph = FONT[idx];

	for (row = 0; row < 8; row++) {
		uint8_t byte = glyph[row];

		for (col = 0; col < 8; col++) {
			int set = (byte >> col) & 1;
			uint16_t color = set ? fg : bg;

			for (sy = 0; sy < scale; sy++)
				for (sx = 0; sx < scale; sx++)
					screen_px(scr, px + col * scale + sx, py + row * scale + sy, color);
		}
	}
}

static void screen_draw_text(struct screen *scr, int x, int y, int scale, uint16_t fg, uint16_t bg, const char *text)
{
	int cw = 8 * scale;

	for (; *text; text++) {
		if (x + cw > SCR_W)
			break;
		screen_draw_char(scr, x, y, *text, scale, fg, bg);
		x += cw;
	}
}

/* Draws text horizontally centered on the screen. */
static void screen_draw_text_centered(struct screen *scr, int y, int scale, uint16_t fg, uint16_t bg, const char *text)
{
	int width = (int)strlen(text) * 8 * scale;
	int x = (SCR_W - width) / 2;

	if (x < 0)
		x = 0;
	screen_draw_text(scr, x, y, scale, fg, bg, text);
}

/* Fixed-capacity key/value table for parsed status-file lines. */
#define KV_MAX_ENTRIES 32
#define KV_KEY_LEN 32
#define KV_VAL_LEN 64

struct kv_store {
	char key[KV_MAX_ENTRIES][KV_KEY_LEN];
	char val[KV_MAX_ENTRIES][KV_VAL_LEN];
	int count;
};

/* Parses NANO3S_LIVE_FILE (key=value lines) into kv. Returns 0 on
 * success, -1 if the file doesn't exist. */
static int read_nano3s_status(struct kv_store *kv)
{
	FILE *f = fopen(NANO3S_LIVE_FILE, "r");
	char line[128];

	if (!f)
		return -1;

	kv->count = 0;
	while (kv->count < KV_MAX_ENTRIES && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		char *nl;
		size_t klen, vlen;

		if (!eq)
			continue;
		nl = strpbrk(line, "\r\n");
		if (nl)
			*nl = '\0';

		klen = (size_t)(eq - line);
		if (klen >= KV_KEY_LEN)
			klen = KV_KEY_LEN - 1;
		memcpy(kv->key[kv->count], line, klen);
		kv->key[kv->count][klen] = '\0';

		vlen = strlen(eq + 1);
		if (vlen >= KV_VAL_LEN)
			vlen = KV_VAL_LEN - 1;
		memcpy(kv->val[kv->count], eq + 1, vlen);
		kv->val[kv->count][vlen] = '\0';

		kv->count++;
	}
	fclose(f);
	return 0;
}

static const char *kv_str(const struct kv_store *kv, const char *key)
{
	int i;

	for (i = 0; i < kv->count; i++) {
		if (strcmp(kv->key[i], key) == 0)
			return kv->val[i];
	}
	return NULL;
}

/* Returns 1 and fills *out if key exists and parses as a double. */
static int kv_have_f64(const struct kv_store *kv, const char *key, double *out)
{
	const char *s = kv_str(kv, key);
	char *end;

	if (!s)
		return 0;
	*out = strtod(s, &end);
	return end != s;
}

/* Returns 1 and fills *out if key exists and parses as an unsigned int. */
static int kv_have_u32(const struct kv_store *kv, const char *key, uint32_t *out)
{
	const char *s = kv_str(kv, key);
	char *end;
	unsigned long v;

	if (!s)
		return 0;
	v = strtoul(s, &end, 10);
	if (end == s)
		return 0;
	*out = (uint32_t)v;
	return 1;
}

static int kv_bool(const struct kv_store *kv, const char *key)
{
	const char *s = kv_str(kv, key);

	return s != NULL && strcmp(s, "1") == 0;
}

static int kv_has_key(const struct kv_store *kv, const char *key)
{
	return kv_str(kv, key) != NULL;
}

/* Maps a temperature to a status color. */
static uint16_t temp_color_c(double temp)
{
	if (temp < 38.0)
		return parse_color("39d353");
	if (temp < 44.0)
		return parse_color("ffff00");
	if (temp < 50.0)
		return parse_color("ff9f1c");
	return parse_color("ff5555");
}

/* Draws a placeholder screen showing a status message. */
static void render_nano3s_waiting(struct screen *scr, const char *message)
{
	uint16_t bg = parse_color(MUJINA_BG);

	screen_blank(scr);
	screen_clear(scr, bg);
	screen_draw_text_centered(scr, 96, 1, parse_color(MUJINA_ACCENT), bg, "MUJINA");
	screen_draw_text_centered(scr, 120, 2, parse_color(MUJINA_TEXT_3), bg, message);
	screen_flush(scr);
}

/* Draws the main status page: mining state, hashrate, chain temperature,
 * pool/share info, and power draw. */
static void render_nano3s_live(struct screen *scr, const struct kv_store *kv)
{
	uint16_t bg = parse_color(MUJINA_BG);
	uint16_t muted = parse_color(MUJINA_TEXT_3);
	uint16_t dim = parse_color(MUJINA_TEXT_2);
	uint16_t white = parse_color(MUJINA_TEXT_1);
	uint16_t accent = parse_color(MUJINA_ACCENT);
	int pool_connected, ipc_connected, have_hashrate, paused;
	const char *status_text;
	uint16_t status_color;
	uint32_t ghs;
	double avg, max_t, diff, w;
	char buf[64];

	screen_blank(scr);
	screen_clear(scr, bg);

	pool_connected = kv_bool(kv, "pool_connected");
	ipc_connected = kv_bool(kv, "ipc_connected");
	have_hashrate = kv_has_key(kv, "hashrate_ghs");
	paused = kv_bool(kv, "paused");

	if (paused) {
		status_text = "PAUSED";
		status_color = parse_color("ffff00");
	} else if (pool_connected && ipc_connected && have_hashrate) {
		status_text = "MINING";
		status_color = parse_color("39d353");
	} else if (pool_connected || ipc_connected) {
		status_text = "CONNECTING";
		status_color = parse_color("ffff00");
	} else {
		status_text = "OFFLINE";
		status_color = parse_color("ff5555");
	}
	screen_draw_text_centered(scr, 24, 1, status_color, bg, status_text);

	/* Hashrate block */
	screen_draw_text_centered(scr, 44, 1, dim, bg, "HASHRATE");
	if (paused) {
		screen_draw_text_centered(scr, 64, 2, muted, bg, "IDLE");
	} else if (kv_have_u32(kv, "hashrate_ghs", &ghs) && ghs > 0) {
		snprintf(buf, sizeof(buf), "%.2f TH/S", ghs / 1000.0);
		screen_draw_text_centered(scr, 64, 2, white, bg, buf);
	} else {
		screen_draw_text_centered(scr, 64, 2, muted, bg, "WARMING UP");
	}

	screen_hline(scr, 52, 96, 136, parse_color(MUJINA_DIVIDER));

	/* Chain temperature block */
	screen_draw_text_centered(scr, 108, 1, dim, bg, "CHAIN TEMP");
	if (kv_have_f64(kv, "temp_avg", &avg) && kv_have_f64(kv, "temp_max", &max_t)) {
		snprintf(buf, sizeof(buf), "%.1fC AVG", avg);
		screen_draw_text_centered(scr, 128, 2, temp_color_c(avg), bg, buf);
		snprintf(buf, sizeof(buf), "%.1fC MAX", max_t);
		screen_draw_text_centered(scr, 150, 1, muted, bg, buf);
	} else {
		screen_draw_text_centered(scr, 128, 2, muted, bg, "--");
	}

	screen_hline(scr, 52, 160, 136, parse_color(MUJINA_DIVIDER));

	/* Pool / job block */
	screen_draw_text_centered(scr, 172, 1, dim, bg, "POOL");
	if (kv_have_f64(kv, "difficulty", &diff)) {
		if (diff >= 1000.0)
			snprintf(buf, sizeof(buf), "DIFF %.0fK", diff / 1000.0);
		else
			snprintf(buf, sizeof(buf), "DIFF %.0f", diff);
	} else {
		snprintf(buf, sizeof(buf), "DIFF --");
	}
	{
		const char *shares = kv_str(kv, "shares_found");
		char line[96];

		snprintf(line, sizeof(line), "%s  %s SHARES", buf, shares ? shares : "0");
		screen_draw_text_centered(scr, 190, 1, accent, bg, line);
	}

	/* Estimated power draw. */
	if (kv_have_f64(kv, "power_w", &w)) {
		snprintf(buf, sizeof(buf), "~%.1f W", w);
		screen_draw_text_centered(scr, 210, 1, muted, bg, buf);
	}

	screen_flush(scr);
}

/* Draws the diagnostics page: chip count, PLL frequencies, core voltage,
 * and CRC error count. */
static void render_nano3s_diag(struct screen *scr, const struct kv_store *kv)
{
	uint16_t bg = parse_color(MUJINA_BG);
	uint16_t muted = parse_color(MUJINA_TEXT_3);
	uint16_t dim = parse_color(MUJINA_TEXT_2);
	uint16_t white = parse_color(MUJINA_TEXT_1);
	uint16_t accent = parse_color(MUJINA_ACCENT);
	uint32_t n, pa, pb, pc, pd, v;
	char buf[64];

	screen_blank(scr);
	screen_clear(scr, bg);

	screen_draw_text_centered(scr, 24, 1, accent, bg, "DIAGNOSTICS");

	screen_draw_text_centered(scr, 46, 1, dim, bg, "CHIPS ONLINE");
	if (kv_have_u32(kv, "asics_total", &n)) {
		snprintf(buf, sizeof(buf), "%u / 12", n);
		screen_draw_text_centered(scr, 66, 2, white, bg, buf);
	} else {
		screen_draw_text_centered(scr, 66, 2, muted, bg, "--");
	}

	screen_hline(scr, 52, 96, 136, parse_color(MUJINA_DIVIDER));

	screen_draw_text_centered(scr, 108, 1, dim, bg, "PLL LOW MODE (MHZ)");
	if (kv_have_u32(kv, "pll0", &pa) && kv_have_u32(kv, "pll1", &pb) &&
	    kv_have_u32(kv, "pll2", &pc) && kv_have_u32(kv, "pll3", &pd)) {
		snprintf(buf, sizeof(buf), "%u  %u  %u  %u", pa, pb, pc, pd);
		screen_draw_text_centered(scr, 128, 1, white, bg, buf);
	} else {
		screen_draw_text_centered(scr, 128, 1, muted, bg, "--");
	}

	screen_hline(scr, 52, 154, 136, parse_color(MUJINA_DIVIDER));

	screen_draw_text_centered(scr, 166, 1, dim, bg, "CORE VOLTAGE");
	if (kv_have_u32(kv, "voltage_mv", &v)) {
		snprintf(buf, sizeof(buf), "%u MV", v);
		screen_draw_text_centered(scr, 186, 2, white, bg, buf);
	} else {
		screen_draw_text_centered(scr, 186, 2, muted, bg, "--");
	}
	{
		const char *err = kv_str(kv, "err_crc");
		char line[64];

		snprintf(line, sizeof(line), "ERR_CRC %s", err ? err : "0");
		screen_draw_text_centered(scr, 210, 1, muted, bg, line);
	}

	screen_flush(scr);
}

/* Draws the network page, showing wlan0's IPv4 address parsed from
 * ifconfig output. */
static void render_ip(struct screen *scr)
{
	uint16_t bg = parse_color("001510");
	char ip[64] = "";
	FILE *p = popen("ifconfig wlan0 2>/dev/null", "r");

	screen_blank(scr);
	screen_clear(scr, bg);
	screen_draw_text_centered(scr, 48, 2, parse_color("39d353"), bg, "NETWORK");

	if (p) {
		char line[256];

		while (fgets(line, sizeof(line), p)) {
			char *start = strstr(line, "inet addr:");

			if (start) {
				char *end;
				size_t len;

				start += strlen("inet addr:");
				end = start;
				while (*end && !isspace((unsigned char)*end))
					end++;
				len = (size_t)(end - start);
				if (len >= sizeof(ip))
					len = sizeof(ip) - 1;
				memcpy(ip, start, len);
				ip[len] = '\0';
				break;
			}
		}
		pclose(p);
	}

	if (ip[0] != '\0') {
		screen_draw_text_centered(scr, 90, 1, parse_color("7fbcff"), bg, "IP ADDRESS");
		screen_draw_text_centered(scr, 112, 2, parse_color("ffffff"), bg, ip);
	} else {
		screen_draw_text_centered(scr, 104, 2, parse_color("ff5555"), bg, "NO WIFI");
	}
	screen_flush(scr);
}

/* Reads the current page name from PAGE_FILE; defaults to "nano3s" if
 * the file doesn't exist. */
static void read_page_file(char *out, size_t outlen)
{
	FILE *f = fopen(PAGE_FILE, "r");
	size_t len;

	snprintf(out, outlen, "nano3s");
	if (!f)
		return;
	if (fgets(out, (int)outlen, f)) {
		len = strlen(out);
		while (len > 0 && isspace((unsigned char)out[len - 1])) {
			out[len - 1] = '\0';
			len--;
		}
	} else {
		snprintf(out, outlen, "nano3s");
	}
	fclose(f);
}

/* Main render loop: reads the current page and status file each tick and
 * draws the corresponding page. Any page value other than "ip" or
 * "nano3s-diag" falls through to the live status page. */
static void render_loop(void)
{
	struct screen scr;

	fprintf(stderr, "[harness] LCD render loop starting, %ds interval, reading %s\n",
		RENDER_INTERVAL_SEC, NANO3S_LIVE_FILE);

	for (;;) {
		char page[32];
		struct kv_store kv;
		int have_kv;

		read_page_file(page, sizeof(page));
		have_kv = (read_nano3s_status(&kv) == 0);

		if (strcmp(page, "ip") == 0)
			render_ip(&scr);
		else if (have_kv && strcmp(page, "nano3s-diag") == 0)
			render_nano3s_diag(&scr, &kv);
		else if (have_kv)
			render_nano3s_live(&scr, &kv);
		else
			render_nano3s_waiting(&scr, "STARTING");

		sleep(RENDER_INTERVAL_SEC);
	}
}

/* ==========================================================================
 * Control loop + main().
 * ========================================================================== */

/* Splits a comma-separated string in place, writing a NUL at each comma.
 * Unlike strtok(), an empty field between two adjacent commas is
 * returned as an empty string rather than skipped. *cursor is advanced
 * past the consumed field; call repeatedly for each subsequent field. */
static char *csv_next_field(char **cursor)
{
	char *field = *cursor;
	char *comma = strchr(*cursor, ',');

	if (comma) {
		*comma = '\0';
		*cursor = comma + 1;
	} else {
		*cursor = *cursor + strlen(*cursor);
	}
	return field;
}

static void *control_poll_loop(void *arg)
{
	(void)arg;

	/* Load the persisted fan curve (or seed the default) before the first
	 * fan_reactive_check() call below can need it. */
	fan_curve_load();
	fan_thermostat_load();

	/* Engage power_en and the fan controller at process start. */
	fprintf(stderr, "[harness] auto-engaging power_en + fan control at startup\n");
	harness_apply_idle(0);

	for (;;) {
		FILE *f = fopen(HARNESS_CONTROL_FILE, "r");
		/* Sized for "fancurve:" + FAN_CURVE_MAX_POINTS points (each up
		 * to "100.0:100.0,", ~13 bytes), comfortably under 256. */
		char buf[256];

		if (f) {
			if (fgets(buf, sizeof(buf), f)) {
				if (strstr(buf, "pause")) {
					fprintf(stderr, "[harness] pause requested -- depowering chain\n");
					harness_apply_idle(1);
				} else if (strstr(buf, "resume")) {
					fprintf(stderr, "[harness] resume requested -- repowering chain\n");
					harness_apply_idle(0);
				} else if (strncmp(buf, "fan:", 4) == 0) {
					/* Exactly 2 comma-separated fields: mode, duty. An
					 * empty duty field means "don't change". */
					char *rest = buf + 4;
					char *nl = strchr(rest, '\n');
					char *mode, *duty_s;
					char *cursor = rest;

					if (nl)
						*nl = '\0';
					mode = csv_next_field(&cursor);
					duty_s = csv_next_field(&cursor);

					if (mode && mode[0] != '\0') {
						if (strcmp(mode, "manual") == 0) {
							if (duty_s && duty_s[0] != '\0') {
								g_fan_manual_override = 1;
								g_fan_manual_duty_percent = atoi(duty_s);
								fprintf(stderr, "[harness] fan: manual override ENABLED, duty=%d%% (via dashboard/API)\n",
									g_fan_manual_duty_percent);
							} else {
								fprintf(stderr, "[harness] fan: manual mode requested with no duty -- ignored\n");
							}
						} else if (strcmp(mode, "auto") == 0) {
							g_fan_manual_override = 0;
							/* Reseed the ramp from actual fan speed on the next
							 * manual activation instead of resuming from a
							 * possibly stale position left over from this one. */
							g_fan_manual_ramp_duty_pct = -1;
							fprintf(stderr, "[harness] fan: manual override DISABLED, back to curve+escalation (via dashboard/API)\n");
						}
					}
				} else if (strncmp(buf, "fancurve:", 9) == 0) {
					char *rest = buf + 9;
					char *nl = strchr(rest, '\n');

					if (nl)
						*nl = '\0';
					fan_curve_apply_command(rest);
				} else if (strncmp(buf, "fan_chip_temp_target:", 21) == 0) {
					double t;

					if (sscanf(buf + 21, "%lf", &t) == 1) {
						g_fan_chip_thermostat_target_c = t;
						fan_thermostat_save();
						fprintf(stderr, "[harness] fan thermostat: target set to %.1fC (via dashboard/API)\n", t);
					} else {
						fprintf(stderr, "[harness] malformed fan_chip_temp_target directive: %s", buf);
					}
				}
			}
			fclose(f);
			f = fopen(HARNESS_CONTROL_FILE, "w");
			if (f)
				fclose(f);
		}
		fan_reactive_check();
		sleep(POLL_INTERVAL_SEC);
	}
	return NULL;
}

/* Spawns fan_tach_loop() as a detached thread. Best-effort: a failure here
 * only means no RPM ever gets reported, logged inside the thread function
 * itself, never fatal to the rest of the harness. */
static void spawn_fan_tach_thread(void)
{
	pthread_t tach_tid;

	if (pthread_create(&tach_tid, NULL, fan_tach_loop, NULL) != 0) {
		fprintf(stderr, "[harness] pthread_create(fan tach) failed -- RPM reporting disabled\n");
		return;
	}
	pthread_detach(tach_tid);
}

int main(void)
{
	const char *render_enable = getenv("HARNESS_RENDER_ENABLE");
	int do_render = render_enable != NULL && strcmp(render_enable, "1") == 0;

	spawn_fan_tach_thread();

	if (!do_render) {
		/* Rendering disabled: run the control-poll loop on the main
		 * thread only, with no /dev/fb0 access. */
		fprintf(stderr, "[harness] power_en/fan idle-control harness starting, "
			"polling %s every %ds\n", HARNESS_CONTROL_FILE, POLL_INTERVAL_SEC);
		control_poll_loop(NULL);
		return 0;
	}

	fprintf(stderr, "[harness] power_en/fan idle-control harness starting, "
		"polling %s every %ds (LCD rendering ENABLED via HARNESS_RENDER_ENABLE=1)\n",
		HARNESS_CONTROL_FILE, POLL_INTERVAL_SEC);
	{
		pthread_t poll_tid;

		if (pthread_create(&poll_tid, NULL, control_poll_loop, NULL) != 0) {
			fprintf(stderr, "[harness] pthread_create(control poll) failed -- "
				"falling back to poll-only, no rendering\n");
			control_poll_loop(NULL);
			return 0;
		}
		pthread_detach(poll_tid);
	}

	render_loop();
	return 0;
}

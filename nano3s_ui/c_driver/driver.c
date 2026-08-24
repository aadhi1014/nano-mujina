#include "driver.h"
#include "lvgl.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define HOR_RES 240
#define VER_RES 240
#define FB_DEV "/dev/fb0"
#define FB_BYTES (HOR_RES * VER_RES * 2)

#define PAGE_FILE "/mntapp/release/linux/app/fb_page"
#define PIZZA_FILE "/mntapp/release/linux/app/pizza.rgb565"
/* Written by rtos_core/tools/mujina.c -- plain key=value lines. */
#define NANO3S_LIVE_FILE "/tmp/nano3s_live.txt"
/* Written by ble_setup.rs (2026-08-24) -- same key=value format. Since
 * the real Avalon Life app's own success/failure indicator turned out
 * unreliable (see ble_setup.rs's doc comments), this page is the
 * source of truth for WiFi-setup progress instead. */
#define BLE_WIFI_STATUS_FILE "/tmp/ble_wifi_status.txt"

#define REFRESH_INTERVAL_MS 2000
#define POLL_INTERVAL_MS 100

/* Hashrate gauge scale is in deci-TH/s (0.1 TH/s per unit) so the needle
 * can move smoothly instead of snapping between whole TH/s ticks. 7.0 TH/s
 * headroom comfortably covers this chip's real observed range (~3.4-3.6
 * TH/s at production settings, up to ~6.7-6.8 TH/s in the fastest tested
 * PLL sweep configs) without the needle sitting pinned near max. */
#define HASHRATE_GAUGE_MAX 70

/* Power ring: 140W headroom sits just above this project's own hard safety
 * ceiling (never let ina_power_w exceed 133W -- see feedback memory), so
 * the ring fill also doubles as an at-a-glance "how close to the limit"
 * indicator. Temp ring: 90C headroom covers the highest max-temp values
 * ever observed during this project's PLL/voltage sweeps (~85C). */
#define POWER_GAUGE_MAX 140
#define TEMP_GAUGE_MAX 90

/* All three rings share this width -- uniform thickness per user
 * preference (previously hashrate was thicker at 18px than the other
 * two at 13px). The three rings' r_mod values (-2, -18, -34, see
 * build_screens()) are evenly spaced 16 apart to match: each ring's
 * outer edge sits exactly RING_WIDTH+2 inside the next ring's outer
 * edge, giving the same small 2px gap between every adjacent pair
 * instead of the uneven gap an earlier revision had between the
 * hashrate ring and the other two. */
#define RING_WIDTH 14

/* ── Mujina brand tokens (256foundation/mujina-website .dark block) ──────── */
#define C_BG lv_color_make(0x0e, 0x0f, 0x12)
#define C_TEXT_1 lv_color_make(0xe7, 0xe8, 0xec)
#define C_TEXT_2 lv_color_make(0xb5, 0xb7, 0xbf)
#define C_TEXT_3 lv_color_make(0x99, 0x9b, 0xa4)
#define C_DIVIDER lv_color_make(0x27, 0x29, 0x31)
#define C_ACCENT lv_color_make(0xd0, 0x55, 0x55)
#define C_GREEN lv_color_make(0x39, 0xd3, 0x53)
#define C_YELLOW lv_color_make(0xff, 0xff, 0x00)
#define C_ORANGE lv_color_make(0xff, 0x9f, 0x1c)
#define C_RED lv_color_make(0xff, 0x55, 0x55)
#define C_IP_BG lv_color_make(0x00, 0x15, 0x10)
#define C_IP_BLUE lv_color_make(0x7f, 0xbc, 0xff)
/* Same 0x7fbcff as C_IP_BLUE above, as a literal hex string for LVGL's
 * "#RRGGBB text#" inline label recolor syntax (which needs the digits
 * literally in the format string, not an lv_color_t) -- keep in sync if
 * C_IP_BLUE ever changes. */
#define C_IP_BLUE_HEX "7fbcff"
#define C_WHITE lv_color_make(0xff, 0xff, 0xff)

/* ── Display flush ────────────────────────────────────────────────────── */

static lv_color_t draw_buf1[HOR_RES * 10];

static void fb_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    int fd = open(FB_DEV, O_WRONLY);
    if (fd < 0) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    int32_t w = area->x2 - area->x1 + 1;
    size_t row_bytes = (size_t)w * sizeof(lv_color_t);

    for (int32_t y = area->y1; y <= area->y2; y++) {
        off_t offset = ((off_t)y * HOR_RES + area->x1) * (off_t)sizeof(lv_color_t);
        if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
            break;
        }
        const uint8_t *src = (const uint8_t *)color_p + (size_t)(y - area->y1) * row_bytes;
        size_t written = 0;
        while (written < row_bytes) {
            ssize_t n = write(fd, src + written, row_bytes - written);
            if (n <= 0) {
                break;
            }
            written += (size_t)n;
        }
    }

    close(fd);
    lv_disp_flush_ready(disp_drv);
}

static uint32_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)((uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u);
}

/* ── key=value telemetry file (NANO3S_LIVE_FILE) ─────────────────────────
 * Parsed in place into a fixed static buffer -- no heap allocation. */

#define KV_MAX_ENTRIES 32
#define KV_BUF_SIZE 4096

typedef struct {
    const char *key;
    const char *val;
} kv_entry_t;

static char kv_buf[KV_BUF_SIZE];
static kv_entry_t kv_entries[KV_MAX_ENTRIES];
static int kv_count;

/* Reads `path` and parses "key=value" lines into kv_entries. Returns 0
 * if the file couldn't be read (callers fall back to a waiting/default
 * state). Shared by both NANO3S_LIVE_FILE and BLE_WIFI_STATUS_FILE --
 * only one is ever needed per refresh call, so reusing one static
 * buffer is fine. */
static int read_kv_file(const char *path) {
    kv_count = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    size_t n = fread(kv_buf, 1, sizeof(kv_buf) - 1, f);
    fclose(f);
    kv_buf[n] = '\0';

    char *line = kv_buf;
    while (line < kv_buf + n && kv_count < KV_MAX_ENTRIES) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            kv_entries[kv_count].key = line;
            kv_entries[kv_count].val = eq + 1;
            kv_count++;
        }
        if (!nl) {
            break;
        }
        line = nl + 1;
    }
    return 1;
}

static const char *kv_str(const char *key) {
    for (int i = 0; i < kv_count; i++) {
        if (strcmp(kv_entries[i].key, key) == 0) {
            return kv_entries[i].val;
        }
    }
    return NULL;
}

static int kv_f64(const char *key, double *out) {
    const char *s = kv_str(key);
    if (!s || *s == '\0') {
        return 0;
    }
    char *end;
    double v = strtod(s, &end);
    if (end == s) {
        return 0;
    }
    *out = v;
    return 1;
}

static int kv_u32(const char *key, uint32_t *out) {
    const char *s = kv_str(key);
    if (!s || *s == '\0') {
        return 0;
    }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) {
        return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

static int kv_bool(const char *key) {
    const char *s = kv_str(key);
    return s && strcmp(s, "1") == 0;
}

/* Same thermal thresholds as fb_draw.rs's temp_color_c(). */
static lv_color_t temp_color_c(double t) {
    if (t < 38.0) return C_GREEN;
    if (t < 44.0) return C_YELLOW;
    if (t < 50.0) return C_ORANGE;
    return C_RED;
}

/* ── wlan0 IP lookup (mirrors fb_draw.rs's wlan_ip()) ────────────────────
 * Same `ifconfig wlan0` + "inet addr:" scrape as the original. */
static int wlan_ip(char *out, size_t out_size) {
    FILE *p = popen("ifconfig wlan0 2>/dev/null", "r");
    if (!p) {
        return 0;
    }
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    buf[n] = '\0';

    const char *needle = "inet addr:";
    char *start = strstr(buf, needle);
    if (!start) {
        return 0;
    }
    start += strlen(needle);
    char *end = start;
    while (*end && !isspace((unsigned char)*end)) {
        end++;
    }
    size_t len = (size_t)(end - start);
    if (len == 0 || len >= out_size) {
        return 0;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

/* Defense-in-depth for the doom page hand-off: the beeper
 * (/dev/input/event1, same device fb_button.rs's click-feedback beep
 * uses) is a stateful hardware device -- it keeps sounding whatever tone
 * it was last told to play regardless of whether the writing process is
 * still alive. nano3s_doom has its own SIGTERM-triggered cleanup, but if
 * it ever dies uncleanly (hard kill, crash) the buzzer would otherwise
 * stay stuck making noise until *something* else writes a new tone to
 * the same device. Silence it unconditionally whenever we take control
 * back from the doom page, regardless of why nano3s_doom stopped. */
static void silence_buzzer(void) {
    int fd = open("/dev/input/event1", O_WRONLY);
    if (fd < 0) {
        return;
    }
    uint8_t event[24];
    memset(event, 0, sizeof(event));
    event[16] = 0x12; /* EV_SND */
    event[18] = 0x02; /* SND_TONE */
    /* value (bytes 20..24) already zeroed -- frequency 0 == silence */
    ssize_t ignore = write(fd, event, sizeof(event));
    (void)ignore;
    event[16] = 0x00; /* EV_SYN */
    event[18] = 0x00;
    ignore = write(fd, event, sizeof(event));
    (void)ignore;
    close(fd);
}

/* ── Widget helpers ───────────────────────────────────────────────────── */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t y,
                             const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    if (text) {
        lv_label_set_text(l, text);
    }
    return l;
}

static lv_obj_t *make_divider(lv_obj_t *parent, lv_coord_t y) {
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 136, 1);
    lv_obj_set_style_bg_color(d, C_DIVIDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, y);
    return d;
}

static void style_bg(lv_obj_t *scr, lv_color_t bg) {
    lv_obj_set_style_bg_color(scr, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
}

/* ── Screens ──────────────────────────────────────────────────────────── */

static lv_obj_t *scr_waiting, *waiting_label;

static lv_obj_t *scr_live, *live_meter, *live_status, *live_hashrate_big, *live_sub, *live_temp_small, *live_pool_small;
static lv_meter_scale_t *live_scale, *live_scale_power, *live_scale_temp;
static lv_meter_indicator_t *live_arc, *live_arc_power, *live_arc_temp;

static lv_obj_t *scr_diag, *diag_chips_meter, *diag_chips_label, *diag_pll, *diag_voltage, *diag_err;
static lv_meter_scale_t *diag_chips_scale;
static lv_meter_indicator_t *diag_chips_arc;

static lv_obj_t *scr_ip, *ip_caption, *ip_value, *ip_no_wifi;

static lv_obj_t *scr_wifi_setup, *wifi_setup_status, *wifi_setup_detail, *wifi_setup_meter;
static lv_meter_scale_t *wifi_setup_scale;
static lv_meter_indicator_t *wifi_setup_arc;

static void build_screens(void) {
    const lv_font_t *f14 = &lv_font_montserrat_14;
    const lv_font_t *f24 = &lv_font_montserrat_24;

    /* waiting / startup splash */
    scr_waiting = lv_obj_create(NULL);
    style_bg(scr_waiting, C_BG);
    waiting_label = make_label(scr_waiting, f24, C_TEXT_1, 112, NULL);

    /* live-nano3s telemetry -- a speedometer-style radial gauge as the
     * centerpiece (fitting the display's own round physical shape), with
     * the rest of the telemetry living in its empty center like a car's
     * trip computer, instead of a plain stacked text list. */
    scr_live = lv_obj_create(NULL);
    style_bg(scr_live, C_BG);

    live_meter = lv_meter_create(scr_live);
    lv_obj_remove_style_all(live_meter);
    lv_obj_set_size(live_meter, 220, 220);
    lv_obj_center(live_meter);

    /* Three concentric rings, like a fitness-tracker activity dial:
     * outer = hashrate (status-colored), middle = chain temp
     * (thermal-threshold colored), inner = power draw (fixed accent) --
     * temp and power swapped from their original outer-to-inner order per
     * user preference. All three now share RING_WIDTH (uniform thickness
     * per user preference; hashrate used to be thicker than the other
     * two). No tick marks on any ring (the outer ring's were removed per
     * user preference) -- all three are legible from fill position +
     * color alone, backed by the numeric center readout. */
    live_scale = lv_meter_add_scale(live_meter);
    lv_meter_set_scale_range(live_meter, live_scale, 0, HASHRATE_GAUGE_MAX, 270, 135);

    live_arc = lv_meter_add_arc(live_meter, live_scale, RING_WIDTH, C_GREEN, -2);
    lv_meter_set_indicator_start_value(live_meter, live_arc, 0);
    lv_meter_set_indicator_end_value(live_meter, live_arc, 0);

    live_scale_power = lv_meter_add_scale(live_meter);
    lv_meter_set_scale_range(live_meter, live_scale_power, 0, POWER_GAUGE_MAX, 270, 135);
    live_arc_power = lv_meter_add_arc(live_meter, live_scale_power, RING_WIDTH, C_IP_BLUE, -34);
    lv_meter_set_indicator_start_value(live_meter, live_arc_power, 0);
    lv_meter_set_indicator_end_value(live_meter, live_arc_power, 0);

    live_scale_temp = lv_meter_add_scale(live_meter);
    lv_meter_set_scale_range(live_meter, live_scale_temp, 0, TEMP_GAUGE_MAX, 270, 135);
    live_arc_temp = lv_meter_add_arc(live_meter, live_scale_temp, RING_WIDTH, C_GREEN, -18);
    lv_meter_set_indicator_start_value(live_meter, live_arc_temp, 0);
    lv_meter_set_indicator_end_value(live_meter, live_arc_temp, 0);

    live_status = lv_label_create(scr_live);
    lv_obj_set_style_text_font(live_status, f14, LV_PART_MAIN);
    lv_obj_set_style_text_color(live_status, C_GREEN, LV_PART_MAIN);
    lv_obj_align(live_status, LV_ALIGN_CENTER, 0, -34);

    live_hashrate_big = lv_label_create(scr_live);
    lv_obj_set_style_text_font(live_hashrate_big, f24, LV_PART_MAIN);
    lv_obj_set_style_text_color(live_hashrate_big, C_TEXT_1, LV_PART_MAIN);
    lv_obj_align(live_hashrate_big, LV_ALIGN_CENTER, 0, -8);

    live_sub = lv_label_create(scr_live);
    lv_obj_set_style_text_font(live_sub, f14, LV_PART_MAIN);
    lv_obj_set_style_text_color(live_sub, C_TEXT_2, LV_PART_MAIN);
    lv_label_set_text(live_sub, "TH/S");
    lv_obj_align(live_sub, LV_ALIGN_CENTER, 0, 16);

    /* Compact "temp * power" readout -- the rings carry the at-a-glance
     * signal, this is just the precise numbers. */
    live_temp_small = lv_label_create(scr_live);
    lv_obj_set_style_text_font(live_temp_small, f14, LV_PART_MAIN);
    lv_obj_set_style_text_color(live_temp_small, C_TEXT_3, LV_PART_MAIN);
    lv_obj_align(live_temp_small, LV_ALIGN_CENTER, 0, 36);
    /* The watt figure gets its own inline color (matching the power ring's
     * C_IP_BLUE) via LVGL's built-in "#RRGGBB text#" recolor syntax, so it
     * reads as "that's the power ring's number" at a glance -- the temp
     * figure alongside it keeps using the label's own style color, which
     * refresh_live() still sets to the thermal-threshold color each frame. */
    lv_label_set_recolor(live_temp_small, true);

    live_pool_small = lv_label_create(scr_live);
    lv_obj_set_style_text_font(live_pool_small, f14, LV_PART_MAIN);
    lv_obj_set_style_text_color(live_pool_small, C_TEXT_3, LV_PART_MAIN);
    lv_obj_align(live_pool_small, LV_ALIGN_CENTER, 0, 54);

    /* nano3s-diag */
    scr_diag = lv_obj_create(NULL);
    style_bg(scr_diag, C_BG);
    make_label(scr_diag, f14, C_ACCENT, 16, "DIAGNOSTICS");

    /* Chips-online gauge -- small radial fraction-of-12 dial instead of
     * plain "N / 12" text, echoing the main hashrate gauge's language. */
    diag_chips_meter = lv_meter_create(scr_diag);
    lv_obj_remove_style_all(diag_chips_meter);
    lv_obj_set_size(diag_chips_meter, 92, 92);
    lv_obj_align(diag_chips_meter, LV_ALIGN_TOP_MID, 0, 36);

    diag_chips_scale = lv_meter_add_scale(diag_chips_meter);
    /* Same 270/135 sweep as the main hashrate gauge (bottom gap) for a
     * consistent visual language across pages. */
    lv_meter_set_scale_range(diag_chips_meter, diag_chips_scale, 0, 12, 270, 135);
    lv_meter_set_scale_ticks(diag_chips_meter, diag_chips_scale, 13, 1, 4, C_DIVIDER);

    diag_chips_arc = lv_meter_add_arc(diag_chips_meter, diag_chips_scale, 6, C_GREEN, -6);
    lv_meter_set_indicator_start_value(diag_chips_meter, diag_chips_arc, 0);
    lv_meter_set_indicator_end_value(diag_chips_meter, diag_chips_arc, 0);

    diag_chips_label = lv_label_create(scr_diag);
    lv_obj_set_style_text_font(diag_chips_label, f14, LV_PART_MAIN);
    lv_obj_set_style_text_color(diag_chips_label, C_TEXT_1, LV_PART_MAIN);
    lv_obj_align(diag_chips_label, LV_ALIGN_TOP_MID, 0, 78);

    make_divider(scr_diag, 134);
    make_label(scr_diag, f14, C_TEXT_2, 142, "PLL LOW MODE (MHZ)");
    diag_pll = make_label(scr_diag, f14, C_TEXT_1, 160, NULL);
    make_divider(scr_diag, 182);
    make_label(scr_diag, f14, C_TEXT_2, 190, "CORE VOLTAGE");
    diag_voltage = make_label(scr_diag, f14, C_TEXT_1, 206, NULL);
    diag_err = make_label(scr_diag, f14, C_TEXT_3, 224, NULL);

    /* ip / network */
    scr_ip = lv_obj_create(NULL);
    style_bg(scr_ip, C_IP_BG);
    make_label(scr_ip, f24, C_GREEN, 48, "NETWORK");
    ip_caption = make_label(scr_ip, f14, C_IP_BLUE, 90, "IP ADDRESS");
    ip_value = make_label(scr_ip, f24, C_WHITE, 112, NULL);
    ip_no_wifi = make_label(scr_ip, f24, C_RED, 104, "NO WIFI");

    /* wifi-setup -- BLE provisioning progress. Source of truth for
     * setup outcome, since the vendor phone app's own success/failure
     * indicator was found unreliable (see ble_setup.rs). A single
     * progress ring (same visual language as the main hashrate gauge)
     * instead of plain stacked text -- fill fraction + color together
     * read as "how far along" at a glance, refresh_wifi_setup() picks
     * both per state. */
    scr_wifi_setup = lv_obj_create(NULL);
    style_bg(scr_wifi_setup, C_BG);

    wifi_setup_meter = lv_meter_create(scr_wifi_setup);
    lv_obj_remove_style_all(wifi_setup_meter);
    lv_obj_set_size(wifi_setup_meter, 150, 150);
    lv_obj_align(wifi_setup_meter, LV_ALIGN_TOP_MID, 0, 14);

    wifi_setup_scale = lv_meter_add_scale(wifi_setup_meter);
    lv_meter_set_scale_range(wifi_setup_meter, wifi_setup_scale, 0, 100, 360, 0);
    wifi_setup_arc = lv_meter_add_arc(wifi_setup_meter, wifi_setup_scale, RING_WIDTH, C_YELLOW, 0);
    lv_meter_set_indicator_start_value(wifi_setup_meter, wifi_setup_arc, 0);
    lv_meter_set_indicator_end_value(wifi_setup_meter, wifi_setup_arc, 0);

    /* Short state word inside the ring (e.g. "BLE", "LINKED", "OK") --
     * the full sentence lives below, this is just the at-a-glance core. */
    wifi_setup_status = lv_label_create(scr_wifi_setup);
    lv_obj_set_style_text_font(wifi_setup_status, f24, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifi_setup_status, C_TEXT_1, LV_PART_MAIN);
    lv_obj_align(wifi_setup_status, LV_ALIGN_TOP_MID, 0, 78);

    /* Full status sentence + detail (device name / SSID / IP), below
     * the ring. make_label() aligns based on the label's width AT THAT
     * MOMENT (near-zero for an empty/NULL-text label); widening it
     * afterward does not retroactively recompute that position, so
     * re-align explicitly after setting width/wrap. */
    wifi_setup_detail = make_label(scr_wifi_setup, f14, C_TEXT_2, 176, NULL);
    lv_label_set_long_mode(wifi_setup_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wifi_setup_detail, 210);
    lv_obj_set_style_text_align(wifi_setup_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(wifi_setup_detail, LV_ALIGN_TOP_MID, 0, 176);
}

/* ── Per-page refresh ─────────────────────────────────────────────────── */

static void refresh_waiting(const char *msg) {
    lv_label_set_text(waiting_label, msg);
    lv_scr_load(scr_waiting);
}

static void refresh_live(void) {
    int pool_connected = kv_bool("pool_connected");
    int ipc_connected = kv_bool("ipc_connected");
    int have_hashrate = kv_str("hashrate_ghs") != NULL;
    int paused = kv_bool("paused");

    const char *status_text;
    lv_color_t status_color;
    if (paused) {
        status_text = "PAUSED";
        status_color = C_YELLOW;
    } else if (pool_connected && ipc_connected && have_hashrate) {
        status_text = "MINING";
        status_color = C_GREEN;
    } else if (pool_connected || ipc_connected) {
        status_text = "CONNECTING";
        status_color = C_YELLOW;
    } else {
        status_text = "OFFLINE";
        status_color = C_RED;
    }
    lv_label_set_text(live_status, status_text);
    lv_obj_set_style_text_color(live_status, status_color, LV_PART_MAIN);

    /* lv_meter has no post-creation indicator-color setter -- the struct
     * is public (lv_meter.h), so mutate the field directly and invalidate
     * to force a redraw with the new color. */
    live_arc->type_data.arc.color = status_color;

    char buf[64];
    uint32_t ghs;
    int32_t gauge_deciTH = 0;
    if (paused) {
        lv_label_set_text(live_hashrate_big, "IDLE");
        lv_obj_set_style_text_color(live_hashrate_big, C_TEXT_3, LV_PART_MAIN);
    } else if (kv_u32("hashrate_ghs", &ghs) && ghs > 0) {
        snprintf(buf, sizeof(buf), "%.2f", ghs / 1000.0);
        lv_label_set_text(live_hashrate_big, buf);
        lv_obj_set_style_text_color(live_hashrate_big, C_TEXT_1, LV_PART_MAIN);
        gauge_deciTH = (int32_t)((ghs + 50) / 100);
        if (gauge_deciTH > HASHRATE_GAUGE_MAX) {
            gauge_deciTH = HASHRATE_GAUGE_MAX;
        }
    } else {
        lv_label_set_text(live_hashrate_big, "--");
        lv_obj_set_style_text_color(live_hashrate_big, C_TEXT_3, LV_PART_MAIN);
    }
    lv_meter_set_indicator_end_value(live_meter, live_arc, gauge_deciTH);

    /* Power ring -- fixed accent color (this one isn't a pass/fail signal
     * like temp is, just "how close to the fill"), clamped to the gauge's
     * own range so an out-of-range reading still shows as visually maxed
     * rather than silently misbehaving. */
    double power_w;
    int have_power = kv_f64("power_w", &power_w);
    int32_t gauge_power = 0;
    if (have_power) {
        gauge_power = (int32_t)(power_w + 0.5);
        if (gauge_power < 0) gauge_power = 0;
        if (gauge_power > POWER_GAUGE_MAX) gauge_power = POWER_GAUGE_MAX;
    }
    lv_meter_set_indicator_end_value(live_meter, live_arc_power, gauge_power);

    /* Temp ring -- same thermal-threshold coloring as the rest of the UI. */
    double avg;
    int have_temp = kv_f64("temp_avg", &avg);
    int32_t gauge_temp = 0;
    lv_color_t temp_color = C_TEXT_3;
    if (have_temp) {
        gauge_temp = (int32_t)(avg + 0.5);
        if (gauge_temp < 0) gauge_temp = 0;
        if (gauge_temp > TEMP_GAUGE_MAX) gauge_temp = TEMP_GAUGE_MAX;
        temp_color = temp_color_c(avg);
    }
    live_arc_temp->type_data.arc.color = temp_color;
    lv_meter_set_indicator_end_value(live_meter, live_arc_temp, gauge_temp);

    lv_obj_invalidate(live_meter);

    /* Compact center readout -- the rings already carry hashrate/power/temp
     * at a glance, this is just the precise numbers, kept terse since the
     * innermost ring's clearance (inside the temp ring) is only ~58px
     * radius before running into the rings' fixed start point at the
     * 135 degree (bottom-left) position. Full diff/PLL/voltage detail is
     * one button press away on the diagnostics page. */
    if (have_temp && have_power) {
        snprintf(buf, sizeof(buf), "%.1fC  #" C_IP_BLUE_HEX " %.0fW#", avg, power_w);
    } else if (have_temp) {
        snprintf(buf, sizeof(buf), "%.1fC", avg);
    } else if (have_power) {
        snprintf(buf, sizeof(buf), "#" C_IP_BLUE_HEX " %.0fW#", power_w);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    lv_label_set_text(live_temp_small, buf);
    lv_obj_set_style_text_color(live_temp_small, temp_color, LV_PART_MAIN);

    const char *shares = kv_str("shares_found");
    lv_label_set_text(live_pool_small, shares ? shares : "0");

    lv_scr_load(scr_live);
}

static void refresh_diag(void) {
    char buf[64];
    uint32_t chips;
    if (kv_u32("asics_total", &chips)) {
        snprintf(buf, sizeof(buf), "%u / 12", chips);
        lv_label_set_text(diag_chips_label, buf);
        lv_color_t chips_color = chips >= 12 ? C_GREEN : (chips == 0 ? C_RED : C_YELLOW);
        lv_obj_set_style_text_color(diag_chips_label, C_TEXT_1, LV_PART_MAIN);
        diag_chips_arc->type_data.arc.color = chips_color;
        lv_meter_set_indicator_end_value(diag_chips_meter, diag_chips_arc, (int32_t)chips);
    } else {
        lv_label_set_text(diag_chips_label, "--");
        lv_obj_set_style_text_color(diag_chips_label, C_TEXT_3, LV_PART_MAIN);
        diag_chips_arc->type_data.arc.color = C_TEXT_3;
        lv_meter_set_indicator_end_value(diag_chips_meter, diag_chips_arc, 0);
    }
    lv_obj_invalidate(diag_chips_meter);

    uint32_t p0, p1, p2, p3;
    if (kv_u32("pll0", &p0) && kv_u32("pll1", &p1) && kv_u32("pll2", &p2) && kv_u32("pll3", &p3)) {
        snprintf(buf, sizeof(buf), "%u  %u  %u  %u", p0, p1, p2, p3);
        lv_label_set_text(diag_pll, buf);
        lv_obj_set_style_text_color(diag_pll, C_TEXT_1, LV_PART_MAIN);
    } else {
        lv_label_set_text(diag_pll, "--");
        lv_obj_set_style_text_color(diag_pll, C_TEXT_3, LV_PART_MAIN);
    }

    uint32_t mv;
    if (kv_u32("voltage_mv", &mv)) {
        snprintf(buf, sizeof(buf), "%u MV", mv);
        lv_label_set_text(diag_voltage, buf);
        lv_obj_set_style_text_color(diag_voltage, C_TEXT_1, LV_PART_MAIN);
    } else {
        lv_label_set_text(diag_voltage, "--");
        lv_obj_set_style_text_color(diag_voltage, C_TEXT_3, LV_PART_MAIN);
    }

    const char *err = kv_str("err_crc");
    snprintf(buf, sizeof(buf), "ERR_CRC %s", err ? err : "0");
    lv_label_set_text(diag_err, buf);

    lv_scr_load(scr_diag);
}

static void refresh_ip(void) {
    char ip[32];
    if (wlan_ip(ip, sizeof(ip))) {
        lv_label_set_text(ip_value, ip);
        lv_obj_clear_flag(ip_caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ip_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ip_no_wifi, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ip_caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ip_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ip_no_wifi, LV_OBJ_FLAG_HIDDEN);
    }
    lv_scr_load(scr_ip);
}

/* BLE WiFi-setup progress -- reads BLE_WIFI_STATUS_FILE (written by
 * ble_setup.rs), maps its `state` field to a short status line + color
 * and an optional `detail` line (device name while waiting, SSID while
 * connecting, IP on success). Falls back to a generic "waiting" message
 * if the file doesn't exist yet (e.g. right at boot before ble_setup
 * has written anything). */
static void refresh_wifi_setup(void) {
    /* ring_word: short, fits inside the ring at f24. sentence: full
     * status line shown below the ring. detail_buf: SSID/IP/device-name
     * context line under that. */
    const char *ring_word = "...";
    const char *sentence = "Starting...";
    lv_color_t color = C_TEXT_3;
    int32_t fill = 0;
    char detail_buf[80] = "";

    if (read_kv_file(BLE_WIFI_STATUS_FILE)) {
        const char *state = kv_str("state");
        const char *detail = kv_str("detail");
        if (!state) state = "";
        if (!detail) detail = "";

        if (strcmp(state, "advertising") == 0) {
            ring_word = "BLE";
            sentence = "Waiting for phone";
            color = C_YELLOW;
            fill = 20;
            if (detail[0]) snprintf(detail_buf, sizeof(detail_buf), "Open Avalon Life,\nlook for %s", detail);
        } else if (strcmp(state, "client_connected") == 0) {
            ring_word = "LINK";
            sentence = "Phone connected";
            color = C_YELLOW;
            fill = 40;
        } else if (strcmp(state, "credentials_received") == 0) {
            ring_word = "SAVE";
            sentence = "Wifi info received";
            color = C_YELLOW;
            fill = 60;
            if (detail[0]) snprintf(detail_buf, sizeof(detail_buf), "%s", detail);
        } else if (strcmp(state, "connecting") == 0) {
            ring_word = "...";
            sentence = "Connecting";
            color = C_YELLOW;
            fill = 80;
            if (detail[0]) snprintf(detail_buf, sizeof(detail_buf), "%s", detail);
        } else if (strcmp(state, "connected") == 0) {
            ring_word = "OK";
            sentence = "Wifi connected!";
            color = C_GREEN;
            fill = 100;
            if (detail[0]) snprintf(detail_buf, sizeof(detail_buf), "IP %s", detail);
        } else if (strcmp(state, "failed") == 0) {
            ring_word = "X";
            sentence = "Setup failed";
            color = C_RED;
            fill = 100;
            snprintf(detail_buf, sizeof(detail_buf), "Check password,\ntry again");
        }
    }

    lv_label_set_text(wifi_setup_status, ring_word);
    lv_obj_set_style_text_color(wifi_setup_status, color, LV_PART_MAIN);
    lv_obj_align(wifi_setup_status, LV_ALIGN_TOP_MID, 0, 78);

    wifi_setup_arc->type_data.arc.color = color;
    lv_meter_set_indicator_end_value(wifi_setup_meter, wifi_setup_arc, fill);
    lv_obj_invalidate(wifi_setup_meter);

    char full_detail[120];
    if (detail_buf[0]) {
        snprintf(full_detail, sizeof(full_detail), "%s\n%s", sentence, detail_buf);
    } else {
        snprintf(full_detail, sizeof(full_detail), "%s", sentence);
    }
    lv_label_set_text(wifi_setup_detail, full_detail);

    lv_scr_load(scr_wifi_setup);
}

/* Raw full-screen static image -- bypasses LVGL entirely, same as
 * fb_draw.rs's Screen::load_image()+flush() for this page. Drawn once on
 * page-change only (the source image never changes, unlike the original's
 * unconditional periodic redraw). */
static void refresh_pizza(void) {
    static uint8_t pizza_buf[FB_BYTES];
    FILE *f = fopen(PIZZA_FILE, "rb");
    if (!f) {
        return;
    }
    size_t n = fread(pizza_buf, 1, sizeof(pizza_buf), f);
    fclose(f);
    if (n != sizeof(pizza_buf)) {
        return;
    }
    int fd = open(FB_DEV, O_WRONLY);
    if (fd < 0) {
        return;
    }
    lseek(fd, 0, SEEK_SET);
    size_t written = 0;
    while (written < sizeof(pizza_buf)) {
        ssize_t wn = write(fd, pizza_buf + written, sizeof(pizza_buf) - written);
        if (wn <= 0) {
            break;
        }
        written += (size_t)wn;
    }
    close(fd);
}

/* ── Page-file polling loop (mirrors fb_draw.rs's run_live_nano3s()) ────── */

typedef enum { PG_LIVE, PG_DIAG, PG_IP, PG_PIZZA, PG_DOOM, PG_WIFI_SETUP } page_t;

static page_t parse_page(const char *s) {
    if (strcmp(s, "nano3s-diag") == 0) return PG_DIAG;
    if (strcmp(s, "ip") == 0) return PG_IP;
    if (strcmp(s, "pizza") == 0) return PG_PIZZA;
    if (strcmp(s, "wifi-setup") == 0) return PG_WIFI_SETUP;
    /* "doom" fully yields /dev/fb0 -- nano3s_doom owns the panel while
     * this page is selected, same idea as the pizza page's raw-image
     * bypass, just taken further: no refresh, no lv_timer_handler at all,
     * so nano3s_ui can never race nano3s_doom for framebuffer writes. */
    if (strcmp(s, "doom") == 0) return PG_DOOM;
    return PG_LIVE;
}

static void run_event_loop(void) {
    char last_page[64] = "";
    uint32_t last_refresh = now_ms() - REFRESH_INTERVAL_MS;
    uint32_t last_tick = now_ms();
    int was_doom = 0;

    for (;;) {
        uint32_t now = now_ms();
        lv_tick_inc(now - last_tick);
        last_tick = now;

        char page[64] = "nano3s";
        FILE *pf = fopen(PAGE_FILE, "r");
        if (pf) {
            if (fgets(page, sizeof(page), pf)) {
                page[strcspn(page, "\r\n")] = '\0';
            } else {
                strcpy(page, "nano3s");
            }
            fclose(pf);
        }

        page_t pg = parse_page(page);
        int page_changed = strcmp(page, last_page) != 0;

        if (was_doom && pg != PG_DOOM) {
            silence_buzzer();
        }
        was_doom = (pg == PG_DOOM);

        if (pg == PG_DOOM) {
            /* Touch nothing -- not even lv_timer_handler -- while doom
             * owns the panel. Just keep polling so we notice when the
             * page changes back. */
            strcpy(last_page, page);
            usleep(POLL_INTERVAL_MS * 1000);
            continue;
        }

        if (page_changed || now - last_refresh >= REFRESH_INTERVAL_MS) {
            strcpy(last_page, page);
            last_refresh = now;

            if (pg == PG_IP) {
                refresh_ip();
            } else if (pg == PG_PIZZA) {
                if (page_changed) {
                    refresh_pizza();
                }
            } else if (pg == PG_WIFI_SETUP) {
                refresh_wifi_setup();
            } else if (read_kv_file(NANO3S_LIVE_FILE)) {
                if (pg == PG_DIAG) {
                    refresh_diag();
                } else {
                    refresh_live();
                }
            } else {
                refresh_waiting("STARTING");
            }
        }

        if (pg != PG_PIZZA) {
            lv_timer_handler();
        }
        usleep(POLL_INTERVAL_MS * 1000);
    }
}

static void clear_framebuffer(void) {
    static uint8_t zeros[FB_BYTES];
    int fd = open(FB_DEV, O_WRONLY);
    if (fd < 0) {
        return;
    }
    lseek(fd, 0, SEEK_SET);
    size_t written = 0;
    while (written < sizeof(zeros)) {
        ssize_t n = write(fd, zeros + written, sizeof(zeros) - written);
        if (n <= 0) {
            break;
        }
        written += (size_t)n;
    }
    close(fd);
}

int nano3s_ui_run(void) {
    clear_framebuffer();
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, draw_buf1, NULL, HOR_RES * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = HOR_RES;
    disp_drv.ver_res = VER_RES;
    disp_drv.flush_cb = fb_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    build_screens();
    refresh_waiting("STARTING");

    run_event_loop();
    return 0;
}

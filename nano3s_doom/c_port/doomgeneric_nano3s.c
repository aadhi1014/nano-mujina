// doomgeneric platform layer for the Avalon Nano3s 240x240 RGB565 panel.
//
// Rendering (native 320x200 doom render, downscaled+letterboxed into the
// real 240x240 panel) and input (DG_GetKey backed by web_gamepad.c's
// touchscreen-over-LAN gamepad, since no keyboard/controller is attached)
// are both implemented here -- see nano3s_doom/README.md for the full
// picture of both pipelines.

#include "doomgeneric.h"
#include "doomkeys.h"
#include "web_gamepad.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// Touchscreen gamepad: no keyboard/controller is attached to this device,
// so input arrives over LAN instead -- see web_gamepad.c for the HTTP
// server and HTML page, and its header for why DG_GetKey can just forward
// straight to it (same drain-loop contract).
#define GAMEPAD_PORT 8080

#define FB_DEV "/dev/fb0"
#define PANEL_RES 240

// DOOMGENERIC_RESX/RESY are forced to DOOM's native 320x200 via -D on the
// compiler command line (see build.sh) rather than defined here, so every
// translation unit that includes doomgeneric.h agrees on the same value.
// doomgeneric's own auto-scaling in i_video.c only handles integer
// UPSCALING (dest/src, truncated) -- our 240px panel is smaller than
// 320x200 in both dimensions, so that path can't be used. Instead we take
// the native, unscaled 320x200 render and do our own downscale + letterbox
// into the real 240x240 panel below.
#define DOOM_W 320
#define DOOM_H 200

// 0.75x uniform scale (240/320 == 0.75, keeping 200*0.75 == 150 tall) --
// preserves the full field of view, letterboxed top/bottom. The black
// bars disappear into the panel's already-black background, and this
// panel is physically round anyway (the square framebuffer's corners are
// clipped by the bezel), so unused vertical space at the edges is normal.
#define SCALE_NUM 3
#define SCALE_DEN 4
#define SCALED_W (DOOM_W * SCALE_NUM / SCALE_DEN)  // 240
#define SCALED_H (DOOM_H * SCALE_NUM / SCALE_DEN)  // 150
#define LETTERBOX_Y ((PANEL_RES - SCALED_H) / 2)   // 45

_Static_assert(GAMEPAD_FRAME_W == PANEL_RES && GAMEPAD_FRAME_H == SCALED_H,
               "web_gamepad frame size must match this file's downscale output");

static struct timeval s_start_time;

static void fb_write_frame(const uint16_t *scaled_rows_buf) {
    int fd = open(FB_DEV, O_WRONLY);
    if (fd < 0) {
        return;
    }
    // Black rows are only ever the fixed top/bottom letterbox bands and
    // never touched again once written, so we don't need to re-clear them
    // every frame -- just seek past them and write the DOOM image band.
    off_t offset = (off_t)LETTERBOX_Y * PANEL_RES * sizeof(uint16_t);
    if (lseek(fd, offset, SEEK_SET) != (off_t)-1) {
        size_t total = (size_t)SCALED_H * PANEL_RES * sizeof(uint16_t);
        size_t written = 0;
        const uint8_t *src = (const uint8_t *)scaled_rows_buf;
        while (written < total) {
            ssize_t n = write(fd, src + written, total - written);
            if (n <= 0) {
                break;
            }
            written += (size_t)n;
        }
    }
    close(fd);
}

static void fb_clear(void) {
    static uint8_t zeros[PANEL_RES * PANEL_RES * 2];
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

void DG_Init(void) {
    // Clear the whole panel once up front -- everything outside the
    // SCALED_H image band (the letterbox bars) is written exactly once
    // here and never touched again by fb_write_frame().
    fb_clear();
    gettimeofday(&s_start_time, NULL);

    // Best-effort: a failed bind (port in use) just means no touch input,
    // not a fatal error -- DOOM still renders fine, same as before this
    // existed.
    web_gamepad_init(GAMEPAD_PORT);
}

void DG_DrawFrame(void) {
    // DG_ScreenBuffer is populated by i_video.c's cmap_to_fb() using
    // whatever -gfxmode was selected. We pass "-gfxmode rgb565" in argv
    // (see main() below), so despite pixel_t being declared uint32_t in
    // doomgeneric.h, the buffer is actually packed as 2-byte RGB565
    // values with no padding -- confirmed by reading i_video.c's own
    // cmap_to_fb(), which advances its output pointer by exactly
    // bits_per_pixel/8 bytes per pixel. That means it already matches
    // this panel's native pixel format exactly: no color conversion,
    // only the geometric downscale below.
    const uint16_t *src = (const uint16_t *)DG_ScreenBuffer;

    static uint16_t scaled[SCALED_H * PANEL_RES];
    for (int y = 0; y < SCALED_H; y++) {
        int src_y = y * SCALE_DEN / SCALE_NUM;
        if (src_y >= DOOM_H) {
            src_y = DOOM_H - 1;
        }
        const uint16_t *src_row = src + (size_t)src_y * DOOM_W;
        uint16_t *dst_row = scaled + (size_t)y * PANEL_RES;
        for (int x = 0; x < PANEL_RES; x++) {
            int src_x = x * SCALE_DEN / SCALE_NUM;
            if (src_x >= DOOM_W) {
                src_x = DOOM_W - 1;
            }
            dst_row[x] = src_row[src_x];
        }
    }

    fb_write_frame(scaled);

    // Same buffer, handed to the web gamepad's live preview (see
    // web_gamepad.c's GET /frame.bmp) -- GAMEPAD_FRAME_W/H are asserted
    // below to match PANEL_RES/SCALED_H exactly, so no extra copy or
    // resize is needed here.
    web_gamepad_set_frame(scaled);
}

void DG_SleepMs(uint32_t ms) {
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    struct timeval now;
    gettimeofday(&now, NULL);
    long seconds = now.tv_sec - s_start_time.tv_sec;
    long usec = now.tv_usec - s_start_time.tv_usec;
    return (uint32_t)(seconds * 1000 + usec / 1000);
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    return web_gamepad_get_key(pressed, doomKey);
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

// The buzzer is a stateful hardware device -- it keeps sounding whatever
// tone it was last told to play regardless of whether this process is
// still alive, and SIGKILL can't be caught by anything, so a hard `kill
// -9` leaves it stuck making noise forever. SIGTERM/SIGINT (plain `kill`,
// or Ctrl-C) are caught here so we always get a chance to silence it
// before exiting. See buzzer_music.c's nano3s_doom_emergency_silence().
#include <signal.h>
extern void nano3s_doom_emergency_silence(void);
static volatile sig_atomic_t s_should_exit = 0;
static void handle_shutdown_signal(int sig) {
    (void)sig;
    s_should_exit = 1;
}

int main(int argc, char **argv) {
    signal(SIGTERM, handle_shutdown_signal);
    signal(SIGINT, handle_shutdown_signal);

    doomgeneric_Create(argc, argv);
    while (!s_should_exit) {
        doomgeneric_Tick();
    }

    nano3s_doom_emergency_silence();
    web_gamepad_shutdown();
    return 0;
}

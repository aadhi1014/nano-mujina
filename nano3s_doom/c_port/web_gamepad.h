#ifndef NANO3S_WEB_GAMEPAD_H
#define NANO3S_WEB_GAMEPAD_H

#include <stdint.h>

// Starts a background HTTP server on the given TCP port serving a
// touchscreen gamepad page and turning its input into doomgeneric key
// events. Best-effort: returns 0 on success, -1 if the socket couldn't be
// created/bound (e.g. port in use) -- callers should treat that as
// non-fatal, since DOOM still renders fine with input effectively absent
// (matches the pre-gamepad behavior, where DG_GetKey always reported no
// key).
int web_gamepad_init(unsigned short port);

// Best-effort stop. Safe to call even if init failed or was never called.
// Not required for a clean process exit -- the server thread is detached
// and dies with the process either way -- but harmless to call from a
// graceful shutdown path.
void web_gamepad_shutdown(void);

// Mirrors DG_GetKey's own contract exactly (see i_input.c's
// `while (DG_GetKey(&pressed, &key))` drain loop): pops one queued event
// if present, returns 1 and fills *pressed/*doomKey; returns 0 once the
// queue is empty. Safe to call every tic.
int web_gamepad_get_key(int *pressed, unsigned char *doomKey);

// Must match doomgeneric_nano3s.c's own downscale output exactly (its
// PANEL_RES and SCALED_H) -- that file _Static_asserts this.
#define GAMEPAD_FRAME_W 240
#define GAMEPAD_FRAME_H 150

// Called once per rendered frame (~35Hz, DOOM's own tic rate) from
// DG_DrawFrame with the same RGB565 pixel data already being written to
// /dev/fb0, so the gamepad page's live preview mirrors the physical panel.
// pixels must be GAMEPAD_FRAME_W*GAMEPAD_FRAME_H uint16_t values,
// row-major. Cheap: just a mutex-guarded memcpy, no encoding happens here
// -- the BMP conversion only runs against whatever's latest when a browser
// actually requests /frame.bmp.
void web_gamepad_set_frame(const uint16_t *pixels);

#endif

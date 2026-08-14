#ifndef NANO3S_UI_DRIVER_H
#define NANO3S_UI_DRIVER_H

/* Entire LVGL lifecycle (init, display driver registration, screen build,
 * event loop) lives in C. The Rust side crosses the FFI boundary exactly
 * once, with a plain int return -- no LVGL struct ever crosses into Rust,
 * so there is no bindgen struct-layout/ABI mismatch to get wrong. */
int nano3s_ui_run(void);

#endif

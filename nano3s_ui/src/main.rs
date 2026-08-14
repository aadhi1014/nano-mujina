//! nano3s_ui -- LVGL-based replacement for fb_draw's raw-pixel renderer.
//!
//! LVGL lives entirely in C (c_driver/driver.c): init, display driver
//! registration, screen/widget construction, and the event loop. Rust
//! crosses the FFI boundary exactly once with a plain int return -- no LVGL
//! struct is ever represented on the Rust side, so there is no bindgen
//! struct-layout/ABI mismatch to get wrong.

extern "C" {
    fn nano3s_ui_run() -> std::os::raw::c_int;
}

fn main() {
    let rc = unsafe { nano3s_ui_run() };
    std::process::exit(rc);
}

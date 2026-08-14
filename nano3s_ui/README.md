# nano3s_ui

LVGL-based replacement for the original `fb_draw`/`fb_button` raw-framebuffer
renderer, driving the Avalon Nano3s's 240x240 RGB565 round display.

## Why this exists

The original display stack (`fb_hijack/src/fb_draw.rs`) drew everything by
hand: an 8x8 bitmap font, manual `rect`/`hline`/`vline` calls, one big
match statement per page. It worked, but every visual improvement meant
more raw pixel-pushing code. This project ports the same page set to LVGL
8.3.5 -- real anti-aliased fonts, real radial gauge widgets, proper
styling -- while keeping the same operational model: one page selected via
`PAGE_FILE`, one physical button cycling through pages, `NANO3S_LIVE_FILE`
as the telemetry source.

## Architecture

**All LVGL integration is hand-written C** (`c_driver/driver.c`), not the
`lvgl`/`lvgl-sys` Rust crates. That's not a style preference -- the Rust
bindings genuinely don't work for this target:

1. `lvgl-sys` 0.6.2 unconditionally bundles Redox OS's own `strlen`/
   `strcmp`/etc as `#[no_mangle]` exports (`src/string_impl.rs`), meant for
   bare-metal `no_std` targets with no real libc. On this real glibc-linux
   target those definitions win over glibc's own at static-link time,
   **program-wide** -- including inside std's and glibc's own pre-main
   startup code -- and crash before `main()` ever runs. Patched by
   vendoring `lvgl-sys` (`vendor/lvgl-sys/`) and cutting `mod string_impl;`
   out of `src/lib.rs` (see the file's own comment).
2. Even with that fixed, there's a second, deeper bug: a bindgen struct-
   layout/ABI mismatch between the Rust-side struct definitions and the
   actual C-compiled LVGL library for this riscv64 cross-target, causing a
   wild-pointer memory-corruption crash inside LVGL's own C code. Rather
   than debug bindgen's codegen, the whole approach was dropped.

The fix: skip bindgen and the Rust wrapper crate entirely. `build.rs`
compiles LVGL's actual C sources (still vendored at
`vendor/lvgl-sys/vendor/lvgl/`, just no longer used as a Rust dependency)
directly via the `cc` crate, alongside `c_driver/driver.c`. The Rust side
(`src/main.rs`) is a single `extern "C" fn nano3s_ui_run() -> c_int` call
-- no LVGL struct is ever represented in Rust, so the whole struct-layout-
mismatch bug class is structurally impossible.

`vendor/lvgl-sys/` still has the crate-wrapper files (`Cargo.toml`,
`src/lib.rs`, `shims/`) left over from the abandoned Rust-bindings attempt.
They're inert (nothing depends on that crate anymore) -- only
`vendor/lvgl-sys/vendor/{lvgl,include}/` (the actual C source + `lv_conf.h`)
is used by the real build. Cosmetic cleanup, not worth the churn risk.

## Pages

Polled from `PAGE_FILE` (`/mntapp/release/linux/app/fb_page`) every 100ms,
matching `fb_button.rs`'s write side exactly -- that file is the only
coupling between the two processes, no IPC. `fb_button.rs` itself was never
touched; it still just grabs `/dev/input/event2` and cycles the page name
through `nano3s -> nano3s-diag -> ip -> pizza -> nano3s` on release.

- **`nano3s`** (live telemetry, default): a speedometer-style radial gauge
  cluster -- three concentric rings (hashrate/power/temp, temp dynamically
  colored by the same thermal thresholds as everywhere else in this
  project), center readout for the precise numbers. Reads
  `NANO3S_LIVE_FILE` (`/tmp/nano3s_live.txt`).
- **`nano3s-diag`**: chips-online radial gauge + PLL/voltage/err_crc text.
- **`ip`**: wlan0 IP via `ifconfig wlan0` scrape (same parsing as the
  original).
- **`pizza`**: raw 115200-byte RGB565 file written directly to `/dev/fb0`,
  bypassing LVGL entirely (it's already framebuffer-native). LVGL's
  `lv_timer_handler()` is skipped while this page is active so it can't
  paint over the static image.
- **`doom`**: fully yields `/dev/fb0` -- see nano3s_doom's own README.
  No refresh, no `lv_timer_handler()` at all while this page is selected,
  so nano3s_ui can never race nano3s_doom for framebuffer writes. Also
  silences the hardware buzzer (`silence_buzzer()`) on any transition
  *away* from this page, regardless of how nano3s_doom stopped -- see the
  "stateful hardware" gotcha in nano3s_doom's README.

"6 pages" from an earlier planning pass was wrong -- `render_live_stats`/
`render_chip_temps` in the original `fb_draw.rs` are dead code from the
retired stock-cgminer stack; the real device only ever invokes
`fb_draw live-nano3s 2`, which never reaches those code paths.

## Gauges (`lv_meter` widgets)

LVGL 8.3's gauge widget (`lv_meter_create` + `lv_meter_add_scale` +
`lv_meter_add_arc`) supports multiple independent scales/indicators
layered on one widget -- that's how the three-ring hashrate/power/temp
cluster works, each ring its own scale with its own radius (`r_mod`) and
color, no needle (removed per user preference; the arc fill alone reads
cleanly). Indicator colors are set by mutating
`indicator->type_data.arc.color` directly and calling
`lv_obj_invalidate()` -- `lv_meter.h`'s struct is public and there's no
dedicated color setter in this LVGL version.

**Geometry gotcha worth knowing before touching gauge layout again:** the
arc's *start point* is fixed at the scale's `rotation` angle (135 degrees
here, the classic bottom-gap speedometer look) regardless of the current
value -- a 0% fill isn't invisible, it's a a real point at that angle. Any
center-readout text near that corner can visually collide with it. This
bit us once: a long "DIFF 2K 502 SH ~62W" text line at too-large a
y-offset had its left edge silently clipped by the arc ring. Fix was
two-fold: keep center text short (the ring's usable interior at a given
y-offset is bounded by `2*sqrt(inner_radius^2 - y_offset^2)`, work it out
before assuming text fits) and give the arc a couple more pixels of
clearance (`r_mod` closer to 0) for headroom.

## Fonts

Only `LV_FONT_MONTSERRAT_14` was enabled by default in the vendored
`lv_conf.h`; `_24` was turned on for headline numbers (gauge center
readouts, big page titles). `add_c_files()` in `build.rs` walks the whole
LVGL source tree unconditionally, including every font file under
`src/font/` -- enabling a font is purely a `lv_conf.h` `#define`, not a
build-script change (the `.c` file was already being compiled either way,
just emitting empty glyph data when its corresponding `LV_FONT_*` define
is 0).

## Build & deploy

```
cd nano3s_ui
export RUSTFLAGS="-C target-feature=+crt-static"   # mandatory -- see project-wide note below
cargo build --release
```

Static linking (`+crt-static`) is a hard project-wide requirement, not
specific to this crate: the device's glibc (2.33) is older than this WSL
toolchain's (2.39), and a dynamic build silently never starts on-device.

Deploy target is `/data/nano3s_ui` (not `/mntapp` -- that partition is
only ~7.4MB total and stays too tight for a ~1MB binary alongside
`fb_draw`'s own binary, which is kept as an instant rollback path).
`mujina_display_startup.sh`'s supervisor loop launches it; both
`deploy/mujina_display_startup.sh` (the real one, matches live device
config) and `mujina-upstream/deploy/mujina_display_startup.sh` (a
sanitized template with placeholder pool credentials) need the same edit
if the launch command ever changes.

**Overwriting a running binary on this device's flash filesystem (UBIFS)
can fail outright** (`scp: dest open ... Failure`) if the target path is
the currently-executing process's own image -- kill the process (or
`rm -f` the stale file so `scp` creates fresh rather than truncates-in-
place) before copying, then verify with a checksum, not just `scp`'s exit
code (a garbled/interleaved terminal capture from a stale respawn can look
like success when it wasn't).

Validate any change with a full **reboot**, not just a manual process
swap -- that's the only way to exercise the real
`rcS -> mujina_display_startup.sh` boot path end to end, and it's what
this project's testing protocol has used for every other production
change.

# nano3s_doom

DOOM (shareware) running on the Avalon Nano3s's 240x240 RGB565 panel, with
music played through the device's hardware buzzer. Selected as the `doom`
page in `nano3s_ui`, which fully yields the framebuffer while this runs.

## Why doomgeneric

`vendor_doomgeneric/` is a git clone of `ozkl/doomgeneric` (GPL2) --
a minimal portable DOOM source port designed specifically for this kind of
job: it strips the original DOOM source down to the actual game engine and
exposes a tiny platform API (`DG_Init`, `DG_DrawFrame`, `DG_SleepMs`,
`DG_GetTicksMs`, `DG_GetKey`, `DG_SetWindowTitle`) that any target -- SDL,
a framebuffer, an e-ink screen, whatever -- implements once. `c_port/`
holds the Nano3s implementation of that API; nothing in
`vendor_doomgeneric/doomgeneric/` itself was modified except the two
vendored-source patches below.

## WAD

`wad/DOOM1.WAD` is id Software's shareware WAD -- freely distributable
under id's own original shareware licensing, no purchase or key needed.
Sourced from the Internet Archive's `doom_20230531/doom_dos.ZIP`.
4,196,020 bytes, sha256
`1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`.

## Rendering: native 320x200 + custom downscale

doomgeneric's own `i_video.c` auto-scaler only handles integer *upscaling*
(`dest/src`, truncated) -- it has no path for a destination smaller than
DOOM's native resolution in both dimensions, which is exactly our
situation (240x240 panel vs DOOM's native 320x200). So `build.sh` forces
`DOOMGENERIC_RESX=320`/`RESY=200` (native, `fb_scaling` stays 1, no
artifacts from doomgeneric's own scaler), and `c_port/doomgeneric_nano3s.c`
does its own scale+letterbox entirely in `DG_DrawFrame()`:

- 0.75x uniform scale (240/320), keeping proportions correct: 200*0.75 =
  150px tall image, nearest-neighbor sampled.
- Letterboxed vertically (`(240-150)/2 = 45px` top/bottom bars), which
  disappear into the panel's black background -- and the panel is
  physically round anyway, so the square framebuffer's corners are already
  hidden by the bezel.
- `DG_ScreenBuffer` is read as `const uint16_t*` even though `pixel_t` is
  declared `uint32_t` in `doomgeneric.h` -- passing `-gfxmode rgb565` in
  `argv` (see `main()`) makes `i_video.c`'s `cmap_to_fb()` actually pack
  2-byte RGB565 pixels with no padding, confirmed by reading its own
  pointer-advance logic. That means the source buffer already matches the
  panel's native pixel format with zero color conversion needed -- only
  the geometric downscale above.
- The fixed top/bottom letterbox bands are written once in `DG_Init()`'s
  `fb_clear()` and never touched again; `fb_write_frame()` only writes the
  150-row image band each frame (via `lseek` past the top band), saving a
  full-framebuffer write every frame.

## Input: not yet implemented

`DG_GetKey()` is currently stubbed to always return "no key" -- DOOM sits
on the title screen and renders correctly, which alone proves WAD load +
engine init + the render pipeline all work, but there's no way to actually
play yet. The originally-agreed plan (user picked "web virtual gamepad")
is a minimal embedded HTTP server inside `nano3s_doom` serving touch
controls (move/turn/fire/use) over LAN into a key queue that `DG_GetKey`
drains -- not started.

## Music: MUS -> MIDI -> monophonic buzzer tones

DOOM's music is stored as Id's own MUS format (a compact single-track
event stream). The vendored `mus2mid.c` converts it to a standard MIDI
file at registration time -- MUS is always type-0/single-track/70-ticks-
per-quarter with no tempo event, so the resulting SMF's default 120 BPM is
correct without extra bookkeeping.

`c_port/buzzer_music.c` is a from-scratch SMF (Standard MIDI File) reader
(`midi_reader_t`, VLQ delta-times, running status, note on/off, meta
events for tempo/end-of-track, sysex skip) driving a background
`pthread` that walks the MIDI in 20ms slices and drives the buzzer through
`/dev/input/event1`'s `pwm-beeper` ABI (`EV_SND`/`SND_TONE`) -- tone-only
by hardware design, one voice, no volume control, no PCM.

**Monophonic arbitration.** The hardware can only sound one frequency at a
time, but DOOM's MIDI tracks are polyphonic (bass + melody + drums
overlapping). `sound_highest_held()` tracks all currently-held notes in
`s_held[]` and always sounds the *highest* pitch among them -- standard
"melody priority" technique for reducing polyphonic MIDI to one voice,
picked because melody lines read as more musically recognizable than bass
on a single square-wave voice.

Drum channel (MIDI channel 9, `MIDI_DRUM_CHANNEL`) is filtered out
entirely in `note_on`/`note_off` -- drum "notes" are percussion patch
selections, not pitches, and sounding them as tones is pure noise on a
single-voice buzzer.

**Two vendored-source patches were needed** for `FEATURE_SOUND` to even
compile: `i_sound.c`'s `#include <SDL_mixer.h>` is guarded out
(`#if 0`, with a nano3s comment) since no real SDL exists in this
toolchain and nothing in the file actually calls an SDL API -- the
`#include` was only there because upstream assumes SDL is always the
backend. `FEATURE_SOUND` itself has to stay *defined* (via `build.sh`'s
`CFLAGS`) so `InitMusicModule()` picks up our real `DG_music_module`
struct instead of the built-in no-op stub.

SFX (`DG_sound_module`) is a full no-op stub (`BZS_*` functions) --
intentionally silent, since the single-voice buzzer is already fully
committed to music and layering SFX on top would just cause more
arbitration flapping for no real gain.

### Audio-quality fixes (from direct listening feedback)

1. **"Crackly on low freq"** -- root cause was last-note-wins arbitration:
   a busy bass line and a sustained melody note were both firing
   `buzzer_set_tone()` on every event, causing the buzzer to flip rapidly
   between them even when the audible note shouldn't have changed. Fixed
   by switching to the highest-held-note-priority arbitration described
   above, so a busy low voice can't interrupt a held higher note.
2. **Articulation / redundant retriggers** -- `sound_highest_held()`
   tracks `s_sounding_channel`/`s_sounding_note` identity and only retunes
   the buzzer when the *actual* sounding note changes, not on every MIDI
   event that happens to touch the note set. When it does change, a short
   `ARTICULATION_GAP_MS` (12ms) silence gap is inserted between notes
   (`buzzer_set_tone(0)`, sleep, `buzzer_set_tone(new_freq)`) so consecutive
   same-voice notes are audibly distinct instead of blurring into a single
   pitch-bent tone.

### Stateful-hardware shutdown bug (SIGKILL vs SIGTERM)

The buzzer is stateful hardware -- once told to sound a tone, it keeps
sounding it regardless of whether the process that requested it is still
alive. `SIGKILL` (`kill -9`) cannot be caught by any process, so a hard
kill mid-note left the buzzer stuck buzzing indefinitely; the only
workaround discovered live was changing the display page, since
`fb_button`'s own beep incidentally overrode the stuck tone.

Fixed with two independent layers, both confirmed on hardware:

1. `doomgeneric_nano3s.c`'s `main()` installs `SIGTERM`/`SIGINT` handlers
   that set a flag checked each tick loop; on exit it calls the exported
   `nano3s_doom_emergency_silence()` (in `buzzer_music.c`) before
   returning. This only works for a graceful `kill` (not `-9`) or Ctrl-C.
2. Defense-in-depth in `nano3s_ui`'s `driver.c`: `silence_buzzer()` runs
   unconditionally on *any* page transition away from `doom`, regardless
   of how `nano3s_doom` actually stopped (including a hard `-9` kill).
   This is the layer that actually saves you if something SIGKILLs the
   process, since nothing in `nano3s_doom` itself gets a chance to run at
   that point.

Always prefer plain `kill <pid>` over `kill -9` when stopping
`nano3s_doom` manually, for exactly this reason -- `-9` still works
because of layer 2, but layer 1 is strictly better when available.

## PCM audio investigation (buzzer-as-speaker) -- concluded, not pursued

Investigated whether the buzzer could play real PCM audio instead of
discrete tones. Findings:

- The kernel driver behind `/dev/input/event1` is `pwm-beeper` --
  tone-only by design (`SND_TONE` events), no PCM path exists through it.
- No ALSA device backs this hardware at all (checked -- none present).
- The safe route to raw PWM would be the sysfs PWM class
  (`/sys/class/pwm/pwmchipN/export`), bit-banging duty cycle fast enough
  to approximate a waveform. **Empirically tested and confirmed blocked**:
  `echo 2 > /sys/class/pwm/pwmchip0/export` (channel 2, the beeper's own
  channel) produces no `pwm2` directory -- the channel is held exclusively
  by the in-kernel `pwm-beeper` driver and the sysfs core refuses a second
  claimant. This is a hard blocker via the safe path, not a guess.
- The remaining unexplored option is a `/dev/mem`-based approach that
  bypasses the kernel PWM subsystem entirely and drives the SoC's PWM
  peripheral registers directly. This is meaningfully higher-risk (no
  kernel arbitration, can conflict with whatever else touches that
  peripheral, wrong register writes can hang or damage the SoC) and was
  explicitly not attempted -- current direction ("stick with the tone
  music") closes this investigation rather than pursuing it further.

## Build

```
cd nano3s_doom
./build.sh
```

Plain shell script, no cargo involved -- direct `riscv64-linux-gnu-gcc`
invocation matching this project's existing `cc`-based precedent
elsewhere. `+crt-static` equivalent is baked in via `-static` on the final
link line (mandatory project-wide -- device's glibc 2.33 vs toolchain's
2.39, a dynamic build silently never starts on-device). Compiles the full
vendored DOOM source list (`SRC_DOOM` in `build.sh`) plus
`doomgeneric_nano3s.c` and `buzzer_music.c`, links with `-lm -lpthread`.
Output: `build/nano3s_doom`.

## Deploy / run

Same UBIFS overwrite-while-running gotcha as `nano3s_ui` applies here too
-- if `nano3s_doom` is currently running, `scp` over its own binary path
can fail (`scp: dest open ... Failure`); kill the process (plain `kill`,
see the shutdown section above) and/or `rm -f` the stale file first, then
verify the copy with `sha256sum`, not just `scp`'s exit code.

Launched by `nano3s_ui` when the `doom` page is selected (writes the page
name to `PAGE_FILE`, `nano3s_ui` spawns the process and yields
`/dev/fb0`/skips `lv_timer_handler()` entirely while that page is active
-- see `nano3s_ui/README.md`'s Pages section). Can also be run standalone
from a shell for testing; it doesn't depend on `nano3s_ui` being present,
only on having exclusive access to `/dev/fb0` and the buzzer device while
it runs.

## Known gaps

- No real input yet (`DG_GetKey` stubbed) -- title screen only, can't
  actually play. Web virtual gamepad was the agreed direction, not
  started.
- SFX intentionally silent (stubbed), music-only.
- "Add more notes" (arpeggiation / simulated polyphony on the single-voice
  buzzer, e.g. rapidly alternating between the top 2 held notes to fake a
  chord) was requested but not implemented -- deferred pending work.

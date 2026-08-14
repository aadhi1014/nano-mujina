#!/bin/sh
# Cross-compiles doomgeneric + our nano3s platform layer for riscv64gc-unknown-linux-gnu.
# Pure C, no cargo/rustc involved -- direct invocation, matching the project's
# existing cc-based precedent (mujina-miner/build.rs's nano3s_ipc_shim.c).
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
DG="$ROOT/vendor_doomgeneric/doomgeneric"
OUT="$ROOT/build"
CC=riscv64-linux-gnu-gcc

mkdir -p "$OUT"

# DOOM's native 320x200 render, unscaled (fb_scaling==1 in i_video.c) --
# our own platform layer does the downscale+letterbox to the real 240x240
# panel itself. Defined globally so every translation unit that includes
# doomgeneric.h agrees.
# FEATURE_SOUND is on so i_sound.c's InitMusicModule() picks up our real
# DG_music_module (buzzer_music.c) -- see that file's own header comment
# for how it turns DOOM's music into buzzer tones without any real SDL
# dependency (i_sound.c's one SDL_mixer.h include is patched out locally,
# see its own nano3s comment).
CFLAGS="-Os -Wall -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DFEATURE_SOUND -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200"

SRC_DOOM="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric mus2mid"

OBJS=""
for f in $SRC_DOOM; do
    echo "[doom] $f.c"
    "$CC" $CFLAGS -c "$DG/$f.c" -o "$OUT/$f.o"
    OBJS="$OBJS $OUT/$f.o"
done

echo "[nano3s] doomgeneric_nano3s.c"
"$CC" $CFLAGS -I "$DG" -c "$ROOT/c_port/doomgeneric_nano3s.c" -o "$OUT/doomgeneric_nano3s.o"
OBJS="$OBJS $OUT/doomgeneric_nano3s.o"

echo "[nano3s] buzzer_music.c"
"$CC" $CFLAGS -I "$DG" -c "$ROOT/c_port/buzzer_music.c" -o "$OUT/buzzer_music.o"
OBJS="$OBJS $OUT/buzzer_music.o"

echo "[nano3s] web_gamepad.c"
"$CC" $CFLAGS -I "$DG" -c "$ROOT/c_port/web_gamepad.c" -o "$OUT/web_gamepad.o"
OBJS="$OBJS $OUT/web_gamepad.o"

echo "[link] nano3s_doom"
"$CC" -static $CFLAGS $OBJS -lm -lpthread -o "$OUT/nano3s_doom"

echo "[done]"
file "$OUT/nano3s_doom"
ls -la "$OUT/nano3s_doom"

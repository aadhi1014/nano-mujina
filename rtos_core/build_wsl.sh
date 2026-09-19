#!/bin/bash
# Real musl RT-Smart build, run from WSL. Invoke as a script file (not
# `wsl bash -lc "..."` with an inline command string -- the inherited
# Windows PATH can contain parenthesized entries like "Program Files
# (x86)" that break bash's -c argument parsing; a script file sidesteps
# that). If invoking from Git-Bash-for-Windows, set MSYS_NO_PATHCONV=1
# first so it doesn't mangle /mnt/c/... paths before they reach WSL.
#
# Needs a real musl RISC-V64 cross-compiler, not on PATH by default --
# this project's own copy lives in a sibling `toolchain/` directory next
# to this repo (not included here; get one, e.g. musl.cc's
# riscv64-linux-musl cross toolchain, and point TOOLCHAIN_BIN at its
# bin/ dir, or extract into ../toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/).
# Do NOT substitute a glibc riscv64-linux-gnu-gcc here -- a glibc binary
# launched via /sharefs/init.sh silently fails to start at all under
# RT-Smart's msh.
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOLCHAIN_BIN="${TOOLCHAIN_BIN:-$SCRIPT_DIR/../toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin}"
export PATH="$TOOLCHAIN_BIN:/usr/bin:/bin"
cd "$SCRIPT_DIR"
[ -f vendor/sdk_resource/lib/libipcmsg_slave.a ] || bash tools/install_sdk.sh
rm -rf build
make CC=riscv64-unknown-linux-musl-gcc

#!/bin/bash
# Real musl RT-Smart build, run from WSL. Invoke via:
#   MSYS_NO_PATHCONV=1 wsl bash /mnt/c/Users/aathe/Desktop/nano3s/rtos_core/build_wsl.sh
# (not `wsl bash -lc "..."` with an inline command string -- the inherited
# Windows PATH contains "Program Files (x86)" and other parenthesized
# entries that break bash's -c argument parsing; a script file sidesteps
# that entirely. MSYS_NO_PATHCONV=1 stops Git-Bash-for-Windows from
# mangling the /mnt/c/... path before it reaches WSL.)
#
# Toolchain is a real musl RISC-V64 cross-compiler already extracted here
# (not on PATH by default -- an earlier session's `find` search for it
# failed because it only checked WSL/Windows PATH, not this directory).
# This is the SAME toolchain that produced the currently-deployed, real,
# musl `rtos_core.elf` that runs on the actual RT-Smart big core (confirmed
# 2026-07-23 -- see docs/BUILD_NOTES.md's status banner). Do NOT substitute
# WSL's riscv64-linux-gnu-gcc (glibc) here -- a glibc binary launched via
# /sharefs/init.sh silently fails to start at all under RT-Smart's msh.
set -e
export PATH=/mnt/c/Users/aathe/Desktop/nano3s/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin:/usr/bin:/bin
cd /mnt/c/Users/aathe/Desktop/nano3s/rtos_core
rm -rf build
make CC=riscv64-unknown-linux-musl-gcc

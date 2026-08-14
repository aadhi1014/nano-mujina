#!/bin/bash
# Cross-compiles mujina-minerd (Rust) for riscv64gc-unknown-linux-gnu with
# the nano3s feature enabled. Run from WSL; paths resolve relative to this
# script.
set -e
export PATH="$HOME/.cargo/bin:$PATH"
export CARGO_TARGET_RISCV64GC_UNKNOWN_LINUX_GNU_LINKER=riscv64-linux-gnu-gcc
# pkg-config cross-build settings, pointing libudev-sys at the riscv64
# sysroot. RUSTFLAGS builds a fully static binary (crt-static), statically
# linking glibc and libudev.a instead of linking them dynamically.
export PKG_CONFIG_ALLOW_CROSS=1
export PKG_CONFIG_SYSROOT_DIR=/home/toor/riscv64-sysroot
export PKG_CONFIG_PATH=/home/toor/riscv64-sysroot/usr/lib/riscv64-linux-gnu/pkgconfig
export RUSTFLAGS='-C target-feature=+crt-static'
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# The Cargo workspace root (Cargo.toml, mujina-miner/) is two levels up
# from this script.
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$WORKSPACE_ROOT/mujina-miner"
cargo build --release --target riscv64gc-unknown-linux-gnu --features nano3s --bin mujina-minerd
# Built binary path (workspace-rooted target dir).
BIN="$WORKSPACE_ROOT/target/riscv64gc-unknown-linux-gnu/release/mujina-minerd"
echo "[*] Verifying nano3s symbols are linked in..."
strings "$BIN" | grep -c nano3s_ipc_ || { echo "[!] no nano3s_ipc_* symbols found -- feature silently compiled out"; exit 1; }
ls -lh "$BIN"
riscv64-linux-gnu-strip --strip-all -o "$BIN.stripped" "$BIN"
ls -lh "$BIN.stripped"
echo "[+] Built $BIN.stripped"

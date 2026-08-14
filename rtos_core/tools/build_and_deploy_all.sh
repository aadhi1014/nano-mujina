#!/bin/bash
# One-shot: builds rtos_core.elf, builds mujina-minerd, then deploys both
# (plus everything else deploy_stock_to_nano3s.sh pushes) to a stock Nano3s
# over SSH. Just chains the three already-tracked scripts in order -- no
# new build logic here, so each stays independently runnable too.
#
# Run from WSL (needs the musl riscv64 cross-toolchain for rtos_core.elf,
# and the riscv64gc-unknown-linux-gnu Rust target + riscv64-linux-gnu-gcc
# for mujina-minerd -- see build_wsl.sh and build_mujina_minerd.sh for
# what each needs on PATH).
#
# Usage: DEVICE_IP=192.168.1.x bash build_and_deploy_all.sh
#    or: bash build_and_deploy_all.sh 192.168.1.x
set -e

DEVICE_IP="${DEVICE_IP:-$1}"
if [ -z "$DEVICE_IP" ]; then
    echo "Usage: DEVICE_IP=<ip> bash $0   (or pass the IP as \$1)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RTOS_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "[1/3] Building rtos_core.elf..."
bash "$RTOS_ROOT/build_wsl.sh"

echo "[2/3] Building mujina-minerd..."
bash "$SCRIPT_DIR/build_mujina_minerd.sh"

echo "[3/3] Deploying to $DEVICE_IP..."
DEVICE_IP="$DEVICE_IP" bash "$SCRIPT_DIR/deploy_stock_to_nano3s.sh"

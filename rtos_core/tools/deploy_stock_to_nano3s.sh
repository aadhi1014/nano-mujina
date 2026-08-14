#!/bin/bash
# Deploys the full rtos_core/mujina-minerd stack onto a stock, never-touched
# Avalon Nano3s device over SSH. Run from WSL (needs scp/ssh and the
# riscv64-linux-gnu-gcc / riscv64-unknown-linux-musl-gcc cross-toolchains on
# PATH); paths below are resolved relative to this script's location.
#
# Usage: DEVICE_IP=192.168.1.x bash deploy_stock_to_nano3s.sh
#    or: bash deploy_stock_to_nano3s.sh 192.168.1.x
#
# Everything pushed here is built from tracked source in this repo -- no
# dependency on any local-only backup folder. Display/UI extras (fb_button,
# fb_draw, backlight, the mascot image) are deliberately NOT included: they
# have no tracked source anywhere in this project and aren't needed for
# mining -- see the pushed nano-mujina repo history for why.
#
# What this pushes:
#   - rtos_core.elf + tools/init.sh          -> /sharefs/
#   - mujina-minerd                          -> /data/
#   - wdt_disable_hold, mujina_test_harness  -> built fresh from source
#     (rebuilding here, not committing binaries, matches how rtos_core.elf
#     and mujina-minerd are already handled)
#   - hardware_bringup.sh, mujina_display_startup.sh (from deploy/)
#                                             -> /mntapp/release/linux/app/
#   - tools/rcS                              -> /etc/init.d/rcS (backed up
#     first)
#
# SAFETY:
#   - Refuses to run if the target's rcS already references
#     mujina_display_startup.sh (i.e. it's not actually stock).
#   - Backs up the target's original /etc/init.d/rcS and stock mm_miner
#     binary (if present) with a timestamp before touching anything.
#   - Every file copy uses scp-to-temp-name + mv (atomic rename), never
#     overwrites a file directly.
#   - Reboots at the end -- nothing here takes effect without one.
#   - hardware_bringup.sh's ordering inside rcS (after /data mounts, before
#     /sharefs is set up) has NOT been live-tested on real hardware -- it's
#     reasoned through from the boot sequence, not confirmed. Watch the
#     first boot closely.
set -e

DEVICE_IP="${DEVICE_IP:-$1}"
if [ -z "$DEVICE_IP" ]; then
    echo "Usage: DEVICE_IP=<ip> bash $0   (or pass the IP as \$1)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RTOS_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$RTOS_ROOT/.." && pwd)"
RTOS_ELF="$RTOS_ROOT/build/rtos_core.elf"
MUJINA_MINERD="$PROJECT_ROOT/mujina-upstream/target/riscv64gc-unknown-linux-gnu/release/mujina-minerd.stripped"
SSH="ssh -o StrictHostKeyChecking=no root@$DEVICE_IP"
SCP="scp -o StrictHostKeyChecking=no"
TS="$(date +%Y%m%d_%H%M%S)"
BUILD_DIR="$RTOS_ROOT/build/deploy_tmp"

echo "[*] Checking connectivity to $DEVICE_IP..."
$SSH "echo up" >/dev/null || { echo "[!] Cannot reach $DEVICE_IP"; exit 1; }

echo "[*] Checking this looks like a stock (not already-migrated) device..."
if $SSH "grep -q mujina_display_startup.sh /etc/init.d/rcS" 2>/dev/null; then
    echo "[!] /etc/init.d/rcS on $DEVICE_IP already references mujina_display_startup.sh."
    echo "    This device looks already-migrated -- this script is for a stock device only."
    exit 1
fi

for f in "$RTOS_ELF" "$MUJINA_MINERD" "$SCRIPT_DIR/rcS" "$SCRIPT_DIR/init.sh" \
         "$SCRIPT_DIR/hardware_bringup.sh" "$PROJECT_ROOT/deploy/mujina_display_startup.sh"; do
    [ -f "$f" ] || { echo "[!] Missing required local file: $f"; exit 1; }
done

echo "[*] Building wdt_disable_hold and mujina_test_harness from source..."
mkdir -p "$BUILD_DIR"
riscv64-linux-gnu-gcc -Wall -Wextra -O2 -static -o "$BUILD_DIR/wdt_disable_hold" "$SCRIPT_DIR/wdt_disable_hold.c"
riscv64-linux-gnu-gcc -Wall -Wextra -O2 -static -o "$BUILD_DIR/mujina_test_harness" "$SCRIPT_DIR/mujina_test_harness.c"

echo "[*] Backing up the device's stock /etc/init.d/rcS..."
$SSH "cp /etc/init.d/rcS /etc/init.d/rcS.stock_backup_$TS"

echo "[*] Backing up stock mm_miner binary, if present..."
$SSH "[ -f /data/mm_miner ] && cp /data/mm_miner /data/mm_miner.stock_backup_$TS || true"

echo "[*] Ensuring target directories exist..."
$SSH "mkdir -p /mntapp/release/linux/app /sharefs"

echo "[*] Pushing rtos_core.elf + init.sh..."
$SCP "$RTOS_ELF" "root@$DEVICE_IP:/sharefs/rtos_core.elf.new"
$SCP "$SCRIPT_DIR/init.sh" "root@$DEVICE_IP:/sharefs/init.sh.new"
$SSH "chmod +x /sharefs/rtos_core.elf.new /sharefs/init.sh.new && mv /sharefs/rtos_core.elf.new /sharefs/rtos_core.elf && mv /sharefs/init.sh.new /sharefs/init.sh"

echo "[*] Pushing mujina-minerd..."
$SCP "$MUJINA_MINERD" "root@$DEVICE_IP:/data/mujina-minerd.new"
$SSH "chmod +x /data/mujina-minerd.new && mv /data/mujina-minerd.new /data/mujina-minerd"

echo "[*] Pushing app/ binaries and scripts..."
for f in "$BUILD_DIR/wdt_disable_hold" "$BUILD_DIR/mujina_test_harness" \
         "$SCRIPT_DIR/hardware_bringup.sh" "$PROJECT_ROOT/deploy/mujina_display_startup.sh"; do
    name="$(basename "$f")"
    $SCP "$f" "root@$DEVICE_IP:/mntapp/release/linux/app/$name.new"
    $SSH "chmod +x /mntapp/release/linux/app/$name.new && mv /mntapp/release/linux/app/$name.new /mntapp/release/linux/app/$name"
done

echo "[*] Pushing the new rcS (replaces stock rcS entirely)..."
$SCP "$SCRIPT_DIR/rcS" "root@$DEVICE_IP:/etc/init.d/rcS.new"
$SSH "chmod +x /etc/init.d/rcS.new && mv /etc/init.d/rcS.new /etc/init.d/rcS"

echo "[+] Deploy complete. Rebooting $DEVICE_IP to apply..."
$SSH "reboot" || true

echo "[*] Waiting for device to come back..."
for i in $(seq 1 40); do
    sleep 5
    if $SSH -o ConnectTimeout=3 "echo up" 2>/dev/null | grep -q up; then
        echo "[+] Device back after ~$((i*5))s"
        break
    fi
done

echo "[*] Post-deploy check (give it ~20s to finish bringing up mining)..."
sleep 20
$SSH "tail -5 /data/mujina-minerd.log 2>&1; echo '---'; ps | grep -E \"mujina|rtos_core\" | grep -v grep"

echo
echo "[+] Done. If anything looks wrong: the original stock rcS is backed up at"
echo "    /etc/init.d/rcS.stock_backup_$TS on the device -- restore it and reboot to revert."

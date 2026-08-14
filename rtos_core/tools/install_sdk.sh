#!/bin/bash
# Fetches the one Canaan K230 SDK artifact rtos_core's build actually links
# against -- the slave-side libipcmsg.a -- straight from Kendryte's own
# public repo (https://github.com/kendryte/k230_sdk), and places it where
# the Makefile expects it. Nothing SDK-derived is committed to this repo;
# this script is the "get it yourself" step that replaces that.
#
# Only libipcmsg_slave.a is fetched because it's the only vendor library
# the Makefile's LDLIBS actually references -- see rtos_core/Makefile's
# own comment block for how that was confirmed. The public SDK repo itself
# names both host and slave variants "libipcmsg.a" (they live in sibling
# host/ and slave/ directories); this script renames the slave one on the
# way in so it doesn't collide with anything, matching the name the
# Makefile links against.
#
# Uses a blobless, sparse, depth-1 clone so this doesn't pull the rest of
# k230_sdk (kernel, u-boot, buildroot -- multiple GB) just for one ~200KB
# static library.
#
# Run from WSL (needs git+network). Usage:
#   bash install_sdk.sh                 # fetch if missing
#   bash install_sdk.sh --force         # re-fetch even if already present
#   K230_SDK_REF=<branch/tag/sha> bash install_sdk.sh   # pin a specific ref
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RTOS_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

K230_SDK_URL="https://github.com/kendryte/k230_sdk.git"
K230_SDK_REF="${K230_SDK_REF:-main}"
IPCMSG_COMPONENT_PATH="src/common/cdk/user/component/ipcmsg"
DEST="$RTOS_ROOT/vendor/sdk_resource/lib/libipcmsg_slave.a"

FORCE=0
[ "$1" = "--force" ] && FORCE=1

if [ -f "$DEST" ] && [ "$FORCE" -eq 0 ]; then
    echo "[*] $DEST already present -- skipping fetch (pass --force to re-fetch)."
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "[*] Fetching kendryte/k230_sdk@$K230_SDK_REF (ipcmsg component only)..."
git clone --filter=blob:none --no-checkout --depth 1 --branch "$K230_SDK_REF" "$K230_SDK_URL" "$WORK/sdk"
cd "$WORK/sdk"
git sparse-checkout init --cone
git sparse-checkout set "$IPCMSG_COMPONENT_PATH"
git checkout --quiet

SRC="$WORK/sdk/$IPCMSG_COMPONENT_PATH/slave/lib/libipcmsg.a"
[ -f "$SRC" ] || { echo "[!] Expected file not found at $SRC -- k230_sdk's layout may have changed."; exit 1; }

mkdir -p "$(dirname "$DEST")"
cp "$SRC" "$DEST"

echo "[*] Verifying kd_ipcmsg_connect is present in the fetched archive..."
riscv64-unknown-linux-musl-nm "$DEST" 2>/dev/null | grep -q kd_ipcmsg_connect \
    || nm "$DEST" 2>/dev/null | grep -q kd_ipcmsg_connect \
    || { echo "[!] kd_ipcmsg_connect not found in $DEST -- got the wrong file or a stripped one."; exit 1; }

echo "[+] Installed $DEST"

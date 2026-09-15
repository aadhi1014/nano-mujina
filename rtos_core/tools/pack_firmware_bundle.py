#!/usr/bin/env python3
"""
Packs mujina-minerd and/or the harness binary into one file for
POST /api/v0/firmware/bundle -- applies both in a single reboot instead
of uploading each separately (which reboots once per upload).

Format (see post_firmware_bundle's parser in mujina-miner/src/api/v0.rs):
    8 bytes   magic "MJBUNDL1"
    4 bytes   mujina-minerd section length, little-endian (0 = omitted)
    N bytes   mujina-minerd binary (if length > 0)
    4 bytes   harness section length, little-endian (0 = omitted)
    M bytes   harness binary (if length > 0)

Usage:
    python pack_firmware_bundle.py --out bundle.bin \
        --mujina-minerd ../../target/riscv64gc-unknown-linux-gnu/release/mujina-minerd.stripped \
        --harness mujina_test_harness

Either --mujina-minerd or --harness may be omitted (but not both) to
build a bundle that only touches one binary while still going through
the single-reboot bundle endpoint.

Then upload it:
    curl -X POST --data-binary @bundle.bin \
        -H "Content-Type: application/octet-stream" \
        http://<device>/api/v0/firmware/bundle
"""
import argparse
import struct
import sys

MAGIC = b"MJBUNDL1"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="Output bundle file path")
    ap.add_argument("--mujina-minerd", metavar="PATH", help="Path to the mujina-minerd binary to include")
    ap.add_argument("--harness", metavar="PATH", help="Path to the mujina_test_harness binary to include")
    args = ap.parse_args()

    if not args.mujina_minerd and not args.harness:
        print("error: need at least one of --mujina-minerd / --harness", file=sys.stderr)
        sys.exit(1)

    def read_or_empty(path):
        if not path:
            return b""
        with open(path, "rb") as f:
            return f.read()

    minerd_bytes = read_or_empty(args.mujina_minerd)
    harness_bytes = read_or_empty(args.harness)

    with open(args.out, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", len(minerd_bytes)))
        f.write(minerd_bytes)
        f.write(struct.pack("<I", len(harness_bytes)))
        f.write(harness_bytes)

    total = len(MAGIC) + 4 + len(minerd_bytes) + 4 + len(harness_bytes)
    print(f"Wrote {args.out} ({total} bytes)")
    if minerd_bytes:
        print(f"  mujina-minerd: {len(minerd_bytes)} bytes")
    if harness_bytes:
        print(f"  harness:       {len(harness_bytes)} bytes")


if __name__ == "__main__":
    main()

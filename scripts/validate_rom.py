#!/usr/bin/env python3
"""Canonicalise and verify the supported Rocket: Robot on Wheels US ROM."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zlib
from pathlib import Path

EXPECTED_SHA1 = "622D71A44DA0B81EA68092CAC9198C66154A4F4A"
EXPECTED_SIZE = 0xC00000
EXPECTED_CRC32 = "E0399F23"
Z64_MAGIC = bytes.fromhex("80371240")
V64_MAGIC = bytes.fromhex("37804012")
N64_MAGIC = bytes.fromhex("40123780")


def canonicalise(data: bytes) -> tuple[bytes, str]:
    if len(data) < 4:
        raise ValueError("file is too small to be an N64 ROM")
    magic = data[:4]
    if magic == Z64_MAGIC:
        return data, "z64/big-endian"
    if magic == V64_MAGIC:
        out = bytearray(data)
        for i in range(0, len(out) - 1, 2):
            out[i], out[i + 1] = out[i + 1], out[i]
        return bytes(out), "v64/byte-swapped"
    if magic == N64_MAGIC:
        out = bytearray(data)
        for i in range(0, len(out) - 3, 4):
            out[i:i + 4] = reversed(out[i:i + 4])
        return bytes(out), "n64/little-endian"
    raise ValueError(f"unrecognised N64 byte-order magic {magic.hex().upper()}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--metadata", type=Path)
    args = parser.parse_args()

    raw = args.input.read_bytes()
    data, source_order = canonicalise(raw)
    if len(data) != EXPECTED_SIZE:
        raise ValueError(f"ROM is {len(data)} bytes; supported NSUE dump is {EXPECTED_SIZE} bytes")
    sha1 = hashlib.sha1(data).hexdigest().upper()
    if sha1 != EXPECTED_SHA1:
        raise ValueError(
            "ROM does not match the supported unmodified US NSUE release.\n"
            f"Expected SHA-1: {EXPECTED_SHA1}\n"
            f"Found SHA-1:    {sha1}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    title = data[0x20:0x34].decode("ascii", errors="replace").rstrip("\0 ")
    game_code = data[0x3B:0x3F].decode("ascii", errors="replace")
    crc32 = f"{zlib.crc32(data) & 0xFFFFFFFF:08X}"
    if crc32 != EXPECTED_CRC32:
        raise ValueError(
            "ROM SHA-1 matched but CRC32 did not match the supported canonical dump. "
            f"Expected {EXPECTED_CRC32}, found {crc32}")
    metadata = {
        "source": str(args.input.resolve()),
        "sourceByteOrder": source_order,
        "canonicalPath": str(args.output.resolve()),
        "size": len(data),
        "sha1": sha1,
        "crc32": crc32,
        "headerTitle": title,
        "gameCode": game_code,
    }
    if args.metadata:
        args.metadata.parent.mkdir(parents=True, exist_ok=True)
        args.metadata.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

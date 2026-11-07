#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify a D24 helper compute fixture's raw 512-byte SSBO output.

With a Vulkan compute runner, dispatch one workgroup (64 invocations),
set 0 binding 0 = 512-byte storage buffer. Both subgroup sizes 32 and 64 use the
same reference. The fixture stores components 0 and 3 of the helper result.
Uniform masks use WorkgroupId.x ==/!= 0; do not dispatch additional groups.
No GPU calls are made by this verifier.
"""

import argparse
import struct
from pathlib import Path

# Same input contract as the D24 compute fixture in xbc2_cloud_candidate.cpp.
INPUT_BITS = (
    0x00000000, 0x80000000, 0x3f800000, 0x3f000000,
    0x3e000000, 0x3dfcd6e9, 0x3f7fffff, 0x3f800001,
    0xbf800000, 0x40000000, 0x7f800000, 0xff800000,
    0x7fc12345, 0xffc54321, 0x00800000, 0x80800000,
    0x33800001, 0x34000001, 0x3efffffd, 0x3effffff,
    0x3f000001, 0x3f000002, 0x3f7f0000, 0x3f7f0001,
    0x3f7f0002, 0x3f7fffff, 0x358637bd, 0x38d1b717,
    0x3f7fffef, 0x3eaaaaab, 0x3f000001, 0x3effffff,
)
MODES = ("uniform-off", "uniform-on", "lane-odd")


def float32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def quantized_bits(bits, enabled):
    value = struct.unpack("<f", struct.pack("<I", bits))[0]
    if not enabled or not 0.0 <= value <= 1.0:
        return bits  # Preserve original NaN payloads, sign and out-of-range values.
    packed = round(float32(value * 16777215.0))
    # Independent normalized-integer reference; no reciprocal or bit-increment algorithm.
    return struct.unpack("<I", struct.pack("<f", packed / 16777215.0))[0]


def reference(mode):
    if mode not in MODES:
        raise ValueError(mode)
    words = []
    for lane in range(64):
        enabled = mode == "uniform-on" or (mode == "lane-odd" and lane % 2 == 1)
        words.extend(quantized_bits(INPUT_BITS[(lane + shift) % 32], enabled)
                     for shift in (0, 13))
    return struct.pack("<128I", *words)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", nargs="?", type=Path, help="raw SSBO output to compare")
    parser.add_argument("--mode", choices=MODES, required=True)
    parser.add_argument("--reference", type=Path, help="write the expected 512-byte output")
    args = parser.parse_args()
    if args.output is None and args.reference is None:
        parser.error("provide an output to check or --reference")
    expected = reference(args.mode)
    if args.reference is not None:
        args.reference.write_bytes(expected)
        print(f"Wrote reference: {args.reference} ({len(expected)} bytes)")
    if args.output is None:
        return 0
    actual = args.output.read_bytes()
    if actual != expected:
        mismatches = 0
        for offset in range(0, min(len(actual), len(expected)) - 3, 4):
            got, want = actual[offset:offset + 4], expected[offset:offset + 4]
            if got != want:
                print(f"lane={offset // 8} component={0 if offset % 8 == 0 else 3} "
                      f"actual=0x{int.from_bytes(got, 'little'):08x} "
                      f"expected=0x{int.from_bytes(want, 'little'):08x}")
                mismatches += 1
                if mismatches == 8:
                    break
        print(f"MISMATCH: actual {len(actual)} bytes; expected {len(expected)}")
        return 1
    print(f"PASS: D24 {args.mode}, 64 invocations, components 0/3, 128 words bit exact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

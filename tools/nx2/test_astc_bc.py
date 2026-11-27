#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the offline Vulkan ASTC-to-BC comparison; see astc-bc-probe.md."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


SHADERS = "src/video_core/host_shaders"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--candidate-ref", help="Compile the fused shader from this Git revision")
    source.add_argument("--candidate-dir", type=Path, help="Directory containing astc_bc.comp")
    parser.add_argument("--reference-ref", default="origin/nx2")
    parser.add_argument("--reference-dir", type=Path,
                        help="Compile the baseline shaders from this directory instead of Git")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--quick", action="store_true", help="Only 4x4, 8x5 and 12x12")
    parser.add_argument("--extended", action="store_true", help="Also probe HDR CEM15 and UNORM8 thresholds")
    parser.add_argument("--allow-mismatch", action="store_true", help="Report differences without failing")
    args = parser.parse_args()

    tools = Path(__file__).resolve().parent
    repo = tools.parent.parent
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    def git(*cmd):
        return subprocess.check_output(["git", "-C", str(repo), *cmd])

    reference = git("rev-parse", "--verify", f"{args.reference_ref}^{{commit}}").decode().strip()
    candidate = (git("rev-parse", "--verify", f"{args.candidate_ref}^{{commit}}").decode().strip()
                 if args.candidate_ref else None)
    candidate_dir = None if candidate else args.candidate_dir or repo / SHADERS
    footprints = sorted({
        (int(w), int(h)) for w, h in re.findall(
            r"PIXEL_FORMAT_ELEM\(ASTC_2D_\d+X\d+_UNORM, (\d+), (\d+), 128\)",
            git("show", f"{reference}:src/video_core/surface.h").decode())
    })
    if not footprints:
        raise RuntimeError("No Lichii ASTC footprints found")
    if args.quick:
        footprints = [(4, 4), (8, 5), (12, 12)]

    provenance = {
        "reference": str(args.reference_dir.resolve()) if args.reference_dir else reference,
        "footprint_ref": reference,
        "candidate": candidate or str(candidate_dir.resolve()),
        "source_sha256": {},
    }
    for name, rev, source_dir, entries in (
        ("reference", reference, args.reference_dir, ("astc_decoder.comp", "bc_encoder.comp")),
        ("candidate", candidate, candidate_dir, ("astc_bc.comp",)),
    ):
        stage = out / (name + "-glsl")
        stage.mkdir(exist_ok=True)
        pending = list(entries)
        seen = set()
        while pending:
            file = pending.pop()
            if file in seen:
                continue
            seen.add(file)
            data = (source_dir / file).read_bytes() if source_dir else (
                git("show", f"{rev}:{SHADERS}/{file}"))
            path = stage / file
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            provenance["source_sha256"][f"{name}/{file}"] = hashlib.sha256(data).hexdigest()
            pending.extend(m.decode() for m in re.findall(rb'^\s*#include "([^"]+)"', data, re.M))

        for file in entries:
            binary = out / Path(file).with_suffix(".spv")
            subprocess.run(["glslangValidator", "-V", "--target-env", "spirv1.3",
                            "-DUseUniformBlock=1", f"-I{stage}", "-o", binary, stage / file],
                           check=True)
            subprocess.run(["spirv-val", "--target-env", "vulkan1.1", binary], check=True)
            if name == "candidate":
                spirv = subprocess.check_output(["spirv-dis", binary], text=True)
                provenance["candidate_has_refine_spec"] = bool(re.search(r"\bSpecId\s+1\b", spirv))
                offsets = [int(offset) for offset in re.findall(
                    r"\bOpMemberDecorate %PushConstants \d+ Offset (\d+)", spirv)]
                if len(offsets) not in (17, 18) or offsets != list(range(0, 4 * len(offsets), 4)):
                    raise RuntimeError(f"Unknown fused push ABI: {offsets}")
                provenance["candidate_push_words"] = len(offsets)

    binary = out / "astc_bc_probe"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-O2", "-Wall", "-Wextra",
                    "-Wno-missing-field-initializers", tools / "astc_bc_probe.cpp", "-o", binary,
                    "-lvulkan", "-lastcenc"], check=True)
    env = os.environ.copy()
    if "VK_DRIVER_FILES" not in env and "VK_ICD_FILENAMES" not in env:
        icds = sorted(Path("/usr/share/vulkan/icd.d").glob("lvp_icd*.json"))
        if not icds:
            raise RuntimeError("lavapipe ICD not found; set VK_DRIVER_FILES")
        env["VK_DRIVER_FILES"] = str(icds[0])
    provenance["icd"] = env.get("VK_DRIVER_FILES", env.get("VK_ICD_FILENAMES"))
    provenance["footprints"] = footprints
    provenance["extended"] = args.extended

    command = [binary, out, str(int(provenance["candidate_has_refine_spec"])),
               str(provenance["candidate_push_words"]), str(int(args.extended)),
               *(f"{w}x{h}" for w, h in footprints)]
    with (out / "run.log").open("w") as log, subprocess.Popen(
        command, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    ) as run:
        for line in run.stdout:
            log.write(line)
            print(line, end="")
        status = run.wait()
    if status in (0, 1):
        provenance["bc_sha256"] = {
            file: hashlib.sha256((out / file).read_bytes()).hexdigest()
            for file in ("previous.bc", "candidate.bc")
        }
    (out / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    return 0 if args.allow_mismatch and status == 1 else status


if __name__ == "__main__":
    sys.exit(main())

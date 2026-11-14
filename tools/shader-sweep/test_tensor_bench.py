# SPDX-License-Identifier: GPL-3.0-or-later

"""Contract tests for the tensor-bench CLI in emit-only mode.

Every case runs without a Vulkan device: emit-only lowers through the production
tensor path and writes SPIR-V 1.5 modules that spirv-val checks against the
vulkan1.2 environment when a validator is supplied via --spirv-val.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


BINARY = Path(sys.argv.pop(1)).resolve()
_parser = argparse.ArgumentParser(add_help=False)
_parser.add_argument("--spirv-val", dest="spirv_val")
_options, _remaining = _parser.parse_known_args()
VALIDATOR = shutil.which(_options.spirv_val or "spirv-val")
if _options.spirv_val and not VALIDATOR:
    _parser.error(f"SPIR-V validator is not executable: {_options.spirv_val}")
sys.argv[1:] = _remaining

SPIRV_MAGIC = 0x07230203
SPIRV_1_5 = 0x00010500
OP_ENTRY_POINT = 15
OP_VARIABLE = 59
OP_LOAD = 61
OP_STORE = 62
OP_ACCESS_CHAIN = 65
OP_DECORATE = 71
GL_COMPUTE = 5
STORAGE_BUFFER = 12
DESCRIPTOR_SET = 34
BINDING = 33

# CLI flag bounds; anything outside must be rejected as a bad CLI.
BAD_NUMERICS = (
    ("--operations", "0"), ("--operations", "65"), ("--operations", "-1"), ("--operations", "abc"),
    ("--depth", "0"), ("--depth", "12"), ("--depth", "32"),
    ("--local-size", "0"), ("--local-size", "48"),
    ("--workgroups", "0"), ("--workgroups", "4097"),
    ("--iterations", "0"), ("--iterations", "1001"),
    ("--compile-repeats", "0"), ("--compile-repeats", "21"),
    ("--seed", "-1"), ("--seed", "4294967296"),
    ("--device", "-1"), ("--device", "4294967296"),
)


def spirv_instructions(data):
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    offset = 5
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        assert size and offset + size <= len(words)
        yield opcode, words[offset + 1:offset + size]
        offset += size


class TensorBenchCliTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.output = self.root / "output.spv"

    def run_bench_raw(self, *args, env=None):
        return subprocess.run([str(BINARY), *map(str, args)], capture_output=True, text=True,
                              timeout=15, env=env)

    def run_bench(self, *args, code=0, env=None):
        result = self.run_bench_raw(*args, env=env)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        diagnostic = json.loads(result.stdout)
        self.assertEqual(diagnostic["schema_version"], 1)
        self.assertEqual(diagnostic["status"], "ok" if code == 0 else "error")
        if code == 0:
            self.assertIn("case", diagnostic)
            self.assertIn("shader", diagnostic)
        else:
            self.assertTrue(diagnostic.get("error", {}).get("message"), diagnostic)
        return diagnostic

    def validate(self, path):
        if not VALIDATOR:
            return
        result = subprocess.run([VALIDATOR, "--target-env", "vulkan1.2", str(path)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_tensor_module(self, path):
        """Sparing structural checks: SPIR-V 1.5 compute entry point plus an SSBO at
        set 0 binding 0 whose input loads and output stores survived emission."""
        data = path.read_bytes()
        self.assertGreater(len(data), 20 * 4)
        self.assertEqual(len(data) % 4, 0, "module must be word aligned")
        words = struct.unpack("<" + "I" * (len(data) // 4), data)
        self.assertEqual(words[0], SPIRV_MAGIC)
        self.assertEqual(words[1], SPIRV_1_5)
        instructions = list(spirv_instructions(data))
        entry_points = [args for opcode, args in instructions
                        if opcode == OP_ENTRY_POINT and args and args[0] == GL_COMPUTE]
        self.assertTrue(entry_points, "no GLCompute entry point")
        decorated = {decoration: {args[0] for opcode, args in instructions
                                  if opcode == OP_DECORATE and args[1:] == (decoration, value)}
                     for decoration, value in ((DESCRIPTOR_SET, 0), (BINDING, 0))}
        buffers = decorated[DESCRIPTOR_SET] & decorated[BINDING]
        self.assertTrue(buffers, "no SSBO decorated with set 0 binding 0")
        storage_classes = {args[1]: args[2] for opcode, args in instructions
                           if opcode == OP_VARIABLE and args[1] in buffers}
        buffers &= {target for target, storage in storage_classes.items()
                    if storage == STORAGE_BUFFER}
        self.assertTrue(buffers, "set 0 binding 0 target is not a storage buffer variable")
        pointers = set()
        for opcode, args in instructions:
            if opcode == OP_ACCESS_CHAIN and len(args) >= 3 and args[2] in buffers | pointers:
                pointers.add(args[1])
        self.assertTrue(pointers, "SSBO never accessed: runtime side effects were lost")
        self.assertTrue(any(opcode == OP_LOAD and args[2] in pointers
                            for opcode, args in instructions),
                        "no storage input load survived emission")
        self.assertTrue(any(opcode == OP_STORE and args[0] in pointers
                            for opcode, args in instructions),
                        "no storage output store survived emission")

    def test_help_lists_contract_flags(self):
        result = self.run_bench_raw("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        for flag in ("--emit-spirv", "--emit-only", "--list-devices", "--mode"):
            self.assertIn(flag, result.stdout)

    def test_numeric_bounds_are_bad_cli(self):
        for case in BAD_NUMERICS:
            with self.subTest(flag=" ".join(case)):
                self.run_bench("--emit-only", "--emit-spirv", self.output, *case, code=2)
                self.assertFalse(self.output.exists())

    def test_missing_values_and_unknown_options(self):
        for option in ("--operations", "--emit-spirv"):
            with self.subTest(missing=option):
                self.run_bench("--emit-only", option, code=2)
        for case in (("--frobnicate",), ("--mode", "serial"), ("--pattern", "complex")):
            with self.subTest(flag=" ".join(case)):
                self.run_bench(*case, "--emit-only", "--emit-spirv", self.output, code=2)
                self.assertFalse(self.output.exists())

    def test_emit_only_requires_path(self):
        self.run_bench("--emit-only", code=2)
        self.assertFalse(self.output.exists())

    def test_unwritable_emit_path_fails(self):
        for path in (self.root / "missing-dir" / "tensor.spv", self.root / "as-directory"):
            with self.subTest(path=str(path)):
                if path.name == "as-directory":
                    path.mkdir()
                self.run_bench("--emit-only", "--emit-spirv", path, code=1)
                self.assertFalse(path.is_file())

    def test_software_emit_depths_and_local_sizes(self):
        for depth in (8, 16):
            for local_size in (32, 64):
                with self.subTest(depth=depth, local_size=local_size):
                    path = self.root / f"software-k{depth}-local{local_size}.spv"
                    self.run_bench("--emit-only", "--mode", "software", "--depth", depth,
                                   "--local-size", local_size, "--pattern", "fractions",
                                   "--seed", 17, "--workgroups", 4, "--iterations", 3,
                                   "--compile-repeats", 2, "--emit-spirv", path)
                    self.assert_tensor_module(path)
                    self.validate(path)

    def test_software_emit_operation_counts(self):
        for operations in (1, 3):
            with self.subTest(operations=operations):
                path = self.root / f"software-ops{operations}.spv"
                self.run_bench("--emit-only", "--mode", "software", "--depth", 16,
                               "--operations", operations, "--emit-spirv", path)
                self.assert_tensor_module(path)
                self.validate(path)

    def test_cooperative_emit_is_structurally_valid(self):
        path = self.root / "cooperative-k16.spv"
        self.run_bench("--emit-only", "--mode", "cooperative", "--depth", 16,
                       "--emit-spirv", path)
        self.assert_tensor_module(path)
        instructions = list(spirv_instructions(path.read_bytes()))
        self.assertTrue(any(opcode == 17 and args == (6022,)
                            for opcode, args in instructions), "no cooperative-matrix capability")
        self.assertTrue(any(opcode == 4459 for opcode, _ in instructions),
                        "cooperative mode silently emitted the software fallback")
        self.validate(path)

    def test_inline_values_and_boolean_flags(self):
        result = self.run_bench("--emit-only", "--depth=8", "--operations=3",
                                f"--emit-spirv={self.output}")
        self.assertEqual(result["case"]["depth"], 8)
        self.assertEqual(result["case"]["operations"], 3)
        self.assert_tensor_module(self.output)
        self.validate(self.output)
        for flag in ("--emit-only=true", "--validation=true", "--list-devices=true"):
            with self.subTest(flag=flag):
                self.run_bench(flag, code=2)

    def test_emit_only_without_icd(self):
        absent = self.root / "absent-gpu.icd"
        env = dict(os.environ)
        env["VK_ICD_FILENAMES"] = str(absent)
        env["VK_DRIVER_FILES"] = str(absent)
        self.run_bench("--emit-only", "--depth", 8, "--emit-spirv", self.output, env=env)
        self.assert_tensor_module(self.output)
        self.validate(self.output)


if __name__ == "__main__":
    unittest.main()

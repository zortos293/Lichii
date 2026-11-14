#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Game-free producer/consumer contract tests for captured compute compilation.

Consumer checks use a missing Vulkan ICD: invalid inputs must fail before Vulkan,
while valid metadata must reach the loader. GPU compilation is verified separately.
"""

import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

SWEEP = Path(sys.argv.pop(1)).resolve()
BENCH = Path(sys.argv.pop(1)).resolve()

MAXWELL_EXIT = 0xE30000000007000F
COMPUTE_STAGE = 5
CACHE_VERSION = 54
PROFILE = "offline-spirv-1.4"
# Production compute push-constant range, derived from emit_spirv.h's layouts.
PUSH_CONSTANT_BYTES = 44
CONTEXT_KEYS = ("schema_version", "profile", "workgroup_size", "shared_memory_bytes",
                "uses_warp_ops", "uses_cooperative_matrix", "bindings", "push_constant_bytes",
                "spirv_hash")

SPIRV_MAGIC = 0x07230203
SPIRV_1_4 = 0x00010400
OP_ENTRY_POINT = 15
OP_EXECUTION_MODE = 16
OP_DECORATE = 71
GL_COMPUTE = 5
LOCAL_SIZE = 17
DESCRIPTOR_SET = 34


def compute_record(unique_hash=1, workgroup=(32, 1, 1), shared_memory=0):
    """A minimal compute pipeline record: environment count, one environment (six u64
    counts, seven u32 fields, 80-byte header + one EXIT, workgroup/shared tail) and
    the 24-byte compute key, as the v54 cache stores it."""
    code = bytes(80) + struct.pack("<Q", MAXWELL_EXIT)
    env = struct.pack("<6Q", len(code), 0, 0, 0, 0, 0)
    env += struct.pack("<7I", 0, 2, 80, 0, len(code) - 8, 1, COMPUTE_STAGE) + code
    env += struct.pack("<4I", *workgroup, shared_memory)
    key = struct.pack("<QI3I", unique_hash, shared_memory, *workgroup)
    return struct.pack("<I", 1) + env + key


def spirv_instructions(module):
    """Yield (opcode, args) pairs, requiring exact instruction boundaries."""
    words = struct.unpack(f"<{len(module) // 4}I", module)
    offset = 5
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 0xFFFF
        if not size or offset + size > len(words):
            raise AssertionError(f"instruction at word {offset} overruns the module")
        yield opcode, words[offset + 1:offset + size]
        offset += size


class CompilePipelineCase(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.modules = self.root / "modules"
        self.absent_icd = self.root / "absent-gpu.icd"
        self.icdless = dict(os.environ, VK_ICD_FILENAMES=str(self.absent_icd),
                            VK_DRIVER_FILES=str(self.absent_icd))
        self.runs = 0

    def produce(self, record=None):
        """Lower one record through shader-sweep --pipeline; returns the subprocess."""
        path = self.root / f"record-{self.runs}.bin"
        self.runs += 1
        path.write_bytes(compute_record() if record is None else record)
        self.modules = self.root / f"modules-{self.runs}"
        return subprocess.run(
            [str(SWEEP), "--pipeline", str(path), "--cache-version", str(CACHE_VERSION),
             "--output-dir", str(self.modules)],
            capture_output=True, text=True, timeout=120)

    def pipeline(self):
        """A successful lowering: emitted module with replay.json saved beside it."""
        result = self.produce()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        doc = json.loads(result.stdout)
        self.assertEqual(doc["status"], "ok", doc)
        metadata = self.modules / "replay.json"
        metadata.write_text(json.dumps(doc, indent=1))
        return doc, metadata

    def module_path(self, doc):
        return self.modules / doc["stages"][0]["spirv"]

    def staged(self, doc, changes=None, module=None):
        """The module and a possibly mutated replay.json in a fresh directory."""
        self.runs += 1
        target = self.root / f"case-{self.runs:02d}"
        target.mkdir()
        name = doc["stages"][0]["spirv"]
        (target / name).write_bytes(
            self.module_path(doc).read_bytes() if module is None else module)
        mutated = json.loads(json.dumps(doc))
        if changes:
            changes(mutated)
        path = target / "replay.json"
        path.write_text(json.dumps(mutated, indent=1))
        return path

    def diagnostic(self, result):
        try:
            return json.loads(result.stdout)
        except ValueError:
            self.fail(f"no JSON diagnostic: {result.stdout!r} / {result.stderr!r}")

    def consume(self, metadata, *extra):
        return subprocess.run([str(BENCH), "--compile-pipeline", str(metadata), *map(str, extra)],
                              capture_output=True, text=True, timeout=60, env=self.icdless)

    def reject(self, metadata, *extra):
        """Invalid input must fail in the usage class before any Vulkan work."""
        result = self.consume(metadata, *extra)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        doc = self.diagnostic(result)
        self.assertEqual(doc["schema_version"], 1, doc)
        self.assertEqual(doc["status"], "error", doc)
        self.assertTrue(doc["error"]["message"], doc)
        return doc


class ProducerContextTests(CompilePipelineCase):
    def test_successful_lowering_emits_the_frozen_context(self):
        doc, metadata = self.pipeline()
        module = self.module_path(doc).read_bytes()
        self.assertEqual(metadata.parent, self.module_path(doc).parent)
        self.assertEqual(doc["schema_version"], 1)
        self.assertEqual(doc["kind"], "compute")
        stage, = doc["stages"]
        self.assertEqual(stage["stage"], "compute")
        self.assertTrue(stage["spirv"])
        self.assertEqual(stage["spirv_words"], len(module) // 4)
        self.assertGreater(stage["spirv_words"], 0)
        context = doc["compile_context"]
        self.assertEqual(context["schema_version"], 1)
        self.assertEqual(context["profile"], PROFILE)
        self.assertEqual(context["workgroup_size"], [32, 1, 1])
        self.assertEqual(context["shared_memory_bytes"], 0)
        self.assertIs(context["uses_warp_ops"], False)
        self.assertIs(context["uses_cooperative_matrix"], False)
        self.assertEqual(context["bindings"], [])
        self.assertEqual(context["push_constant_bytes"], PUSH_CONSTANT_BYTES)
        self.assertTrue(re.fullmatch(r"[0-9a-f]{16}", context["spirv_hash"]), context)
        self.assertEqual(struct.unpack_from("<I", module)[0], SPIRV_MAGIC)
        self.assertEqual(struct.unpack_from("<I", module, 4)[0], SPIRV_1_4)
        instructions = list(spirv_instructions(module))
        self.assertTrue(any(opcode == OP_ENTRY_POINT and args and args[0] == GL_COMPUTE
                            for opcode, args in instructions), "no GLCompute entry point")
        self.assertIn((LOCAL_SIZE, (32, 1, 1)),
                      [(args[1], args[2:5]) for opcode, args in instructions
                       if opcode == OP_EXECUTION_MODE and len(args) >= 5],
                      "module LocalSize disagrees with the context")
        self.assertFalse(any(opcode == OP_DECORATE and len(args) > 1 and args[1] == DESCRIPTOR_SET
                             for opcode, args in instructions),
                         "descriptor-free context but decorated bindings in the module")

    def test_repeated_lowering_is_byte_deterministic(self):
        first, _ = self.pipeline()
        first_bytes = self.module_path(first).read_bytes()
        second, _ = self.pipeline()
        self.assertEqual(first_bytes, self.module_path(second).read_bytes())
        self.assertEqual(first["compile_context"], second["compile_context"])
        self.assertEqual(first["stages"][0]["spirv_words"], second["stages"][0]["spirv_words"])

    def test_failed_or_malformed_records_emit_no_context(self):
        disagreeing = bytearray(compute_record())
        struct.pack_into("<3I", disagreeing, len(disagreeing) - 12, 8, 1, 1)
        for name, record in (("truncated", compute_record()[:-4]),
                             ("key-tail-mismatch", bytes(disagreeing))):
            with self.subTest(case=name):
                result = self.produce(record)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                doc = self.diagnostic(result)
                self.assertEqual(doc["status"], "error", doc)
                self.assertTrue(doc["error"]["message"], doc)
                self.assertNotIn("compile_context", doc)
                self.assertFalse(list(self.modules.glob("*")), "a module was still written")


class CompileOnlyConsumerTests(CompilePipelineCase):
    def test_valid_metadata_parses_then_the_loader_fails(self):
        _, metadata = self.pipeline()
        result = self.consume(metadata)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.returncode, 1, "valid input failed in the usage class instead "
                         "of the runtime one: " + result.stdout + result.stderr)
        diagnostic = self.diagnostic(result)
        self.assertEqual(diagnostic["pipeline_lifetime"], "destroy_before_recreate")
        message = diagnostic["error"]["message"]
        self.assertTrue(message)
        lowered = message.lower()
        self.assertTrue(any(word in lowered for word in ("vulkan", "instance", "loader", "icd")),
                        f"failure does not read as a Vulkan loader error: {message!r}")

    def test_allowed_options_stay_in_the_runtime_class(self):
        _, metadata = self.pipeline()
        for extra in (("--device", "0"), ("--compile-repeats", "2"), ("--validation",)):
            with self.subTest(flag=" ".join(extra)):
                self.assertEqual(self.consume(metadata, *extra).returncode, 1)

    def test_incompatible_explicit_options_are_usage_rejections(self):
        _, metadata = self.pipeline()
        for extra in (("--emit-only",), ("--emit-spirv", str(self.root / "unused.spv")),
                      ("--list-devices",), ("--operations", "8"), ("--mode", "software")):
            with self.subTest(flag=" ".join(extra)):
                self.reject(metadata, *extra)

    def test_unreadable_metadata_files_are_rejected_before_vulkan(self):
        empty = self.root / "empty.json"
        empty.write_text("")
        broken = self.root / "broken.json"
        broken.write_text('{"schema_version": 1, ')
        for name, metadata in (("missing", self.root / "absent.json"), ("directory", self.root),
                               ("empty", empty), ("broken-json", broken)):
            with self.subTest(case=name):
                self.reject(metadata)

    def test_json_document_must_be_bounded_and_complete(self):
        _, metadata = self.pipeline()
        metadata.write_text(metadata.read_text() + " trailing garbage")
        self.reject(metadata)
        metadata.write_bytes(b" " * (4 * 1024 * 1024 + 1))
        self.reject(metadata)

    def test_failed_and_noncompute_diagnostics_are_rejected(self):
        doc, _ = self.pipeline()

        def noncompute(d):
            d.update(kind="graphics")
            d["stages"][0]["stage"] = "vertex"

        for name, changes in (
            ("failed-status", lambda d: d.update(status="error", error={"message": "boom"})),
            ("noncompute", noncompute),
            ("no-context", lambda d: d.pop("compile_context", None)),
            ("context-schema", lambda d: d["compile_context"].update(schema_version=2)),
            ("context-profile", lambda d: d["compile_context"].update(profile="offline-spirv-1.3")),
            ("no-stages", lambda d: d.update(stages=[])),
            ("absent-module", lambda d: d["stages"][0].update(spirv="ghost.spv")),
        ):
            with self.subTest(case=name):
                self.reject(self.staged(doc, changes))

    def test_missing_context_fields_are_rejected(self):
        doc, _ = self.pipeline()
        for key in CONTEXT_KEYS:
            with self.subTest(key=key):
                self.reject(self.staged(doc, lambda d, key=key: d["compile_context"].pop(key)))

    def test_out_of_bounds_context_is_rejected(self):
        doc, _ = self.pipeline()
        for name, changes in (
            ("workgroup-zero", lambda d: d["compile_context"].update(workgroup_size=[32, 1, 0])),
            ("workgroup-arity", lambda d: d["compile_context"].update(workgroup_size=[32, 1])),
            ("workgroup-mismatch", lambda d: d["compile_context"].update(
                workgroup_size=[16, 2, 1])),
            ("cooperative-profile", lambda d: d["compile_context"].update(
                uses_cooperative_matrix=True)),
            ("negative-shared", lambda d: d["compile_context"].update(shared_memory_bytes=-1)),
            ("push-constant-zero", lambda d: d["compile_context"].update(push_constant_bytes=0)),
            ("word-count", lambda d: d["stages"][0].update(
                spirv_words=d["stages"][0]["spirv_words"] + 1)),
            ("hash-value", lambda d: d["compile_context"].update(spirv_hash="0123456789abcdef")),
            ("hash-shape", lambda d: d["compile_context"].update(spirv_hash="DEADBEEF")),
        ):
            with self.subTest(case=name):
                self.reject(self.staged(doc, changes))

    def test_damaged_modules_are_rejected(self):
        doc, _ = self.pipeline()
        original = self.module_path(doc).read_bytes()
        words = list(struct.unpack(f"<{len(original) // 4}I", original))
        overrun = list(words)
        overrun[5] = (overrun[5] & 0xFFFF) | (0xFFFF << 16)
        stale = bytearray(original)
        stale[8] ^= 1  # Change the generator word without damaging instruction boundaries.
        for name, module in (
            ("bad-magic", bytes(4) + original[4:]),
            ("unaligned", original[:-2]),
            ("empty", b""),
            ("instruction-overrun", struct.pack(f"<{len(overrun)}I", *overrun)),
            ("stale-hash", bytes(stale)),
        ):
            with self.subTest(case=name):
                self.reject(self.staged(doc, module=module))


class PipelineCacheTests(CompilePipelineCase):
    """--pipeline-cache contract: opt-in only with --compile-pipeline, raw blob
    validation before Vulkan, and application_cache fields reported before any
    Vulkan call."""

    def plain(self, *args):
        """Run the bench without --compile-pipeline, under the absent-ICD environment."""
        return subprocess.run([str(BENCH), *map(str, args)],
                              capture_output=True, text=True, timeout=60, env=self.icdless)

    def test_pipeline_cache_requires_compile_pipeline_and_a_value(self):
        _, metadata = self.pipeline()
        cache = self.root / "unused.cache"
        help_result = self.plain("--help")
        self.assertEqual(help_result.returncode, 0, help_result.stderr)
        self.assertIn("--pipeline-cache", help_result.stdout)
        for name, result in (
            ("missing-value", self.plain("--compile-pipeline", metadata, "--pipeline-cache")),
            ("without-compile-pipeline", self.plain("--pipeline-cache", cache)),
            ("with-list-devices", self.plain("--list-devices", "--pipeline-cache", cache)),
            ("with-synthetic-mode", self.plain("--mode", "software", "--pipeline-cache", cache)),
            ("with-emit-mode", self.plain("--emit-spirv", self.root / "unused.spv",
                                          "--pipeline-cache", cache)),
        ):
            with self.subTest(case=name):
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        with self.subTest(case="empty-value"):
            self.reject(metadata, "--pipeline-cache", "")
        self.assertFalse(cache.exists(), "a rejected run still created the cache file")

    def test_application_cache_reports_a_disabled_default(self):
        _, metadata = self.pipeline()
        result = self.consume(metadata)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        cache = self.diagnostic(result)["application_cache"]
        self.assertIs(cache["enabled"], False)
        self.assertIsNone(cache["path"])
        self.assertEqual(cache["initial_bytes"], 0)
        self.assertEqual(cache["saved_bytes"], 0)

    def test_absent_cache_path_reaches_the_loader_without_writing(self):
        _, metadata = self.pipeline()
        cache = self.root / "fresh.cache"
        result = self.consume(metadata, "--pipeline-cache", cache)
        self.assertEqual(result.returncode, 1, "an absent cache file must be valid, so the run "
                         "belongs in the runtime class: " + result.stdout + result.stderr)
        application = self.diagnostic(result)["application_cache"]
        self.assertIs(application["enabled"], True)
        self.assertEqual(application["path"], str(cache))
        self.assertEqual(application["initial_bytes"], 0)
        self.assertEqual(application["saved_bytes"], 0)
        self.assertFalse(cache.exists(), "the failed run created the cache file")
        self.assertFalse(cache.with_name(cache.name + ".tmp").exists(),
                         "the failed run created the cache temp file")

    def test_malformed_cache_files_are_rejected_before_vulkan(self):
        doc, metadata = self.pipeline()
        module = self.module_path(doc).read_bytes()
        metadata_bytes = metadata.read_bytes()
        header = struct.pack("<IIII", 32, 1, 0, 0) + bytes(16)
        oversize = self.root / "oversized.cache"
        oversize.write_bytes(header)
        with oversize.open("ab") as handle:
            handle.truncate(64 * 1024 * 1024 + 1)
        cases = (("empty", b""), ("short-header", header[:-1]),
                 ("bad-header-size", struct.pack("<IIII", 48, 1, 0, 0) + bytes(16)),
                 ("bad-header-version", struct.pack("<IIII", 32, 2, 0, 0) + bytes(16)))
        for name, blob in cases:
            path = self.root / f"{name}.cache"
            path.write_bytes(blob)
            with self.subTest(case=name):
                self.reject(metadata, "--pipeline-cache", path)
                self.assertEqual(path.read_bytes(), blob, "the cache input was modified")
        with self.subTest(case="oversize"):
            self.assertEqual(oversize.stat().st_size, 64 * 1024 * 1024 + 1)
            self.reject(metadata, "--pipeline-cache", oversize)
            self.assertEqual(oversize.stat().st_size, 64 * 1024 * 1024 + 1,
                             "the oversized cache input was modified")
        self.assertEqual(metadata.read_bytes(), metadata_bytes, "the input JSON was modified")
        self.assertEqual(self.module_path(doc).read_bytes(), module, "the module was modified")

    def test_cache_paths_may_not_alias_the_inputs(self):
        doc, _ = self.pipeline()

        # The cache file itself may not be an input file.
        direct = self.staged(doc)
        module = direct.parent / doc["stages"][0]["spirv"]
        for name, target in (("cache-is-metadata", direct), ("cache-is-module", module)):
            with self.subTest(case=name):
                before = (direct.read_bytes(), module.read_bytes())
                self.reject(direct, "--pipeline-cache", target)
                self.assertEqual((direct.read_bytes(), module.read_bytes()), before,
                                 "an input file was modified")

        # Nor may the cache's ".tmp" save sibling collide with an input file.
        renamed = self.staged(doc)
        temp = renamed.with_name(renamed.name + ".tmp")
        renamed.rename(temp)
        with self.subTest(case="cache-temp-is-metadata"):
            before = temp.read_bytes()
            self.reject(temp, "--pipeline-cache", renamed)
            self.assertEqual(temp.read_bytes(), before, "the input JSON was modified")

        module_case = self.staged(doc)
        original = module_case.parent / doc["stages"][0]["spirv"]
        renamed_module = module_case.parent / "blob.tmp"
        original.rename(renamed_module)
        mutated = json.loads(module_case.read_text())
        mutated["stages"][0]["spirv"] = "blob.tmp"
        module_case.write_text(json.dumps(mutated, indent=1))
        with self.subTest(case="cache-temp-is-module"):
            before = (renamed_module.read_bytes(), module_case.read_bytes())
            self.reject(module_case, "--pipeline-cache", module_case.parent / "blob")
            self.assertEqual((renamed_module.read_bytes(), module_case.read_bytes()), before,
                             "an input file was modified")


if __name__ == "__main__":
    unittest.main()

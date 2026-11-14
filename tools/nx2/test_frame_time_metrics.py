#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from frame_time_metrics import read_intervals, summarize


class FrameMetricsTests(unittest.TestCase):
    def test_throughput_uses_total_time_not_average_instantaneous_fps(self):
        result = summarize([10, 30])
        self.assertEqual(result['average_fps'], 50)
        self.assertEqual(result['median_ms'], 20)
        self.assertEqual(result['p95_ms'], 29)
        self.assertEqual(result['p99_ms'], 29.8)

    def test_thresholds_are_strict_and_budget_is_explicit(self):
        result = summarize([1000/60, 33.3, 50, 100, 100.01])
        self.assertEqual(result['within_60fps_budget_share'], .2)
        self.assertEqual(result['over_33_3_ms'], 3)
        self.assertEqual(result['over_50_ms'], 2)
        self.assertEqual(result['over_100_ms'], 1)

    def test_invalid_or_empty_windows_fail(self):
        for values in ([], [0], [-1], [True], ['16.7'], [float('nan')], [float('inf')]):
            with self.subTest(values=values), self.assertRaises(ValueError):
                summarize(values)

    def test_recent_only_json_cannot_impersonate_complete_window(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.json'
            path.write_text(json.dumps({'frames': 600, 'intervals_ms': [16.7]*120}))
            with self.assertRaises(ValueError):
                read_intervals(path)

    def test_truncated_json_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.json'
            path.write_text(json.dumps({'truncated': True, 'intervals_ms': [16.7]}))
            with self.assertRaises(ValueError):
                read_intervals(path)

    def test_input_contaminated_window_cannot_enter_comparison(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.json'
            path.write_text(json.dumps({'valid_for_comparison': False,
                                        'frames': 2, 'intervals_ms': [10, 20]}))
            with self.assertRaisesRegex(ValueError, 'excluded'):
                read_intervals(path)

    def test_missing_or_conflicting_counts_fail(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.json'
            for metadata in ({}, {'frames': 2, 'summary': {'intervals': 600}}):
                with self.subTest(metadata=metadata):
                    path.write_text(json.dumps({**metadata, 'intervals_ms': [10, 20]}))
                    with self.assertRaises(ValueError):
                        read_intervals(path)
            path.write_text(json.dumps({'frames': True, 'intervals_ms': [10]}))
            with self.assertRaises(ValueError):
                read_intervals(path)

    def test_tsv_bounds_exclude_menus_and_require_complete_frames(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.tsv'
            path.write_text('# recorder\nframe\tinterval_ms\n1\t1000\n2\t10\n3\t20\n4\t1000\n')
            self.assertEqual(read_intervals(path, 2, 4), [10, 20])
            with self.assertRaises(ValueError):
                read_intervals(path, 2, 6)

    def test_tsv_gaps_fail(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'frames.tsv'
            path.write_text('frame\tinterval_ms\n2\t10\n4\t20\n')
            with self.assertRaises(ValueError):
                read_intervals(path)

    def test_native_csv_nanoseconds_and_bounds(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'presents.csv'
            path.write_text('frame,return_ns,interval_ns\n1,100,0\n2,10000100,10000000\n3,30000100,20000000\n')
            self.assertEqual(read_intervals(path, 2, 4), [10, 20])
            with self.assertRaises(ValueError):
                summarize(read_intervals(path))

    def test_audited_native_csv_rejects_manual_interference(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'presents.csv'
            path.write_text('frame,interval_ns,input_interference_count,input_audit_enabled\n'
                            '1,0,0,1\n2,10000000,0,1\n3,20000000,1,1\n')
            with self.assertRaisesRegex(ValueError, 'excluded'):
                read_intervals(path, 2, 4)

    def test_audited_native_csv_keeps_quiet_stalls_and_requires_boundary(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'presents.csv'
            header = 'frame,interval_ns,input_interference_count,input_audit_enabled\n'
            path.write_text(header + '1,0,7,1\n2,10000000,7,1\n3,200000000,7,1\n')
            self.assertEqual(read_intervals(path, 2, 4), [10, 200])
            path.write_text(header + '2,10000000,7,1\n3,200000000,7,1\n')
            with self.assertRaisesRegex(ValueError, 'complete'):
                read_intervals(path, 2, 4)


class FrameMetricsOutputTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.folder = Path(folder.name)
        self.source = self.folder / 'frames.json'
        self.original = b'{"frames": 2, "intervals_ms": [10, 30]}\n'
        self.source.write_bytes(self.original)

    def run_cli(self, output, source=None):
        script = Path(__file__).with_name('frame_time_metrics.py').resolve()
        return subprocess.run([sys.executable, '-B', str(script),
                               str(self.source if source is None else source),
                               '--domain', 'test present', '--output', str(output)],
                              cwd=self.folder, capture_output=True, text=True)

    def assert_input_preserved(self, output, source=None):
        result = self.run_cli(output, source)
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('output must not refer to the input file', result.stderr)

    def test_output_rejects_same_path(self):
        self.assert_input_preserved(self.source)

    def test_output_rejects_relative_absolute_alias(self):
        self.assert_input_preserved(self.source, Path('frames.json'))

    @unittest.skipUnless(os.name == 'nt', 'Windows filename case alias')
    def test_output_rejects_windows_case_alias(self):
        alias = self.source.with_name('FRAMES.JSON')
        if not alias.exists() or not alias.samefile(self.source):
            self.skipTest('temporary directory has case-sensitive filenames')
        self.assert_input_preserved(alias)

    def test_output_rejects_hardlink_alias(self):
        alias = self.folder / 'linked.json'
        alias.hardlink_to(self.source)
        self.assert_input_preserved(alias)
        self.assertEqual(alias.read_bytes(), self.original)

    def test_distinct_output_preserves_input(self):
        output = self.folder / 'report.json'
        result = self.run_cli(output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.source.read_bytes(), self.original)
        report = json.loads(output.read_text(encoding='utf-8'))
        self.assertEqual(report['average_fps'], 50)
        self.assertEqual(json.loads(result.stdout), report)


if __name__ == '__main__':
    unittest.main()

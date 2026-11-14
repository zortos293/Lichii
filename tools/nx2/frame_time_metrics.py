#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarize positive frame intervals, keeping measurement domains explicit."""
import argparse
import csv
import json
import math
from pathlib import Path
from native_input_audit import validate_input_window


def summarize(intervals):
    values = list(intervals)
    if not values or any(isinstance(v, bool) or not isinstance(v, (int, float))
                         or not math.isfinite(v) or v <= 0 for v in values):
        raise ValueError('frame intervals must be finite, positive and nonempty')
    ordered = sorted(values)

    def percentile(q):
        position = (len(ordered) - 1) * q
        low = int(position)
        high = min(low + 1, len(ordered) - 1)
        return ordered[low] + (ordered[high] - ordered[low]) * (position - low)

    elapsed = math.fsum(values)
    return {
        'interval_count': len(values),
        'elapsed_seconds': elapsed / 1000,
        'average_fps': len(values) * 1000 / elapsed,
        'median_ms': percentile(.5),
        'p95_ms': percentile(.95),
        'p99_ms': percentile(.99),
        'maximum_ms': max(values),
        'within_60fps_budget_share': sum(v <= 1000 / 60 for v in values) / len(values),
        'over_33_3_ms': sum(v > 33.3 for v in values),
        'over_50_ms': sum(v > 50 for v in values),
        'over_100_ms': sum(v > 100 for v in values),
    }


def read_intervals(path, first_frame=None, end_frame=None):
    if path.suffix.lower() == '.json':
        if first_frame is not None or end_frame is not None:
            raise ValueError('frame bounds require a TSV with explicit frame numbers')
        data = json.loads(path.read_text(encoding='utf-8-sig'))
        if data.get('valid_for_comparison') is False:
            raise ValueError('timing window was excluded from comparisons by its recorder')
        if data.get('truncated') or data.get('frame_times', {}).get('truncated'):
            raise ValueError('refusing a truncated timing window')
        values = data.get('intervals_ms')
        if values is None:
            raise ValueError('JSON must contain complete intervals_ms, not percentile summaries')
        counts = [data[key] for key in ('frames', 'interval_count') if key in data]
        if 'intervals' in data.get('summary', {}):
            counts.append(data['summary']['intervals'])
        if not counts:
            raise ValueError('JSON requires an explicit interval count to verify completeness')
        if any(isinstance(count, bool) or not isinstance(count, int) or count != len(values)
               for count in counts):
            raise ValueError('interval count differs from the declared window; recent-only arrays are insufficient')
        return values
    native_csv = path.suffix.lower() == '.csv'
    interval_column = 'interval_ns' if native_csv else 'interval_ms'
    divisor = 1_000_000 if native_csv else 1
    with path.open(encoding='utf-8-sig') as stream:
        rows = list(csv.DictReader((line for line in stream if not line.startswith('#')),
                                  delimiter=',' if native_csv else '\t'))
        selected = [(int(row['frame']), float(row[interval_column]) / divisor) for row in rows
                    if (first_frame is None or int(row['frame']) >= first_frame)
                    and (end_frame is None or int(row['frame']) < end_frame)]
    if not selected:
        raise ValueError('no frames in requested interval')
    numbers = [frame for frame, _ in selected]
    if any(b != a + 1 for a, b in zip(numbers, numbers[1:])):
        raise ValueError('frame sequence contains gaps or duplicates')
    if first_frame is not None and numbers[0] != first_frame:
        raise ValueError('requested first frame is missing')
    if end_frame is not None and numbers[-1] != end_frame - 1:
        raise ValueError('requested final frame is missing')
    if native_csv and any(key in rows[0] for key in ('input_interference_count', 'input_audit_enabled')):
        audit = validate_input_window(rows, numbers[0], numbers[-1] + 1)
        if not audit['valid_for_comparison']:
            raise ValueError('native timing window was excluded because input interference was recorded')
    return [value for _, value in selected]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('--domain', required=True, help='e.g. guest present, host present, or physical scanout')
    parser.add_argument('--first-frame', type=int)
    parser.add_argument('--end-frame', type=int, help='exclusive')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists() and args.output.samefile(args.input):
        parser.error('output must not refer to the input file')
    result = {'measurement_domain': args.domain, 'source': str(args.input),
              'first_frame': args.first_frame, 'end_frame_exclusive': args.end_frame,
              **summarize(read_intervals(args.input, args.first_frame, args.end_frame))}
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.write_text(output, encoding='utf-8')
    print(output, end='')


if __name__ == '__main__':
    main()

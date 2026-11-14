# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate manual-input counters sampled beside native present-return timestamps."""


def validate_input_window(rows, first_frame, end_frame):
    if first_frame < 1 or end_frame <= first_frame:
        raise ValueError('input audit requires a positive, nonempty frame window')
    try:
        selected = [row for row in rows if first_frame - 1 <= int(row['frame']) < end_frame]
        numbers = [int(row['frame']) for row in selected]
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError('input audit has invalid frame columns') from error
    if numbers != list(range(first_frame - 1, end_frame)):
        raise ValueError('input audit requires complete frames plus the previous boundary')
    try:
        counts = [int(row['input_interference_count']) for row in selected]
        enabled = [int(row['input_audit_enabled']) for row in selected]
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError('input audit columns are missing or invalid') from error
    if any(value != 1 for value in enabled):
        raise ValueError('input audit was not enabled over the complete window')
    if counts[0] < 0 or any(b < a for a, b in zip(counts, counts[1:])):
        raise ValueError('input interference counter moved backward or was negative')
    delta = counts[-1] - counts[0]
    return dict(first_boundary_frame=first_frame - 1, final_boundary_frame=end_frame - 1,
                count_before=counts[0], count_after=counts[-1], interference_delta=delta,
                valid_for_comparison=delta == 0,
                method='Counter snapshots adjacent to native present-return timestamps; any manual input, renderer menu, or unsupported direct mouse mode excludes the window')

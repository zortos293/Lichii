#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import hashlib
import json
import subprocess
import sys
import tempfile
from collections.abc import Iterator, Sequence
from html import escape
from pathlib import Path

import progress
from progress import PAGE_STYLE, svg_open

WIDTH, LEFT, RIGHT, TITLE, PANEL, GAP = 900, 56, 190, 34, 150, 16
MAX_POINTS = 60
BACKGROUND, GRID, TEXT, MUTED = "#0d1117", "#30363d", "#c9d1d9", "#8b949e"
SERIES = (
    ("services", "Bound", "#2f81f7"),
    ("shaders", "Shaders", "#2ea043"),
)


def git(*args: str) -> str:
    try:
        return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout
    except subprocess.CalledProcessError as error:
        raise ValueError(f"git {' '.join(args)} failed: {error.stderr.strip()}") from error


def signature() -> str:
    scanner = Path(progress.__file__).read_bytes()
    return hashlib.sha256(scanner + Path(__file__).read_bytes()).hexdigest()


def load_cache(path: Path, rules: str) -> dict[str, dict[str, float]]:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict) or data.get("rules") != rules or not isinstance(data.get("commits"), dict):
        return {}
    return {sha: value for sha, value in data["commits"].items() if isinstance(value, dict)}


def save_cache(path: Path, rules: str, commits: dict[str, dict[str, float]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {"rules": rules, "commits": commits}
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def measure(root: Path, sha: str) -> dict[str, float] | None:
    paths = (progress.SERVICES.as_posix(), progress.OPCODES.as_posix(), progress.SPIRV.as_posix())
    archive = subprocess.run(["git", "-C", str(root), "archive", sha, *paths], capture_output=True, check=False,
                              text=False)
    if archive.returncode != 0:
        return None
    with tempfile.TemporaryDirectory() as directory:
        tree = Path(directory)
        try:
            subprocess.run(["tar", "-x", "-C", str(tree)], input=archive.stdout, check=True, capture_output=True)
            services = progress.scan_services(tree).metric
            shaders = progress.scan_shaders(tree).metric
        except (OSError, ValueError, subprocess.CalledProcessError):
            return None
    return {"services": services.percent, "shaders": shaders.percent}


def snapshots(root: Path, limit: int, cache: Path | None) -> list[dict[str, object]]:
    rules = signature()
    commits = load_cache(cache, rules) if cache else {}
    shas = git("-C", str(root), "log", "--first-parent", "--format=%H", f"-{limit}").split()
    points: list[dict[str, object]] = []
    for sha in reversed(shas):
        counts = commits.get(sha) if sha in commits else measure(root, sha)
        if counts is None:
            continue
        commits[sha] = counts
        points.append({"sha": sha[:7], **counts})
    if cache:
        save_cache(cache, rules, commits)
    return points


def axis(points: list[dict[str, object]], x: float, y: float, height: float) -> list[str]:
    parts = []
    for step in range(5):
        value = step * 25
        line_y = y + height - height * value / 100
        parts.append(f'<line x1="{x}" y1="{line_y}" x2="{x + WIDTH - LEFT - RIGHT}" y2="{line_y}" stroke="{GRID}"/>')
        parts.append(f'<text x="{x - 8}" y="{line_y + 4}" fill="{MUTED}" font-size="10" text-anchor="end">{value}%</text>')
    return parts


def series(points: list[dict[str, object]], key: str, color: str, x: float, y: float, width: float,
           height: float) -> list[str]:
    if len(points) < 2:
        return []
    step = width / (len(points) - 1)
    coordinates = " ".join(f"{x + index * step:.1f},{y + height - height * float(point[key]) / 100:.1f}"
                           for index, point in enumerate(points))
    return [f'<polyline points="{coordinates}" fill="none" stroke="{color}" stroke-width="2"/>']


def legend(x: float, y: float) -> list[str]:
    parts = []
    for index, (_, label, color) in enumerate(SERIES):
        offset = x + index * 110
        parts.append(f'<rect x="{offset}" y="{y}" width="10" height="10" fill="{color}"/>')
        parts.append(f'<text x="{offset + 16}" y="{y + 9}" fill="{TEXT}" font-size="11">{escape(label)}</text>')
    return parts


def render(points: list[dict[str, object]]) -> str:
    height = TITLE + PANEL + GAP
    width = WIDTH
    plot_width, plot_height = width - LEFT - RIGHT, PANEL
    caption = f"{len(points)} most recent first-parent commits on {progress.BRANCH}"
    parts = ['<title>Lichii progress history</title>',
             f'<text x="0" y="18" fill="{TEXT}" font-size="15">{escape(caption)}</text>',
             *legend(LEFT, TITLE - 6),
             *axis(points, LEFT, TITLE, plot_height)]
    for key, _, color in SERIES:
        parts += series(points, key, color, LEFT, TITLE, plot_width, plot_height)
    if len(points) >= 2:
        axis_y = TITLE + plot_height + 18
        parts.append(f'<text x="{LEFT}" y="{axis_y}" fill="{MUTED}" font-size="10">{escape(str(points[0]["sha"]))}</text>')
        parts.append(f'<text x="{LEFT + plot_width}" y="{axis_y}" fill="{MUTED}" font-size="10" '
                     f'text-anchor="end">{escape(str(points[-1]["sha"]))}</text>')
    frame = f'<rect width="{width}" height="{height}" fill="{BACKGROUND}"/>'
    return "\n".join([svg_open(width, height), frame, *parts, "</svg>"])


def point_rows(points: list[dict[str, object]]) -> Iterator[str]:
    for point in reversed(points):
        yield (f'<tr><td><code>{escape(str(point["sha"]))}</code></td>'
               f'<td>{point["services"]}%</td><td>{point["shaders"]}%</td></tr>')


def page(points: list[dict[str, object]]) -> str:
    if len(points) < 2:
        body = "<p>Not enough commits measured yet to draw a history chart.</p>"
    else:
        body = "\n".join([render(points), "<h2>Per commit</h2>",
                          "<table><tr><th>Commit</th><th>Services bound</th><th>Shaders</th></tr>",
                          *point_rows(points), "</table>"])
    return "\n".join([
        "<!DOCTYPE html>", '<html lang="en"><head><meta charset="utf-8">',
        "<title>Lichii progress history</title>", f"<style>{PAGE_STYLE}</style></head><body>",
        "<h1>Lichii progress history</h1>", body,
        '<p><a href="progress.html">Current progress</a></p>',
        "</body></html>",
    ])


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--root", type=Path, default=progress.ROOT)
    parser.add_argument("--limit", type=int, default=MAX_POINTS)
    parser.add_argument("--cache", type=Path, help="reuse per-commit counts while the scanners are unchanged")
    args = parser.parse_args(argv)
    try:
        points = snapshots(args.root, args.limit, args.cache)
    except ValueError as error:
        parser.error(str(error))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "history.json").write_text(json.dumps(points, indent=2) + "\n", encoding="utf-8")
    (args.output / "history.html").write_text(page(points), encoding="utf-8")
    print(f"history {len(points)} points")
    return 0


if __name__ == "__main__":
    sys.exit(main())

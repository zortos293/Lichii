#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import collections
import dataclasses
import json
import re
import sys
import urllib.error
import urllib.request
from collections.abc import Iterator, Sequence
from enum import Enum
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SERVICES = Path("src/core/hle/service")
OPCODES = Path("src/shader_recompiler/frontend/ir/opcodes.inc")
SPIRV = Path("src/shader_recompiler/backend/spirv")
COMPATIBILITY = "https://lichii.zortos.me/api/summary"
REPOSITORY = "zortos293/Lichii"
BRANCH = "nx2"

TABLE = re.compile(r"FunctionInfo\s+\w+\s*\[\s*\]\s*=\s*\{")
ROW = re.compile(r'\{\s*(?:0x[0-9a-fA-F]+|\d+)\s*,\s*(.*?)\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}')
EMITTER = re.compile(r"\b(?:Id|void)\s+Emit(\w+)\s*\([^;{}]*\)\s*\{")
SECTION = re.compile(r"^//\s*(.+?)\s*$", re.MULTILINE)
OPCODE_LINE = re.compile(r"^OPCODE\((\w+)", re.MULTILINE)
THROW = re.compile(r"throw\s+NotImplementedException\s*\([^;]*\)\s*;")
STUB_HANDLER = re.compile(r"::Stub\w*")
CODE_NOISE = re.compile(r"[\s{}();]|\(void\)\s*\w+")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")

WIDTH = 960
GAP = 10
PANEL_WIDTH = (WIDTH - GAP) / 2
MAP_HEIGHT = 280.0
HEADER = 30.0
GRID_ROW = 30.0
DONE_COLOR = "#2ea043"
TODO_COLOR = "#6e7681"
BORDER = "#0d1117"
TEXT = "#ffffff"
MUTED = "#8b949e"
WARN_COLOR = "#d29922"
TRACK_COLOR = "#30363d"
SQUARIFY_LIMIT = 40
TILES_PER_ROW = 12
SOURCE = f"https://github.com/{REPOSITORY}/tree/{BRANCH}/{SERVICES.as_posix()}"
TABLE_HEAD = ("<table><tr><th>Module</th><th>Emulated</th><th>Stub reply</th><th>Unbound</th>"
              "<th>Declared</th><th>Answered %</th></tr>")
PAGE_STYLE = (
    "body{background:#0d1117;color:#c9d1d9;font:14px/1.5 -apple-system,Segoe UI,sans-serif;"
    "margin:2rem auto;max-width:60rem;padding:0 1rem}"
    "table{border-collapse:collapse;width:100%;margin:1rem 0}"
    "th,td{border-bottom:1px solid #30363d;padding:.3rem .6rem;text-align:left}"
    "th{color:#8b949e;font-weight:600}code{color:#79c0ff}a{color:#58a6ff}"
)


class Handler(Enum):
    BOUND = "bound"
    STUB = "stub"
    UNBOUND = "unbound"


@dataclasses.dataclass(frozen=True, slots=True)
class Metric:
    done: int
    total: int

    @property
    def percent(self) -> float:
        return round(100 * self.done / self.total, 2) if self.total else 0.0


@dataclasses.dataclass(frozen=True, slots=True)
class Module:
    name: str
    counts: tuple[tuple[Handler, int], ...]

    def count(self, handler: Handler) -> int:
        return dict(self.counts).get(handler, 0)

    @property
    def total(self) -> int:
        return sum(count for _, count in self.counts)

    @property
    def answered(self) -> int:
        return self.count(Handler.BOUND) + self.count(Handler.STUB)

    @property
    def metric(self) -> Metric:
        return Metric(self.answered, self.total)


@dataclasses.dataclass(frozen=True, slots=True)
class Services:
    modules: tuple[Module, ...]

    @property
    def metric(self) -> Metric:
        return Metric(sum(module.metric.done for module in self.modules),
                      sum(module.metric.total for module in self.modules))

    def sorted(self) -> tuple[Module, ...]:
        return tuple(sorted(self.modules, key=lambda module: (-module.total, module.name)))


@dataclasses.dataclass(frozen=True, slots=True)
class Group:
    name: str
    done: int
    total: int

    @property
    def metric(self) -> Metric:
        return Metric(self.done, self.total)


@dataclasses.dataclass(frozen=True, slots=True)
class Shaders:
    groups: tuple[Group, ...]
    throws: tuple[tuple[str, int], ...]

    @property
    def metric(self) -> Metric:
        return Metric(sum(group.done for group in self.groups),
                      sum(group.total for group in self.groups))

    @property
    def missing(self) -> int:
        return self.metric.total - self.metric.done


@dataclasses.dataclass(frozen=True, slots=True)
class Compatibility:
    games: int
    reports: int
    playable: int
    perfect: int
    build: int


@dataclasses.dataclass(frozen=True, slots=True)
class Report:
    services: Services
    shaders: Shaders
    compatibility: Compatibility | None


def block_end(text: str, start: int) -> int:
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return len(text)


def strip_comments(text: str) -> str:
    return LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", text))


def iter_tables(text: str) -> Iterator[str]:
    for match in TABLE.finditer(text):
        brace = match.end() - 1
        yield text[brace:block_end(text, brace) + 1]


def classify(expression: str) -> Handler:
    if expression.strip() == "nullptr":
        return Handler.UNBOUND
    if STUB_HANDLER.search(expression):
        return Handler.STUB
    return Handler.BOUND


def service_files(root: Path) -> tuple[Path, ...]:
    directory = root / SERVICES
    return tuple(sorted(directory.rglob("*.cpp"))) + tuple(sorted(directory.rglob("*.h")))


def module_name(path: Path, root: Path) -> str:
    relative = path.relative_to(root / SERVICES)
    return relative.parts[0] if len(relative.parts) > 1 else relative.stem


def scan_services(root: Path) -> Services:
    tallies: dict[str, collections.Counter[Handler]] = {}
    for path in service_files(root):
        counter = tallies.setdefault(module_name(path, root), collections.Counter())
        text = strip_comments(path.read_text(encoding="utf-8", errors="ignore"))
        for table in iter_tables(text):
            for match in ROW.finditer(table):
                counter[classify(match.group(1))] += 1
    modules = tuple(Module(name, tuple(sorted(counter.items(), key=lambda item: item[0].value)))
                    for name, counter in tallies.items() if counter)
    return Services(modules)


def emitter_bodies(root: Path) -> dict[str, list[str]]:
    bodies: dict[str, list[str]] = {}
    for path in sorted((root / SPIRV).rglob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="ignore")
        for match in EMITTER.finditer(text):
            brace = match.end() - 1
            bodies.setdefault(match.group(1), []).append(text[brace:block_end(text, brace) + 1])
    return bodies


def has_no_work(body: str) -> bool:
    if "NotImplementedException" not in body:
        return False
    code = LINE_COMMENT.sub("", BLOCK_COMMENT.sub("", body))
    return not CODE_NOISE.sub("", THROW.sub("", code))


def shader_sections(root: Path) -> list[tuple[str, list[str]]]:
    sections: list[tuple[str, list[str]]] = []
    current = ""
    for line in (root / OPCODES).read_text(encoding="utf-8", errors="ignore").splitlines():
        heading = SECTION.match(line)
        if heading and not line.startswith("// SPDX") and not line.startswith("//     opcode"):
            current = heading.group(1)
            continue
        opcode = OPCODE_LINE.match(line)
        if not opcode:
            continue
        if not sections or sections[-1][0] != current:
            sections.append((current, []))
        sections[-1][1].append(opcode.group(1))
    return [(name, names) for name, names in sections if names]


def throw_areas(root: Path) -> tuple[tuple[str, int], ...]:
    counts: dict[str, int] = {}
    for path in sorted((root / "src/shader_recompiler").rglob("*.cpp")):
        found = path.read_text(encoding="utf-8", errors="ignore").count("NotImplementedException")
        if not found:
            continue
        relative = path.relative_to(root / "src/shader_recompiler")
        area = relative.parts[0] if len(relative.parts) > 1 else relative.stem
        counts[area] = counts.get(area, 0) + found
    return tuple(sorted(counts.items(), key=lambda item: (-item[1], item[0])))


def scan_shaders(root: Path) -> Shaders:
    bodies = emitter_bodies(root)
    groups = []
    for name, opcodes in shader_sections(root):
        missing = sum(1 for opcode in opcodes
                      if not bodies.get(opcode) or all(map(has_no_work, bodies[opcode])))
        groups.append(Group(name, len(opcodes) - missing, len(opcodes)))
    return Shaders(tuple(groups), throw_areas(root))


def parse_compatibility(payload: object) -> Compatibility | None:
    if not isinstance(payload, dict):
        return None
    overall = payload.get("overall")
    counts = overall.get("counts") if isinstance(overall, dict) else None
    values = [payload.get("gamesTested"), payload.get("latestBuild"),
              overall.get("total") if isinstance(overall, dict) else None,
              overall.get("playable") if isinstance(overall, dict) else None,
              counts.get("Perfect") if isinstance(counts, dict) else None]
    if not all(isinstance(value, int) for value in values):
        return None
    games, build, total, playable, perfect = (int(value) for value in values)
    return Compatibility(games, total, playable, perfect, build)


def fetch_compatibility(url: str, timeout: float) -> Compatibility | None:
    if not url:
        return None
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return parse_compatibility(json.loads(response.read().decode("utf-8")))
    except (OSError, ValueError, urllib.error.URLError) as error:
        print(f"warning: compatibility numbers left out: {error}", file=sys.stderr)
        return None


def percent_color(percent: float) -> str:
    return "#2ea043" if percent >= 90 else "#97ca00" if percent >= 60 else "#dfb317" if percent >= 30 else "#fe7d37"


def svg_open(width: float, height: float) -> str:
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
            f'width="{width}" height="{height}">')


def badge(label: str, value: str, percent: float) -> str:
    left, right = 10 + 7 * len(label), 10 + 7 * len(value)
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{left + right}" height="20" role="img" '
            f'aria-label="{label}: {value}"><rect width="{left}" height="20" fill="#555"/>'
            f'<rect x="{left}" width="{right}" height="20" fill="{percent_color(percent)}"/>'
            f'<g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11" text-anchor="middle">'
            f'<text x="{left / 2}" y="14">{label}</text><text x="{left + right / 2}" y="14">{value}</text></g></svg>')


def worst_ratio(row: Sequence[float], side: float) -> float:
    area = sum(row)
    return max(max(side * side * value / area ** 2, area ** 2 / (side * side * value)) for value in row)


def place(row: Sequence[float], x: float, y: float, w: float, h: float,
          rects: list[tuple[float, float, float, float]]) -> tuple[float, float, float, float]:
    thickness = sum(row) / min(w, h)
    offset = 0.0
    for area in row:
        length = area / thickness
        if w >= h:
            rects.append((x, y + offset, thickness, length))
        else:
            rects.append((x + offset, y, length, thickness))
        offset += length
    if w >= h:
        return x + thickness, y, w - thickness, h
    return x, y + thickness, w, h - thickness


def squarify(values: Sequence[float], x: float, y: float, w: float, h: float) -> list[tuple[float, float, float, float]]:
    total = sum(values)
    if not total or w <= 0 or h <= 0:
        return []
    areas = [value * w * h / total for value in values]
    rects: list[tuple[float, float, float, float]] = []
    row: list[float] = []
    while areas:
        side = min(w, h)
        if not row or worst_ratio([*row, areas[0]], side) <= worst_ratio(row, side):
            row.append(areas.pop(0))
            continue
        x, y, w, h = place(row, x, y, w, h, rects)
        row = []
    if row:
        place(row, x, y, w, h, rects)
    return rects


def cells(count: int, x: float, y: float, w: float, h: float) -> list[tuple[float, float, float, float]]:
    if not count or w <= 0 or h <= 0:
        return []
    rows = max(1, min(count, round((count * h / w) ** 0.5)))
    cells_out = []
    for row in range(rows):
        in_row = count // rows + (row < count % rows)
        width = w / in_row
        cells_out += [(x + column * width, y + row * h / rows, width, h / rows) for column in range(in_row)]
    return cells_out


def rect(x: float, y: float, w: float, h: float, color: str, stroke: float = 0.5) -> str:
    return (f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}" '
            f'fill="{color}" stroke="{BORDER}" stroke-width="{stroke}"/>')


def tile_label(text: str, w: float, h: float) -> str:
    room = int((w - 10) / 7)
    label = text[:room] if room > 0 else ""
    return label if (label == text or len(label) >= 3) and h > 22 else ""


def tile(group: Group, x: float, y: float, w: float, h: float) -> list[str]:
    metric = group.metric
    suffix = "" if group.name else "unnamed section"
    title = (f'<g><title>{escape(group.name)}: {metric.done}/{metric.total} '
             f'({metric.percent}%){escape(suffix)}</title>')
    parts = [title]
    for index, cell in enumerate(cells(group.total, x + 1, y + 1, w - 2, h - 2)):
        parts.append(rect(*cell, DONE_COLOR if index < group.done else TODO_COLOR))
    parts.append(rect(x, y, w, h, "none", 2))
    label = tile_label(group.name, w, h)
    if label:
        parts.append(f'<text x="{x + 5:.2f}" y="{y + 16:.2f}" font-family="sans-serif" font-size="12" '
                     f'fill="{TEXT}" stroke="{BORDER}" stroke-width="3" paint-order="stroke">{escape(label)}</text>')
    parts.append("</g>")
    return parts


def treemap(title: str, groups: Sequence[Group], left: float, top: float) -> list[str]:
    ordered = sorted(groups, key=lambda group: -group.total)
    total = Metric(sum(group.done for group in ordered), sum(group.total for group in ordered))
    parts = [panel_heading(title, total, left, top)]
    layout = squarify([group.total for group in ordered], left, top + HEADER, PANEL_WIDTH, MAP_HEIGHT)
    for group, (x, y, w, h) in zip(ordered, layout):
        parts += tile(group, x, y, w, h)
    return parts


def tile_grid(title: str, groups: Sequence[Group], top: float) -> tuple[list[str], float]:
    ordered = sorted(groups, key=lambda group: group.name.lower())
    width = (2 * PANEL_WIDTH + GAP) / TILES_PER_ROW
    parts = [panel_heading(title, Metric(sum(g.done for g in ordered), sum(g.total for g in ordered)), 0, top)]
    for index, group in enumerate(ordered):
        parts += tile(group, index % TILES_PER_ROW * width, top + HEADER + index // TILES_PER_ROW * GRID_ROW,
                      width, GRID_ROW)
    return parts, HEADER + -(-len(ordered) // TILES_PER_ROW) * GRID_ROW


def panel_heading(title: str, metric: Metric, x: float, y: float) -> str:
    return (f'<text x="{x + 4:.2f}" y="{y + 21:.2f}" font-family="sans-serif" font-size="16" fill="{TEXT}" '
            f'stroke="{BORDER}" stroke-width="3" paint-order="stroke">'
            f'{escape(title)}: {metric.percent}% ({metric.done}/{metric.total})</text>')


def bar_row(label: str, count: int, total: int, scale: int, left: float, y: float, color: str) -> list[str]:
    width = 240.0
    fraction = count / scale if scale else 0.0
    caption = f'{count}{f"/{total}" if total else ""}'
    name = f'<text x="{left:.2f}" y="{y + 11:.2f}" font-family="sans-serif" font-size="12" fill="{TEXT}">{escape(label)}</text>'
    value = (f'<text x="{left + 150 + width + 8:.2f}" y="{y + 11:.2f}" font-family="sans-serif" font-size="12" '
             f'fill="{MUTED}">{caption}</text>')
    return [name, rect(left + 150, y, width, 12, TRACK_COLOR, 0),
            rect(left + 150, y, width * min(1.0, fraction), 12, color, 0), value]


def services_tiers(services: Services) -> list[tuple[str, list[Group]]]:
    groups = [Group(module.name, module.metric.done, module.total) for module in services.sorted()]
    large, medium = groups[:4], groups[4:18]
    small = groups[18:]
    return [("Large system services", large), ("Medium system services", medium),
            ("Small system services", small)]


def still_needed_panel(shaders: Shaders, left: float, top: float) -> list[str]:
    scale = max((count for _, count in shaders.throws), default=0)
    title = (f'<text x="{left + 4:.2f}" y="{top + 21:.2f}" font-family="sans-serif" font-size="16" fill="{TEXT}" '
             f'stroke="{BORDER}" stroke-width="3" paint-order="stroke">NotImplementedException sites by area</text>')
    parts = [title]
    y = top + HEADER
    for name, count in shaders.throws:
        parts += bar_row(name, count, 0, scale, left, y, WARN_COLOR)
        y += 20
    return parts


def render_svg(report: Report) -> str:
    tiers = services_tiers(report.services)
    parts: list[str] = []
    top = 0.0
    for index, (title, groups) in enumerate(tiers[:2]):
        if groups:
            parts += treemap(title, groups, index * (PANEL_WIDTH + GAP), top)
    top += HEADER + MAP_HEIGHT + GAP
    if tiers[2][1]:
        grid, height = tile_grid(tiers[2][0], tiers[2][1], top)
        parts += grid
        top += height + GAP
    parts += treemap("Shader IR opcodes decoded", report.shaders.groups, 0, top)
    parts += still_needed_panel(report.shaders, PANEL_WIDTH + GAP, top)
    top += HEADER + MAP_HEIGHT + GAP
    if report.compatibility:
        parts.append(panel_heading("Community compatibility reports",
                                   Metric(report.compatibility.playable, report.compatibility.reports), 0, top))
        top += HEADER
        detail = (f"{report.compatibility.reports} reports across {report.compatibility.games} games on build "
                  f"{report.compatibility.build}, {report.compatibility.perfect} perfect")
        parts.append(f'<text x="4" y="{top + 12:.2f}" font-family="sans-serif" font-size="12" '
                     f'fill="{MUTED}">{escape(detail)}</text>')
        top += 20
    height = round(top)
    frame = f'<rect width="{WIDTH}" height="{height}" fill="{BORDER}"/>'
    return "\n".join([svg_open(WIDTH, height), frame, *parts, "</svg>"])


def module_rows(modules: Sequence[Module]) -> Iterator[str]:
    for module in modules:
        metric = module.metric
        yield (f'<tr><td><code>{escape(module.name)}</code></td><td>{module.count(Handler.BOUND)}</td>'
               f'<td>{module.count(Handler.STUB)}</td><td>{module.count(Handler.UNBOUND)}</td>'
               f'<td>{metric.total}</td><td>{metric.percent}%</td></tr>')


def render_html(report: Report) -> str:
    services, shaders = report.services.metric, report.shaders.metric
    compatibility = ""
    if report.compatibility:
        compatibility = (f"<p>{report.compatibility.reports} community reports across "
                         f"{report.compatibility.games} games on build {report.compatibility.build}: "
                         f"{report.compatibility.playable} playable, {report.compatibility.perfect} perfect.</p>")
    summary = (f"<p>Services: <strong>{services.percent}%</strong> of declared IPC commands ({services.done} of "
               f"{services.total}) are answered, by an emulated handler or a stub reply. Shaders: "
               f"<strong>{shaders.percent}%</strong> of IR opcodes ({shaders.done} of {shaders.total}) have a "
               f"working SPIR-V emitter.</p>")
    note = ('A row with <code>nullptr</code> declares a command the emulator does not handle at all. A row bound '
            "to a <code>Stub*</code> helper answers the call but returns a placeholder, so it is listed separately "
            "from a real handler.</p>")
    return "\n".join([
        "<!DOCTYPE html>", '<html lang="en"><head><meta charset="utf-8">',
        "<title>Lichii progress</title>", f"<style>{PAGE_STYLE}</style></head><body>",
        "<h1>Lichii progress</h1>", summary, compatibility,
        "<h2>System services</h2>", TABLE_HEAD,
        *module_rows(report.services.sorted()), "</table>",
        f'<p>Counted from the <code>FunctionInfo</code> tables in <a href="{SOURCE}">{SERVICES.as_posix()}</a>. {note}',
        '<p><a href="history.html">Progress history</a></p>',
        "</body></html>",
    ])


def site_data(report: Report) -> dict[str, object]:
    services = report.services.metric
    shaders = report.shaders.metric
    data: dict[str, object] = {
        "services": {"answered": services.done, "total": services.total, "percent": services.percent,
                     "modules": [{"name": module.name, "emulated": module.count(Handler.BOUND),
                                  "stub": module.count(Handler.STUB),
                                  "unbound": module.count(Handler.UNBOUND)}
                                 for module in report.services.sorted()]},
        "shaders": {"implemented": shaders.done, "total": shaders.total, "percent": shaders.percent,
                    "sections": [{"name": group.name, "done": group.done, "total": group.total}
                                 for group in report.shaders.groups],
                    "throw_areas": dict(report.shaders.throws)},
    }
    if report.compatibility:
        data["compatibility"] = dataclasses.asdict(report.compatibility)
    return data


def write_site(output: Path, report: Report) -> None:
    output.mkdir(parents=True, exist_ok=True)
    services, shaders = report.services.metric, report.shaders.metric
    (output / "progress.json").write_text(json.dumps(site_data(report), indent=2) + "\n", encoding="utf-8")
    (output / "badge-services.svg").write_text(
        badge("services", f"{services.percent}%", services.percent), encoding="utf-8")
    (output / "badge-shaders.svg").write_text(
        badge("shaders", f"{shaders.percent}%", shaders.percent), encoding="utf-8")
    (output / "progress.svg").write_text(render_svg(report), encoding="utf-8")
    (output / "progress.html").write_text(render_html(report), encoding="utf-8")


def snapshot_metric(snapshot: object, key: str, done: str, total: str) -> Metric | None:
    if not isinstance(snapshot, dict):
        return None
    section = snapshot.get(key)
    if not isinstance(section, dict):
        return None
    values = section.get(done, section.get("bound")), section.get(total)
    if not all(isinstance(value, int) for value in values):
        return None
    return Metric(int(values[0]), int(values[1]))


def format_delta(before: Metric, after: Metric) -> str:
    change = after.done - before.done
    return f"{before.done}/{before.total} -> {after.done}/{after.total} ({change:+d})"


def report(base: object, head: object) -> str:
    lines = []
    for label, key, done in (("IPC commands answered", "services", "answered"),
                             ("Shader IR opcodes", "shaders", "implemented")):
        before = snapshot_metric(base, key, done, "total")
        after = snapshot_metric(head, key, done, "total")
        if before is None or after is None:
            continue
        if (before.done, before.total) == (after.done, after.total):
            continue
        lines.append(f"| {label} | {before.done}/{before.total} | {after.done}/{after.total} | "
                     f"{after.done - before.done:+d} |")
    if not lines:
        return ""
    return "\n".join(["| Measure | Before | After | Delta |", "| --- | --- | --- | --- |", *lines]) + "\n"


def load_snapshot(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--root", type=Path, help="source tree to measure instead of the one containing this script")
    parser.add_argument("--json", action="store_true", help="print progress.json to stdout")
    parser.add_argument("--compare", type=Path, nargs=2, metavar=("BASE", "HEAD"),
                        help="print a markdown report of the changes between two progress.json files")
    parser.add_argument("--compatibility-url", default="", help="compatibility API to read; empty skips it")
    args = parser.parse_args(argv)
    if args.compare:
        try:
            print(report(load_snapshot(args.compare[0]), load_snapshot(args.compare[1])), end="")
        except (OSError, ValueError) as error:
            parser.error(f"{error}")
        return 0
    root = args.root or ROOT
    if not (root / SERVICES).is_dir():
        parser.error(f"{root} is not a source tree: {SERVICES} not found")
    measured = Report(scan_services(root), scan_shaders(root),
                      fetch_compatibility(args.compatibility_url, timeout=15))
    if args.output:
        write_site(args.output, measured)
    if args.json:
        print(json.dumps(site_data(measured), indent=2))
    else:
        services, shaders = measured.services.metric, measured.shaders.metric
        print(f"services {services.done}/{services.total} ({services.percent}%)")
        print(f"shaders {shaders.done}/{shaders.total} ({shaders.percent}%)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

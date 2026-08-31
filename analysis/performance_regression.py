#!/usr/bin/env python3
"""Online CUSUM/EWMA detection for order-book performance telemetry."""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path
from statistics import mean, stdev
from typing import Iterable


@dataclass(frozen=True)
class DetectionResult:
    scores: list[float]
    alarms: list[int]


def lower_cusum(
    values: list[float], calibration_batches: int, drift: float = 0.5, threshold: float = 10.0
) -> DetectionResult:
    """Detect downward shifts using only the calibration prefix and past observations."""
    baseline = values[:calibration_batches]
    center = mean(baseline)
    scale = stdev(baseline)
    if scale == 0:
        scale = max(abs(center) * 1e-9, 1e-9)
    score = 0.0
    scores = [0.0] * calibration_batches
    alarms: list[int] = []
    for index in range(calibration_batches, len(values)):
        standardized_drop = (center - values[index]) / scale
        score = max(0.0, score + standardized_drop - drift)
        scores.append(score)
        if score > threshold:
            alarms.append(index)
            score = 0.0
    return DetectionResult(scores, alarms)


def lower_ewma(
    values: list[float], calibration_batches: int, alpha: float = 0.2, limit: float = 4.0
) -> DetectionResult:
    """Detect downward shifts with a causal exponentially weighted moving average."""
    baseline = values[:calibration_batches]
    center = mean(baseline)
    scale = stdev(baseline)
    if scale == 0:
        scale = max(abs(center) * 1e-9, 1e-9)
    current = center
    scores = [center] * calibration_batches
    alarms: list[int] = []
    in_alarm = False
    for step, index in enumerate(range(calibration_batches, len(values)), start=1):
        current = alpha * values[index] + (1.0 - alpha) * current
        scores.append(current)
        ewma_scale = scale * math.sqrt(
            alpha / (2.0 - alpha) * (1.0 - (1.0 - alpha) ** (2 * step))
        )
        below_limit = current < center - limit * ewma_scale
        if below_limit and not in_alarm:
            alarms.append(index)
        in_alarm = below_limit
    return DetectionResult(scores, alarms)


def read_telemetry(path: Path, metric: str) -> tuple[list[dict[str, str]], list[float], int]:
    with path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    if not rows or metric not in rows[0]:
        raise ValueError(f"{path} does not contain metric {metric!r}")
    values = [float(row[metric]) for row in rows]
    try:
        change_batch = next(i for i, row in enumerate(rows) if row["phase"] == "regression")
    except StopIteration as exc:
        raise ValueError(f"{path} has no regression phase") from exc
    return rows, values, change_batch


def evaluation(alarms: Iterable[int], change_batch: int) -> tuple[int, int | None]:
    alarm_list = list(alarms)
    false_alarms = sum(index < change_batch for index in alarm_list)
    after_change = [index for index in alarm_list if index >= change_batch]
    delay = after_change[0] - change_batch if after_change else None
    return false_alarms, delay


def _polyline(values: list[float], x0: float, y0: float, width: float, height: float) -> str:
    low, high = min(values), max(values)
    span = high - low or 1.0
    points = []
    for index, value in enumerate(values):
        x = x0 + width * index / max(len(values) - 1, 1)
        y = y0 + height * (high - value) / span
        points.append(f"{x:.1f},{y:.1f}")
    return " ".join(points)


def write_svg(
    path: Path,
    values: list[float],
    change_batch: int,
    cusum_alarms: list[int],
    ewma_alarms: list[int],
    title: str,
) -> None:
    width, height = 1000, 520
    left, top, plot_width, plot_height = 80, 70, 860, 360
    low, high = min(values), max(values)
    change_x = left + plot_width * change_batch / max(len(values) - 1, 1)

    def representative_alarms(indices: list[int]) -> list[int]:
        before = [index for index in indices if index < change_batch]
        after = [index for index in indices if index >= change_batch]
        return ([before[0]] if before else []) + ([after[0]] if after else [])

    def alarm_lines(indices: list[int], color: str, dash: str) -> str:
        return "".join(
            f'<line x1="{left + plot_width * i / max(len(values) - 1, 1):.1f}" y1="{top}" '
            f'x2="{left + plot_width * i / max(len(values) - 1, 1):.1f}" y2="{top + plot_height}" '
            f'stroke="{color}" stroke-width="1.5" stroke-dasharray="{dash}"/>'
            for i in indices
        )

    svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
<rect width="100%" height="100%" fill="white"/>
<text x="{width / 2}" y="32" text-anchor="middle" font-family="sans-serif" font-size="20">{title}</text>
<rect x="{left}" y="{top}" width="{plot_width}" height="{plot_height}" fill="#f8fafc" stroke="#94a3b8"/>
<line x1="{change_x:.1f}" y1="{top}" x2="{change_x:.1f}" y2="{top + plot_height}" stroke="#111827" stroke-width="2" stroke-dasharray="8,5"/>
{alarm_lines(representative_alarms(cusum_alarms), "#dc2626", "5,4")}
{alarm_lines(representative_alarms(ewma_alarms), "#7c3aed", "2,4")}
<polyline fill="none" stroke="#0284c7" stroke-width="2" points="{_polyline(values, left, top, plot_width, plot_height)}"/>
<text x="{left}" y="{top - 12}" font-family="sans-serif" font-size="13">throughput: {low / 1e6:.2f}M–{high / 1e6:.2f}M events/s</text>
<text x="{change_x + 6:.1f}" y="{top + 18}" font-family="sans-serif" font-size="12">controlled shift</text>
<text x="{left}" y="{top + plot_height + 32}" font-family="sans-serif" font-size="13">batch</text>
<line x1="{left + 400}" y1="{height - 43}" x2="{left + 435}" y2="{height - 43}" stroke="#0284c7" stroke-width="2"/><text x="{left + 443}" y="{height - 38}" font-family="sans-serif" font-size="12">throughput</text>
<line x1="{left + 530}" y1="{height - 43}" x2="{left + 565}" y2="{height - 43}" stroke="#dc2626" stroke-dasharray="5,4"/><text x="{left + 573}" y="{height - 38}" font-family="sans-serif" font-size="12">CUSUM alarm</text>
<line x1="{left + 680}" y1="{height - 43}" x2="{left + 715}" y2="{height - 43}" stroke="#7c3aed" stroke-dasharray="2,4"/><text x="{left + 723}" y="{height - 38}" font-family="sans-serif" font-size="12">EWMA alarm</text>
</svg>'''
    path.write_text(svg, encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("telemetry", nargs="+", type=Path)
    parser.add_argument("--metric", default="throughput_eps")
    parser.add_argument("--calibration-batches", type=int, default=30)
    parser.add_argument("--output-dir", type=Path, default=Path("results"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    summary_rows = []
    for telemetry_path in args.telemetry:
        rows, values, change_batch = read_telemetry(telemetry_path, args.metric)
        if args.calibration_batches < 2 or args.calibration_batches >= change_batch:
            raise ValueError("calibration batches must be at least 2 and end before the shift")
        cusum = lower_cusum(values, args.calibration_batches)
        ewma = lower_ewma(values, args.calibration_batches)
        cusum_false, cusum_delay = evaluation(cusum.alarms, change_batch)
        ewma_false, ewma_delay = evaluation(ewma.alarms, change_batch)
        magnitude = rows[change_batch]["matching_percent"]
        summary_rows.extend(
            [
                {"file": telemetry_path.name, "matching_percent": magnitude, "detector": "CUSUM", "false_alarms": cusum_false, "detection_delay_batches": cusum_delay},
                {"file": telemetry_path.name, "matching_percent": magnitude, "detector": "EWMA", "false_alarms": ewma_false, "detection_delay_batches": ewma_delay},
            ]
        )
        write_svg(
            args.output_dir / f"{telemetry_path.stem}.svg",
            values,
            change_batch,
            cusum.alarms,
            ewma.alarms,
            f"Performance regression: {magnitude}% matching workload",
        )

    summary_path = args.output_dir / "detection_summary.csv"
    with summary_path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=summary_rows[0].keys(), lineterminator="\n")
        writer.writeheader()
        writer.writerows(summary_rows)
    print(f"summary_file={summary_path}")


if __name__ == "__main__":
    main()

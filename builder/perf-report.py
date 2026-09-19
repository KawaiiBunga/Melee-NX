#!/usr/bin/env python3
"""Summarize runtime captures without mistaking logged averages for frame samples.

Use --start/--end (seconds since app start) to select a matched gameplay window.
Example: python builder/perf-report.py capture.log --start 90 --end 210 --json out.json
"""
import argparse
import json
import re
from pathlib import Path


def quantile(values, fraction):
    values = sorted(values)
    index = (len(values) - 1) * fraction
    lo = int(index)
    hi = min(lo + 1, len(values) - 1)
    return round(values[lo] + (values[hi] - values[lo]) * (index - lo), 3)


def summarize(path, start, end):
    text = path.read_text(encoding="utf-8", errors="replace")
    result = {"file": str(path.resolve()), "start_s": start, "end_s": end,
              "metadata": [], "perf_samples": 0, "interval_statistics": {},
              "pipeline_windows": 0, "skipped_draws": 0, "zero_skip_windows": 0}
    values = {}
    frame_windows = []
    stages = {}
    for line in text.splitlines():
        if any(marker in line for marker in ["manifest:", "SWEEP ", "THREAD ", "AUDIO ", "framebuffer size"]):
            result["metadata"].append(line)
        stamp = re.match(r"\[(\d+(?:\.\d+)?)\]", line)
        if not stamp:
            continue
        seconds = float(stamp[1]) / 1000
        if seconds < start or (end is not None and seconds > end):
            continue
        if "PERF fps " in line:
            result["perf_samples"] += 1
            for key, val in re.findall(r"\b(fps|frame|peak|sim|submit|wait|pump|post|log|other|worst) (-?\d+(?:\.\d+)?)", line):
                values.setdefault(key, []).append(float(val))
        pipeline = re.search(r"PIPELINES skipped_draws(?:/60f)? (\d+)", line)
        if pipeline:
            misses = int(pipeline[1])
            result["pipeline_windows"] += 1
            result["skipped_draws"] += misses
            result["zero_skip_windows"] += misses == 0
        if "FRAMES n " in line:
            frame_windows.append(dict((key, float(val)) for key, val in re.findall(r"(\w+) (-?\d+(?:\.\d+)?)", line)))
        if "STAGES " in line:
            for key, val in re.findall(r"(\w+_ms) (\d+(?:\.\d+)?)", line):
                stages.setdefault(key, []).append(float(val))
    for key, data in values.items():
        result["interval_statistics"][key] = {
            "p10": quantile(data, .1), "p50": quantile(data, .5),
            "p90": quantile(data, .9), "max": max(data)}
    result["notes"] = ["PERF percentiles describe logged interval averages, not individual frames.",
                       "Pipeline windows may straddle the selected time boundary.",
                       "Stage timings are game-thread wall time; GPU work may overlap on the render worker."]
    if frame_windows:
        samples = sum(w["n"] for w in frame_windows)
        span = sum(w["span_ms"] for w in frame_windows)
        result["frame_windows"] = frame_windows
        result["frame_totals"] = {"n": samples, "span_ms": span, "fps": samples * 1000 / span,
                                  "late60": sum(w["late60"] for w in frame_windows),
                                  "overflow": sum(w["overflow"] for w in frame_windows),
                                  "worst_ms": max(w["worst_ms"] for w in frame_windows)}
        result["notes"].append("FRAMES percentiles are per-window 0.25 ms upper bounds; -1 means >=1000 ms. Do not average quantiles.")
    if stages:
        result["stage_window_medians_ms"] = {key: quantile(data, .5) for key, data in stages.items()}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs="+")
    parser.add_argument("--start", type=float, default=0)
    parser.add_argument("--end", type=float)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    if args.start < 0 or (args.end is not None and args.end <= args.start):
        parser.error("Require 0 <= start < end")
    reports = [summarize(path, args.start, args.end) for path in args.logs]
    output = json.dumps(reports, indent=2)
    if args.json:
        args.json.write_text(output + "\n", encoding="utf-8")
    for report in reports:
        print(report["file"])
        print(f'  {report["perf_samples"]} PERF intervals; {report["skipped_draws"]} skipped draws in {report["pipeline_windows"]} pipeline windows')
        for key in ["fps", "frame", "sim", "submit", "wait", "other"]:
            if key in report["interval_statistics"]:
                print(f'  {key}: {report["interval_statistics"][key]}')
        if "frame_totals" in report:
            print("  frame totals:", report["frame_totals"])
        if "stage_window_medians_ms" in report:
            print("  stage medians:", report["stage_window_medians_ms"])
    print("PERF percentiles are interval statistics, not frame-time percentiles.")

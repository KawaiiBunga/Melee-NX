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
    raw = path.read_text(encoding="utf-8", errors="replace")
    # Preserve the capture on disk. Concurrent log writers can leave NUL gaps;
    # recover intact records after a gap, but report the damaged input.
    text = raw.replace("\0", "")
    result = {"file": str(path.resolve()), "start_s": start, "end_s": end,
              "metadata": [], "perf_samples": 0, "interval_statistics": {},
              "pipeline_windows": 0, "skipped_draws": 0, "zero_skip_windows": 0}
    values = {}
    frame_windows = []
    stages = {}
    pipeline_records = []
    blob_records = []
    gx_records = []
    worker_records = {"FIFOWORK": [], "RENDERWORK": [], "RENDERSTAGES": []}
    log_records = []
    partial_blob_records = 0
    for line in text.splitlines():
        if any(marker in line for marker in ["manifest:", "SWEEP ", "THREAD ", "AUDIO ", "framebuffer size"]):
            result["metadata"].append(line)
        stamp = re.match(r"\[\s*(\d+(?:\.\d+)?)\]", line)
        if not stamp:
            continue
        seconds = float(stamp[1]) / 1000
        if seconds < start or (end is not None and seconds > end):
            continue
        if "PERF fps " in line:
            result["perf_samples"] += 1
            for key, val in re.findall(r"\b(fps|frame|peak|sim|submit|wait|pump|post|log|other|worst) (-?\d+(?:\.\d+)?)", line):
                values.setdefault(key, []).append(float(val))
        for marker, records in worker_records.items():
            if marker + " " in line:
                record = dict((key, float(val)) for key, val in
                              re.findall(r"(\w+) (\d+(?:\.\d+)?)", line))
                if record.get("frames", 0) > 0:
                    records.append(record)
        if "LOGIO " in line:
            log_records.append(dict((key, float(val)) for key, val in
                                    re.findall(r"(\w+) (\d+(?:\.\d+)?)", line)))
        pipeline = re.search(r"PIPELINES skipped_draws(?:/60f)? (\d+)", line)
        if pipeline:
            misses = int(pipeline[1])
            result["pipeline_windows"] += 1
            result["skipped_draws"] += misses
            result["zero_skip_windows"] += misses == 0
            pipeline_records.append(dict((key, float(val)) for key, val in
                                         re.findall(r"(\w+) (-?\d+(?:\.\d+)?)", line)))
        if "BLOBCACHE " in line:
            record = dict((key, float(val)) for key, val in re.findall(r"(\w+) (\d+(?:\.\d+)?)", line))
            if all(key in record for key in ["hits", "misses", "lookups", "avg_lookup_ms"]):
                blob_records.append(record)
            else:
                partial_blob_records += 1
        if "GXCPU " in line:
            record = dict((key, float(val)) for key, val in re.findall(r"(\w+) (\d+(?:\.\d+)?)", line))
            if all(key in record for key in ["same_pipeline", "shader_info_hits", "shader_info_misses"]):
                gx_records.append(record)
        if "FRAMES n " in line:
            record = dict((key, float(val)) for key, val in re.findall(r"(\w+) (-?\d+(?:\.\d+)?)", line))
            if all(key in record for key in ["n", "span_ms", "late60", "overflow", "worst_ms"]):
                frame_windows.append({"at_s": seconds, **record})
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
    creates = [p for p in pipeline_records if "created" in p and "avg_create_ms" in p]
    count = sum(p["created"] for p in creates)
    if count:
        result["pipeline_creation"] = {
            "count": count,
            "weighted_mean_ms": sum(p["created"] * p["avg_create_ms"] for p in creates) / count}
    if blob_records:
        hits = sum(b["hits"] for b in blob_records)
        misses = sum(b["misses"] for b in blob_records)
        lookups = sum(b["lookups"] for b in blob_records)
        result["blob_cache"] = {
            "windows": len(blob_records), "hits": hits, "misses": misses, "lookups": lookups,
            "hit_rate": hits / (hits + misses) if hits + misses else None,
            "weighted_mean_lookup_ms": sum(b["lookups"] * b["avg_lookup_ms"] for b in blob_records) / lookups
                if lookups else None}
        result["notes"].append("Blob lookup time is callback wall time after mutex acquisition, not pure SD or CPU time. Size-query and copy callbacks are both counted.")
        ram = [b for b in blob_records if "ram_hits" in b]
        if not ram:
            result["notes"].append("Legacy blob timings include diagnostic logging and their telemetry boundaries can differ by one callback.")
        if ram:
            ram_lookups = sum(b["lookups"] for b in ram)
            result["blob_cache"].update({
                "ram_hit_callbacks": sum(b["ram_hits"] for b in ram),
                "sql_reads": sum(b.get("sql_reads", 0) for b in ram),
                "max_ram_kb": max(b.get("ram_kb", 0) for b in ram),
                "weighted_mean_lock_ms": sum(b["lookups"] * b.get("avg_lock_ms", 0) for b in ram) / ram_lookups
                    if ram_lookups else None})
            result["notes"].append("RAM-cache builds measure completed callbacks before diagnostic logging and report mutex wait separately; older builds included logging and shifted boundaries by one callback.")
    for marker, records in worker_records.items():
        if records:
            count = sum(r["frames"] for r in records)
            keys = set.intersection(*(set(r) for r in records))
            result[marker.lower()] = {"frames": count, "windows": len(records),
                "weighted_mean_ms": {k: sum(r[k] * r["frames"] for r in records) / count
                                     for k in sorted(keys) if k.endswith("_ms")},
                "counts": {k: sum(r[k] for r in records)
                           for k in sorted(keys) if k != "frames" and not k.endswith("_ms")}}
    if any(worker_records.values()):
        result["notes"].append("FIFO and render timings are overlapping worker wall times, not GPU durations or CPU cycle counts. Render stages are contained in RENDERWORK end_ms; do not add them together.")
    if log_records:
        result["log_io"] = {"windows": len(log_records), **{
            k: sum(r.get(k, 0) for r in log_records)
            for k in ["bytes", "dropped_records", "errors", "write_ms", "flush_ms"]}}
    if gx_records:
        hits = sum(g["shader_info_hits"] for g in gx_records)
        misses = sum(g["shader_info_misses"] for g in gx_records)
        result["gx_cpu"] = {
            "same_pipeline": sum(g["same_pipeline"] for g in gx_records),
            "shader_info_hits": hits, "shader_info_misses": misses,
            "shader_info_hit_rate": hits / (hits + misses) if hits + misses else None}
    result["capture_integrity"] = {"nul_bytes": raw.count("\0"),
                                   "partial_or_legacy_blob_records": partial_blob_records}
    if raw.count("\0"):
        result["notes"].append("Capture contains NUL gaps; recovered intact lines after removing NUL bytes. Overwritten or truncated records cannot be reconstructed.")
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
        for key in ["pipeline_creation", "blob_cache", "gx_cpu", "capture_integrity"]:
            if key in report:
                print(f"  {key}:", report[key])
    print("PERF percentiles are interval statistics, not frame-time percentiles.")

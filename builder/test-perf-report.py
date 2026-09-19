#!/usr/bin/env python3
"""Check capture selection and the distinction between frame and interval data."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("perf_report", Path(__file__).with_name("perf-report.py"))
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class ReportTests(unittest.TestCase):
    def parse(self, text, start=0, end=None):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "capture.log"
            log.write_text(text, encoding="utf-8")
            return report.summarize(log, start, end)

    def test_selects_matched_interval_and_keeps_metadata(self):
        result = self.parse("""[0] SWEEP affinity main
[1000] PERF fps 1.0 frame 1000.0ms sim 1.0ms submit 2.0ms
[2000] PERF fps 60.0 frame 16.7ms sim 4.0ms submit 3.0ms
[2500] [INFO] cache: PIPELINES skipped_draws 0 over 120 frames/2000ms pending 1
[3000] PERF fps 30.0 frame 33.3ms sim 5.0ms submit 20.0ms
""", 2, 2.9)
        self.assertEqual(result["perf_samples"], 1)
        self.assertEqual(result["interval_statistics"]["fps"]["p50"], 60)
        self.assertEqual(result["zero_skip_windows"], 1)
        self.assertEqual(len(result["metadata"]), 1)
        self.assertNotIn("frame_totals", result)

    def test_frame_totals_weight_duration_without_averaging_quantiles(self):
        result = self.parse("""[2000] FRAMES n 120 span_ms 2000 fps 60 p50_le_ms 16.75 p95_le_ms 17 p99_le_ms 18 worst_ms 18 late60 2 overflow 0
[6000] FRAMES n 80 span_ms 4000 fps 20 p50_le_ms 40 p95_le_ms 50 p99_le_ms -1 worst_ms 1200 late60 80 overflow 1
[6001] STAGES fifo_ms 1.0 texture_ms 2.0
""")
        self.assertAlmostEqual(result["frame_totals"]["fps"], 200/6)
        self.assertEqual(result["frame_totals"]["overflow"], 1)
        self.assertEqual(result["frame_totals"]["worst_ms"], 1200)
        self.assertEqual(result["frame_windows"][1]["p99_le_ms"], -1)
        self.assertNotIn("p99_le_ms", result["frame_totals"])

    def test_legacy_pipeline_format_and_partial_record(self):
        result = self.parse("""[1000] [INFO] cache: PIPELINES skipped_draws/60f 42 pending 10
[2000] PERF fps 30.0 frame 33.3ms
[3000] PERF fps
""")
        self.assertEqual(result["skipped_draws"], 42)
        self.assertEqual(result["perf_samples"], 1)


if __name__ == "__main__":
    unittest.main()

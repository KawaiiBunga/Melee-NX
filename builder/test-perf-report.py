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

    def test_padded_timestamp_and_nul_gap(self):
        result = self.parse("\0\0[   12.500] PERF fps 60 frame 16.7ms\n")
        self.assertEqual(result["perf_samples"], 1)
        self.assertEqual(result["capture_integrity"]["nul_bytes"], 2)

    def test_weighted_cache_diagnostics_and_partial_lines(self):
        result = self.parse("""[2000] BLOBCACHE hits 9 misses 1 lookups 19 avg_lookup_ms 10
[4000] BLOBCACHE hits 1 misses 1 lookups 3 avg_lookup_ms 30
[4100] BLOBCACHE hits 12
[4200] FRAMES n 12
[4300] GXCPU same_pipeline 10 shader_info_hits 9 shader_info_misses 1 over 120 frames
[4400] PIPELINES skipped_draws 0 created 2 avg_create_ms 100
[4500] PIPELINES skipped_draws 0 created 1 avg_create_ms 400
""")
        self.assertAlmostEqual(result["blob_cache"]["weighted_mean_lookup_ms"], 280 / 22)
        self.assertAlmostEqual(result["blob_cache"]["hit_rate"], 10 / 12)
        self.assertEqual(result["pipeline_creation"]["weighted_mean_ms"], 200)
        self.assertEqual(result["gx_cpu"]["shader_info_hit_rate"], .9)
        self.assertEqual(result["capture_integrity"]["partial_or_legacy_blob_records"], 1)
        self.assertNotIn("frame_totals", result)

    def test_worker_cache_and_log_windows(self):
        result = self.parse("""[1000] LOGIO bytes 10 dropped_records 9 errors 0 write_ms 1 flush_ms 2
[2000] FIFOWORK frames 120 process_ms 10 buffer_wait_ms 1 drain_ms 6 grows 1 batches 500 bytes 1234
[3000] FIFOWORK frames 60 process_ms 4 buffer_wait_ms 0 drain_ms 1 grows 2 batches 250 bytes 4321
[3001] RENDERWORK frames 120 begin_ms 1 encode_ms 2 end_ms 3 sync_ms 0 passes 5
[3002] RENDERSTAGES frames 120 acquire_ms 0.1 finalize_ms 0.2 submit_ms 1 present_ms 0.5
[3100] BLOBCACHE hits 10 misses 2 lookups 22 avg_lookup_ms 2 ram_hits 12 sql_reads 10 ram_kb 200 evictions 4 avg_lock_ms 0.1
[3200] LOGIO bytes 100 dropped_records 2 errors 1 write_ms 3 flush_ms 4
""", 2, 4)
        self.assertEqual(result["fifowork"]["weighted_mean_ms"]["process_ms"], 8)
        self.assertEqual(result["fifowork"]["counts"]["grows"], 3)
        self.assertEqual(result["renderstages"]["weighted_mean_ms"]["submit_ms"], 1)
        self.assertEqual(result["blob_cache"]["ram_hit_callbacks"], 12)
        self.assertAlmostEqual(result["blob_cache"]["weighted_mean_lock_ms"], .1)
        self.assertEqual(result["log_io"]["dropped_records"], 2)
        self.assertEqual(result["log_io"]["errors"], 1)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Self-test for builder/prof-report.py: a synthetic profile and nm listing, no console or ELF needed.

  python builder/test-prof-report.py
"""
import runpy
import struct
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
report = runpy.run_path(str(ROOT / 'builder' / 'prof-report.py'))

NM = """\
0000000000001000 0000000000000010 T melee_nx_prof_anchor
0000000000001100 0000000000000100 T foo
0000000000001200 0000000000000040 t foo.constprop.0
0000000000001300 0000000000000020 t foo.cold
0000000000002000 0000000000000080 T bar
0000000000003000 B some_data
0000000000004000 w weak_without_size
"""


class ProfReport(unittest.TestCase):
    def test_parse_nm_keeps_sized_code_symbols_sorted(self):
        names = [s[2] for s in report['parse_nm'](NM)]
        self.assertEqual(names, ['melee_nx_prof_anchor', 'foo', 'foo.constprop.0', 'foo.cold', 'bar'])

    def test_attribution_applies_the_load_bias_and_merges_clones(self):
        symbols = report['parse_nm'](NM)
        bias = 0x80000000  # the NRO loaded here
        samples = [(0, bias + 0x1104), (0, bias + 0x1210), (0, bias + 0x1304), (1, bias + 0x2010),
                   (1, bias + 0x9000)]
        by_role = report['attribute'](samples, symbols, bias)
        self.assertEqual(dict(by_role[0]), {'foo': 3})
        self.assertEqual(dict(by_role[1]), {'bar': 1, '(unknown)': 1})

    def test_read_profile_drops_a_torn_record(self):
        header = struct.pack('<8sQII48s', b'MNXPROF1', 0x80001000, 750, 24, b'test')
        body = struct.pack('<IIQQ', 10, 0, 0x80001104, 0) + struct.pack('<IIQQ', 20, 2, 0x80002010, 0) + b'\1\2\3'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'prof.bin'
            path.write_bytes(header + body)
            info, samples = report['read_profile'](path)
        self.assertEqual(samples, [(0, 0x80001104), (2, 0x80002010)])
        self.assertEqual((info['hz'], info['records'], info['stamp']), (750, 2, 'test'))

    def test_read_profile_keeps_only_the_time_window(self):
        header = struct.pack('<8sQII48s', b'MNXPROF1', 0x80001000, 750, 24, b'test')
        body = b''.join(struct.pack('<IIQQ', t, 1, 0x80001000 + t, 0) for t in (5, 10, 15, 20))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'prof.bin'
            path.write_bytes(header + body)
            info, samples = report['read_profile'](path, 10, 20)
        self.assertEqual(samples, [(1, 0x8000100A), (1, 0x8000100F)])
        self.assertEqual(info['seconds'], 0.02)


if __name__ == '__main__':
    unittest.main()

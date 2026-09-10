# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import sys
import unittest

from cinderx.test_support import passUnless

try:
    # pyre-ignore[21]: can't find _testinternalcapi
    from _testinternalcapi import perf_map_state_teardown, write_perf_map_entry

    _HAVE_PERF_MAP_UTILS = True
except ImportError:
    _HAVE_PERF_MAP_UTILS = False


@passUnless(
    _HAVE_PERF_MAP_UTILS and sys.platform == "linux",
    "perf map utilities are Linux-only and need _testinternalcapi",
)
class TestPerfMapWriting(unittest.TestCase):
    def test_write_perf_map_entry(self) -> None:
        self.assertEqual(write_perf_map_entry(0x1234, 5678, "entry1"), 0)
        self.assertEqual(write_perf_map_entry(0x2345, 6789, "entry2"), 0)
        with open(f"/tmp/perf-{os.getpid()}.map") as f:
            perf_file_contents = f.read()
            self.assertIn("1234 162e entry1", perf_file_contents)
            self.assertIn("2345 1a85 entry2", perf_file_contents)
        perf_map_state_teardown()

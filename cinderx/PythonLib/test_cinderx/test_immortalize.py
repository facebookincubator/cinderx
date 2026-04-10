# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe

import unittest

import cinderx
from cinderx.test_support import passUnless, run_in_subprocess, skip_if_ft


@skip_if_ft("T251571267: Free threading doesn't support heap immortalization")
@passUnless(cinderx.is_initialized(), "Tests immortalization APIs in CinderX")
class ImmortalizeTests(unittest.TestCase):
    def test_default_not_immortal(self) -> None:
        obj = []
        self.assertFalse(cinderx.is_immortal(obj))

    @run_in_subprocess
    def test_is_immortal(self) -> None:
        obj = []
        cinderx.immortalize_heap()
        self.assertTrue(cinderx.is_immortal(obj))

    @run_in_subprocess
    def test_post_immortalize(self) -> None:
        cinderx.immortalize_heap()
        obj = []
        self.assertFalse(cinderx.is_immortal(obj))

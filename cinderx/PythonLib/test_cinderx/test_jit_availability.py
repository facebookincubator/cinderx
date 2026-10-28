# Copyright (c) Meta Platforms, Inc. and affiliates.

import runpy
import sys
import unittest
from unittest.mock import patch

import cinderx.jit
from cinderx.test_support import skip_unless_jit


class JitAvailabilityTests(unittest.TestCase):
    def test_unavailable_without_native_apis(self) -> None:
        with patch.dict(sys.modules, {"cinderjit": None, "_cinderx": None}):
            fallback = runpy.run_path(cinderx.jit.__file__)
        self.assertFalse(fallback["is_available"]())
        self.assertFalse(fallback["is_enabled"]())

    @skip_unless_jit("Requires an enabled JIT")
    def test_available_while_paused(self) -> None:
        self.assertTrue(cinderx.jit.is_available())
        with cinderx.jit.pause():
            self.assertFalse(cinderx.jit.is_enabled())
            self.assertTrue(cinderx.jit.is_available())
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_available())

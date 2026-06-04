# Copyright (c) Meta Platforms, Inc. and affiliates.

import importlib
import unittest

# This is just a quick test to see if CinderX works. It's intended purpose is
# for quick and basic validation of OSS builds.


class CinderXOSSTest(unittest.TestCase):
    def test_import(self) -> None:
        import cinderx  # noqa: F401

        if not cinderx.is_initialized():
            try:
                importlib.import_module("_cinderx")
            except Exception as e:
                print(f"Failed to import _cinderx: {e}")

        self.assertTrue(cinderx.is_initialized())


if __name__ == "__main__":
    unittest.main()

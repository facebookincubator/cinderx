# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest

# This is just a quick test to see if CinderX works. It's intended purpose is
# for quick and basic validation of OSS builds.


class CinderXOSSTest(unittest.TestCase):
    def test_import(self) -> None:
        import cinderx  # noqa: F401

        cinderx.init()
        self.assertTrue(cinderx.is_initialized())


if __name__ == "__main__":
    unittest.main()

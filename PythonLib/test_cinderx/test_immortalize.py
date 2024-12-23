# Copyright (c) Meta Platforms, Inc. and affiliates.

import gc
import unittest
import sys

from test.support.script_helper import assert_python_ok
import cinderx

class ImmortalizeTests(unittest.TestCase):

    def test_not_immortal(self):
        obj = []
        self.assertFalse(cinderx.is_immortal(obj))  # noqa: F821

    def test_is_immortal(self):
        code = """if 1:
            import cinderx
            obj = []
            cinderx.immortalize_heap()
            print(cinderx.is_immortal(obj))
            """
        rc, out, err = assert_python_ok("-c", code)
        self.assertEqual(out.strip(), b"True")

    def test_post_immortalize(self):
        code = """if 1:
            import cinderx
            cinderx.immortalize_heap()
            obj = []
            print(cinderx.is_immortal(obj))
            """
        rc, out, err = assert_python_ok("-c", code)
        self.assertEqual(out.strip(), b"False")


if __name__ == "__main__":
    unittest.main()

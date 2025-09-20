# Copyright (c) Meta Platforms, Inc. and affiliates.

import sys
import unittest

import cinderx.test_support as cinder_support


@unittest.skipUnless(
    sys.version_info >= (3, 12) and sys.version_info < (3, 13), "Python 3.12 only"
)
class Python312Bytecodes(unittest.TestCase, cinder_support.AssertBytecodeContainsMixin):
    def test_LOAD_BUILD_CLASS(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            class TestClass:
                pass

            return TestClass()

        result = x()
        self.assertIsNotNone(result)
        self.assertEqual(result.__class__.__name__, "TestClass")
        self.assertBytecodeContains(x, "LOAD_BUILD_CLASS")


if __name__ == "__main__":
    unittest.main()

# Copyright (c) Meta Platforms, Inc. and affiliates.
import sys
import unittest
from unittest import skipIf

from re import escape

from .common import StaticTestBase


@skipIf(sys.version_info < (3, 12), "New in 3.12")
class TypeParameterTests(StaticTestBase):

    def test_function(self):
        codestr = """
            def f[T, U](x: T, y: U) -> T:
                return x
        """
        with self.in_module(codestr) as mod:
            f = mod.f


if __name__ == "__main__":
    unittest.main()


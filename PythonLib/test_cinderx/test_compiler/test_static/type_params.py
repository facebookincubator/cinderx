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

    def test_class(self):
        codestr = """
            class A[T, U]:
                def __init__(self, a: U):
                    self.a = a

                def f(self, x: T) -> U:
                  return self.a
        """
        with self.assertRaisesRegex(NotImplementedError, "Type params"):
            with self.in_module(codestr) as mod:
                a = mod.A(10)


if __name__ == "__main__":
    unittest.main()


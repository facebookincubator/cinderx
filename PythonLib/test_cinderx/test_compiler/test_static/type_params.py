# Copyright (c) Meta Platforms, Inc. and affiliates.
import sys
import unittest

from unittest import skipIf

from .common import StaticTestBase


@skipIf(sys.version_info < (3, 12), "New in 3.12")
class TypeParameterTests(StaticTestBase):
    def test_function(self):
        codestr = """
            def f[T, U](x: T, y: U) -> T:
                return x
        """
        with self.in_module(codestr) as mod:
            mod.f

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
                mod.A(10)

    def test_type_alias(self):
        codestr = """
            type Collection[T] = list[T] | set[T]
        """
        with self.in_module(codestr) as mod:
            a: mod.Collection[int] = [1, 2, 3]  # noqa: F841

    def test_type_alias_as_annotation(self):
        codestr = """
            type Collection[T] = list[T] | set[T]

            def f[T](x: Collection[T], y: T) -> bool:
                return y in x
        """
        with self.in_module(codestr) as mod:
            a: mod.Collection[int] = [1, 2, 3]
            mod.f(a, 1)


if __name__ == "__main__":
    unittest.main()

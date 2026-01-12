# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import sys
import unittest

from cinderx.compiler.static.module_table import ENABLE_IMPLICIT_TYPE_ALIASES
from cinderx.compiler.static.types import TypedSyntaxError
from cinderx.test_support import passIf, passUnless

from .common import StaticTestBase


@passIf(sys.version_info < (3, 12), "New in 3.12")
class TypeAliasTests(StaticTestBase):
    @passUnless(ENABLE_IMPLICIT_TYPE_ALIASES, "Implicit aliases disabled")
    def test_assign(self) -> None:
        codestr = """
            class B: pass

            A = B

            def f(x: A):
                pass
        """
        with self.in_module(codestr) as mod:
            mod.f(mod.B())
            with self.assertRaises(TypeError):
                mod.f("hello")

    def test_alias(self) -> None:
        codestr = """
            type A = int

            def f(x: A):
                pass
        """
        with self.in_module(codestr) as mod:
            mod.f(42)
            with self.assertRaises(TypeError):
                mod.f("hello")

    @passUnless(ENABLE_IMPLICIT_TYPE_ALIASES, "Implicit aliases disabled")
    def test_optional_assign(self) -> None:
        codestr = """
            A = int | None

            def f(x: A):
                pass
        """
        with self.in_module(codestr) as mod:
            mod.f(42)
            mod.f(None)
            with self.assertRaises(TypeError):
                mod.f("hello")

    def test_optional_alias(self) -> None:
        codestr = """
            type A = int | None

            def f(x: A):
                pass
        """
        with self.in_module(codestr) as mod:
            mod.f(42)
            mod.f(None)
            with self.assertRaises(TypeError):
                mod.f("hello")

    def test_transitive_alias(self) -> None:
        codestr = """
            type A = int | None
            type B = A

            def f(x: B):
                pass
        """
        with self.in_module(codestr) as mod:
            mod.f(42)
            mod.f(None)
            with self.assertRaises(TypeError):
                mod.f("hello")

    @passUnless(ENABLE_IMPLICIT_TYPE_ALIASES, "Implicit aliases disabled")
    def test_transitive_assign(self) -> None:
        codestr = """
            A = int | None
            B = A

            def f(x: B):
                pass
        """
        with self.in_module(codestr) as mod:
            mod.f(42)
            mod.f(None)
            with self.assertRaises(TypeError):
                mod.f("hello")

    @passUnless(ENABLE_IMPLICIT_TYPE_ALIASES, "Implicit aliases disabled")
    def test_transitive_alias_and_assign(self) -> None:
        codestr = """
            A = int | None
            type B = A

            def f(x: B):
                pass
        """
        with self.in_module(codestr) as mod:
            mod.f(42)
            mod.f(None)
            with self.assertRaises(TypeError):
                mod.f("hello")

    @passUnless(ENABLE_IMPLICIT_TYPE_ALIASES, "Implicit aliases disabled")
    def test_transitive_alias_in_optional(self) -> None:
        codestr = """
            A = int
            type B = A | None

            def f(x: B):
                pass
        """
        with self.in_module(codestr) as mod:
            mod.f(42)
            mod.f(None)
            with self.assertRaises(TypeError):
                mod.f("hello")

    def test_alias_check_in_module(self) -> None:
        codestr = """
            class B: pass

            type A = B

            def f(x: A):
                pass

            f("hello")
        """
        with self.assertRaises(TypedSyntaxError):
            with self.in_module(codestr):
                pass

    def test_overload(self) -> None:
        codestr = """
            type A = int

            class B:
                def f(self, x: A):
                    pass

            class D(B):
                def f(self, x: int):
                    super().f(x)

            D().f(10)
        """
        with self.in_module(codestr):
            pass

    def test_type_alias_error(self) -> None:
        codestr = """
            type A = 42
        """
        with self.assertRaisesRegex(TypedSyntaxError, "A is not a type.*42"):
            with self.in_module(codestr):
                pass

    def test_assign_to_constructor(self) -> None:
        # Regression test for a crash when calling resolve_type on the rhs of B
        codestr = """
            B = str("<unknown>")
        """
        with self.in_module(codestr):
            pass


if __name__ == "__main__":
    unittest.main()

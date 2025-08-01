# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

from __static__ import is_static_callable, is_static_module

from .common import StaticTestBase


class ReflectionTests(StaticTestBase):
    def test_is_static_module_plain(self) -> None:
        code = """
        def f(x: int) -> int:
            return x + 1
        """

        with self.in_module(code) as mod:
            self.assertFalse(is_static_module(mod))

    def test_is_static_module_not_static(self) -> None:
        code = """
        def f(x: int) -> int:
            return x + 1
        """

        with self.in_strict_module(code) as mod:
            self.assertFalse(is_static_module(mod))

    def test_is_static_module_not_strict(self) -> None:
        code = """
        import __static__
        def f(x: int) -> int:
            return x + 1
        """

        with self.in_module(code) as mod:
            self.assertFalse(is_static_module(mod))

    def test_is_static_module(self) -> None:
        code = """
        import __static__
        def f(x: int) -> int:
            return x + 1
        """

        with self.in_strict_module(code) as mod:
            self.assertTrue(is_static_module(mod))

    def test_is_static_callable(self) -> None:
        def f(x: int) -> int:
            return x + 1

        self.assertFalse(is_static_callable(f))

        codestr = """
        def f(x: int) -> int:
            return x + 1
        """

        with self.in_strict_module(codestr) as mod:
            self.assertTrue(is_static_callable(mod.f))

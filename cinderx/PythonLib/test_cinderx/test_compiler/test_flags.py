# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe
import __future__

import unittest

from cinderx.compiler import consts
from cinderx.compiler.consts import (
    CO_ASYNC_GENERATOR,
    CO_COROUTINE,
    CO_GENERATOR,
    CO_NEWLOCALS,
    CO_OPTIMIZED,
)

from .common import CompilerTest


# For nicer error reporting
FLAGS = {
    getattr(consts, x): x
    for x in (
        "CO_ASYNC_GENERATOR",
        "CO_COROUTINE",
        "CO_GENERATOR",
        "CO_NESTED",
        "CO_NEWLOCALS",
        "CO_NOFREE",
        "CO_OPTIMIZED",
    )
}


def show_flags(x: int) -> str:
    flags = [name for f, name in FLAGS.items() if x & f]
    return " | ".join(flags)


class FlagTests(CompilerTest):
    def assertFlags(self, f, expected):
        try:
            code = f.__code__
        except AttributeError:
            code = self.find_code(f)
        actual = code.co_flags
        if actual != expected:
            self.assertEqual(show_flags(actual), show_flags(expected))

    def test_future_no_longer_relevant(self) -> None:
        f = self.run_code(
            """
        from __future__ import print_function
        def f(): pass"""
        )["f"]
        self.assertFlags(f, CO_OPTIMIZED | CO_NEWLOCALS)

    def test_future_gen_stop(self) -> None:
        f = self.run_code(
            """
        from __future__ import generator_stop
        def f(): pass"""
        )["f"]
        self.assertFlags(f, CO_OPTIMIZED | CO_NEWLOCALS)

    def test_future_barry_as_bdfl(self) -> None:
        f = self.run_code(
            """
        from __future__ import barry_as_FLUFL
        def f(): pass"""
        )["f"]
        # pyre-ignore[16]: Pyre doesn't recognize this flag.
        barry_bdfl = __future__.CO_FUTURE_BARRY_AS_BDFL
        self.assertEqual(
            f.__code__.co_flags,
            barry_bdfl | CO_OPTIMIZED | CO_NEWLOCALS,
        )

    def test_braces(self) -> None:
        with self.assertRaisesRegex(SyntaxError, "not a chance"):
            self.run_code(
                """
            from __future__ import braces
            def f(): pass"""
            )

    def test_gen_func(self) -> None:
        f = self.run_code("def f(): yield")["f"]
        self.assertFlags(f, CO_OPTIMIZED | CO_NEWLOCALS | CO_GENERATOR)

    def test_async_gen_func(self) -> None:
        f = self.run_code(
            """
        async def f():
            yield
            await foo"""
        )["f"]
        self.assertFlags(f, CO_OPTIMIZED | CO_NEWLOCALS | CO_ASYNC_GENERATOR)

    def test_gen_func_yield_from(self) -> None:
        f = self.run_code("def f(): yield from (1, 2, 3)")["f"]
        self.assertFlags(f, CO_OPTIMIZED | CO_NEWLOCALS | CO_GENERATOR)

    def test_class(self) -> None:
        f = self.compile("class C: pass")
        self.assertFlags(f, 0)

    def test_coroutine(self) -> None:
        f = self.compile("async def f(): pass")
        self.assertFlags(f, CO_OPTIMIZED | CO_NEWLOCALS | CO_COROUTINE)

    def test_coroutine_await(self) -> None:
        f = self.compile("async def f(): await foo")
        self.assertFlags(f, CO_OPTIMIZED | CO_NEWLOCALS | CO_COROUTINE)

    def test_free_vars(self) -> None:
        f = self.compile(
            """
        def g():
            x = 2
            def f():
                return x"""
        )
        self.assertFlags(f, CO_OPTIMIZED | CO_NEWLOCALS)


if __name__ == "__main__":
    unittest.main()

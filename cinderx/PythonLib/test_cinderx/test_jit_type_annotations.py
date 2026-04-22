# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import unittest
from typing import Any, Generator, Self

import cinderx
import cinderx.jit
from cinderx.test_support import passUnless


@passUnless(cinderx.jit.is_enabled(), "JIT required")
class TypeAnnotationTests(unittest.TestCase):
    class Box:
        def __init__(self, value: int) -> None:
            self.value = value

        def __add__(self, other: int) -> Self:
            return self.__class__(self.value + other)

        def __eq__(self, other: object) -> bool:
            return isinstance(other, self.__class__) and self.value == other.value

    def setUp(self) -> None:
        cinderx.jit.enable_emit_type_annotation_guards()

    def tearDown(self) -> None:
        cinderx.jit.disable_emit_type_annotation_guards()

    def test_good_simple(self) -> None:
        def f(x: int) -> int:
            return x + 1

        cinderx.jit.force_compile(f)

        self.assertEqual(f(42), 43)
        # pyrefly: ignore [bad-argument-type]
        self.assertIn("LongBinaryOp", cinderx.jit.get_function_hir_opcode_counts(f))

    def test_good_long_list(self) -> None:
        def f(
            x1: int, x2: int, x3: int, x4: int, x5: int, x6: int, x7: int, x8: int
        ) -> int:
            return x1 + x2 + x3 + x4 + x5 + x6 + x7 + x8

        cinderx.jit.force_compile(f)

        self.assertEqual(f(1, 2, 3, 4, 5, 6, 7, 8), 36)

    def test_good_closure(self) -> None:
        def f(x: int) -> int:
            def g(y: int) -> int:
                return x + y

            return g(1)

        cinderx.jit.force_compile(f)

        self.assertEqual(f(42), 43)

    def test_good_generator(self) -> None:
        def f(x: int) -> int:
            def g(y: int) -> Generator[int, Any, Any]:
                yield x + y

            return next(g(1))

        cinderx.jit.force_compile(f)

        self.assertEqual(f(42), 43)

    def test_bad(self) -> None:
        def f(x: int) -> int:
            return x + 1

        cinderx.jit.force_compile(f)

        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(f(self.Box(42)), self.Box(43))

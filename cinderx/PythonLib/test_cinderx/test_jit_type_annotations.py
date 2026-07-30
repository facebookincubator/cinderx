# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import unittest
from collections.abc import Generator
from typing import Self

import cinderx.jit
from cinderx.test_support import FREE_THREADING_BUILD


class Box:
    """
    Value that will act like an int, but isn't one.
    """

    def __init__(self, value: int) -> None:
        self.value = value

    def __add__(self, other: int) -> Self:
        return self.__class__(self.value + other)

    def __eq__(self, other: object) -> bool:
        return isinstance(other, self.__class__) and self.value == other.value


class TypeAnnotationTests(unittest.TestCase):
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
            def g(y: int) -> Generator[int, None, None]:
                yield x + y

            return next(g(1))

        cinderx.jit.force_compile(f)

        self.assertEqual(f(42), 43)

    def test_bad(self) -> None:
        def f(x: int) -> int:
            return x + 1

        cinderx.jit.force_compile(f)

        # Intentionally using the wrong type here.
        #
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(f(Box(42)), Box(43))

    def test_good_generic_alias(self) -> None:
        """
        The `list[int]` annotation should be reduced to `list` and emit a
        GuardType in the HIR.
        """

        def f(x: list[int]) -> int:
            return len(x)

        cinderx.jit.force_compile(f)

        self.assertEqual(f([1, 2, 3]), 3)

        # Skipped on free-threaded builds because the downstream list-specific
        # lowerings are gated off there, and thus leads GuardTypeRemoval to
        # drop the guard.
        if not FREE_THREADING_BUILD:
            opcode_counts = cinderx.jit.get_function_hir_opcode_counts(f)
            assert opcode_counts is not None
            self.assertIn("GuardType", opcode_counts)

    def test_bad_generic_alias(self) -> None:
        """
        Test that a generic type annotation properly deopts to the interpreter
        when passed an incorrectly typed argument.
        """

        def f(x: list[int]) -> int:
            return len(x)

        cinderx.jit.force_compile(f)

        # Intentionally using the wrong type here.
        #
        # pyrefly: ignore [bad-argument-type]
        self.assertEqual(f((1, 2, 3)), 3)

    def test_bad_generic_alias_type_variable(self) -> None:
        """
        Test what happens when a generic type annotation is passed the correct
        base type, but an incorrect type variable.

        Currently this does the wrong thing, because we don't guard against
        list element types being correct.
        """

        def f(x: list[int]) -> int:
            return len(x)

        cinderx.jit.force_compile(f)

        cinderx.jit.get_and_clear_runtime_stats()

        # Intentionally using the wrong type here.
        #
        # pyrefly: ignore [bad-argument-type]
        result = f([1, 2.0, 3])
        deopts = cinderx.jit.get_and_clear_runtime_stats()["deopt"]
        assert isinstance(deopts, list)

        self.assertEqual(result, 3)

        # Right now nothing checks `list` element types, so we don't expect this to
        # deopt.
        self.assertEqual(len(deopts), 0)

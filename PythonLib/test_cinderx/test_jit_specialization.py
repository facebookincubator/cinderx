# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import dis
import sys
import unittest

import cinderx

cinderx.init()

from typing import Callable, TypeVar

import cinderx.jit


TCallableRet = TypeVar("TCallableRet")


_all_opnames: list[str] = dis.opname
if hasattr(dis, "_specialized_instructions"):
    _specialized_indices: list[int] = [
        index for index, name in enumerate(_all_opnames) if name.startswith("<")
    ]

    # pyre-ignore
    for index, name in zip(_specialized_indices, dis._specialized_instructions):
        _all_opnames[index] = name


# Disassemble the given function and return the output as a string. This is a
# relatively hacky way to ensure that the specializing interpreter has run,
# because we compare this output against a specific instruction name to ensure
# it no longer contains the original.
def opnames(func: Callable[..., TCallableRet]) -> list[str]:
    bytecode = dis.Bytecode(func, adaptive=True)  # pyre-ignore
    return [_all_opnames[insn.opcode] for insn in bytecode]


# Run the given function a certain number of times to ensure the specializing
# interpreter kicks in. Then compile it with cinder.
def specialize(
    func: Callable[..., TCallableRet], callable: Callable[[], TCallableRet]
) -> None:
    cinderx.jit.force_uncompile(func)
    cinderx.jit.jit_suppress(func)

    for _ in range(5):
        callable()

    cinderx.jit.jit_unsuppress(func)
    cinderx.jit.force_compile(func)


@unittest.skipIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
@unittest.skipIf(sys.version_info < (3, 12), "Requires the specializing interpreter")
class SpecializationTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.enable_specialized_opcodes()

    def tearDown(self) -> None:
        cinderx.jit.disable_specialized_opcodes()

    def test_binary_op_add_int(self) -> None:
        def f(a: int, b: int) -> int:
            return a + b

        specialize(f, lambda: f(1, 2))

        self.assertNotIn("BINARY_OP", opnames(f))
        self.assertIn("BINARY_OP_ADD_INT", opnames(f))
        self.assertEqual(f(2, 3), 5)

    def test_binary_op_subtract_int(self) -> None:
        def f(a: int, b: int) -> int:
            return a - b

        specialize(f, lambda: f(2, 1))

        self.assertNotIn("BINARY_OP", opnames(f))
        self.assertIn("BINARY_OP_SUBTRACT_INT", opnames(f))
        self.assertEqual(f(5, 2), 3)

    def test_binary_op_multiply_int(self) -> None:
        def f(a: int, b: int) -> int:
            return a * b

        specialize(f, lambda: f(2, 3))

        self.assertNotIn("BINARY_OP", opnames(f))
        self.assertIn("BINARY_OP_MULTIPLY_INT", opnames(f))
        self.assertEqual(f(5, 2), 10)

    def test_binary_op_add_float(self) -> None:
        def f(a: float, b: float) -> float:
            return a + b

        specialize(f, lambda: f(1.5, 2.5))

        self.assertNotIn("BINARY_OP", opnames(f))
        self.assertIn("BINARY_OP_ADD_FLOAT", opnames(f))
        self.assertEqual(f(2.5, 3.5), 6.0)

    def test_binary_op_subtract_float(self) -> None:
        def f(a: float, b: float) -> float:
            return a - b

        specialize(f, lambda: f(2.5, 1.5))

        self.assertNotIn("BINARY_OP", opnames(f))
        self.assertIn("BINARY_OP_SUBTRACT_FLOAT", opnames(f))
        self.assertEqual(f(5.5, 2.5), 3.0)

    def test_binary_op_multiply_float(self) -> None:
        def f(a: float, b: float) -> float:
            return a * b

        specialize(f, lambda: f(2.5, 3.5))

        self.assertNotIn("BINARY_OP", opnames(f))
        self.assertIn("BINARY_OP_MULTIPLY_FLOAT", opnames(f))
        self.assertEqual(f(5.5, 2.5), 13.75)

    def test_binary_op_add_unicode(self) -> None:
        def f(a: str, b: str) -> str:
            return a + b

        specialize(f, lambda: f("a", "b"))

        self.assertNotIn("BINARY_OP", opnames(f))
        self.assertIn("BINARY_OP_ADD_UNICODE", opnames(f))
        self.assertEqual(f("c", "d"), "cd")

    def test_binary_subscr_dict(self) -> None:
        def f(a: dict[str, str], b: str) -> str:
            return a[b]

        specialize(f, lambda: f({"a": "b"}, "a"))

        self.assertNotIn("BINARY_SUBSCR", opnames(f))
        self.assertIn("BINARY_SUBSCR_DICT", opnames(f))
        self.assertEqual(f({"c": "d"}, "c"), "d")

    def test_binary_subscr_list_int(self) -> None:
        def f(a: list[str], b: int) -> str:
            return a[b]

        specialize(f, lambda: f(["a", "b"], 0))

        self.assertNotIn("BINARY_SUBSCR", opnames(f))
        self.assertIn("BINARY_SUBSCR_LIST_INT", opnames(f))
        self.assertEqual(f(["c", "d"], 0), "c")

    def test_binary_subscr_tuple_int(self) -> None:
        def f(a: tuple[str, str], b: int) -> str:
            return a[b]

        specialize(f, lambda: f(("a", "b"), 0))

        self.assertNotIn("BINARY_SUBSCR", opnames(f))
        self.assertIn("BINARY_SUBSCR_TUPLE_INT", opnames(f))
        self.assertEqual(f(("c", "d"), 0), "c")

    def test_compare_op_float(self) -> None:
        def f(a: float, b: float) -> bool:
            return a < b

        specialize(f, lambda: f(1.5, 2.5))

        self.assertNotIn("COMPARE_OP", opnames(f))
        self.assertIn("COMPARE_OP_FLOAT", opnames(f))
        self.assertEqual(f(2.5, 3.5), True)

    def test_compare_op_int(self) -> None:
        def f(a: int, b: int) -> bool:
            return a < b

        specialize(f, lambda: f(1, 2))

        self.assertNotIn("COMPARE_OP", opnames(f))
        self.assertIn("COMPARE_OP_INT", opnames(f))
        self.assertEqual(f(2, 3), True)

    def test_compare_op_str(self) -> None:
        def f(a: str, b: str) -> bool:
            return a == b

        specialize(f, lambda: f("a", "a"))

        self.assertNotIn("COMPARE_OP", opnames(f))
        self.assertIn("COMPARE_OP_STR", opnames(f))
        self.assertEqual(f("b", "b"), True)

    def test_unpack_sequence_list(self) -> None:
        def f(li: list[str]) -> str:
            (a, _b) = li
            return a

        specialize(f, lambda: f(["a", "b"]))

        self.assertNotIn("UNPACK_SEQUENCE", opnames(f))
        self.assertIn("UNPACK_SEQUENCE_LIST", opnames(f))
        self.assertEqual(f(["c", "d"]), "c")

    def test_unpack_sequence_tuple(self) -> None:
        def f(li: tuple[str, str, str]) -> str:
            (a, _b, _c) = li
            return a

        specialize(f, lambda: f(("a", "b", "c")))

        self.assertNotIn("UNPACK_SEQUENCE", opnames(f))
        self.assertIn("UNPACK_SEQUENCE_TUPLE", opnames(f))
        self.assertEqual(f(("c", "d", "e")), "c")

    def test_unpack_sequence_two_tuple(self) -> None:
        def f(li: tuple[str, str]) -> str:
            (a, _b) = li
            return a

        specialize(f, lambda: f(("a", "b")))

        self.assertNotIn("UNPACK_SEQUENCE", opnames(f))
        self.assertIn("UNPACK_SEQUENCE_TWO_TUPLE", opnames(f))
        self.assertEqual(f(("c", "d")), "c")

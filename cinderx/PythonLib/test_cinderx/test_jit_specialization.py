# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import dis
import sys
import unittest
from types import ModuleType
from typing import Callable, TypeVar

import cinderx
import cinderx.jit
from cinderx.test_support import passIf, passUnless


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
    # pyre-fixme[28]: Unexpected keyword argument `adaptive`.
    bytecode = dis.Bytecode(func, adaptive=True)
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


@passIf(not cinderx.jit.is_enabled(), "Tests functionality on the JIT")
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

        if sys.version_info >= (3, 14):
            self.assertNotIn("BINARY_OP", opnames(f))
            self.assertIn("BINARY_OP_SUBSCR_DICT", opnames(f))
        else:
            self.assertNotIn("BINARY_SUBSCR", opnames(f))
            self.assertIn("BINARY_SUBSCR_DICT", opnames(f))
        self.assertEqual(f({"c": "d"}, "c"), "d")

    def test_binary_subscr_list_int(self) -> None:
        def f(a: list[str], b: int) -> str:
            return a[b]

        specialize(f, lambda: f(["a", "b"], 0))

        if sys.version_info >= (3, 14):
            self.assertNotIn("BINARY_OP", opnames(f))
            self.assertIn("BINARY_OP_SUBSCR_LIST_INT", opnames(f))
        else:
            self.assertNotIn("BINARY_SUBSCR", opnames(f))
            self.assertIn("BINARY_SUBSCR_LIST_INT", opnames(f))
        self.assertEqual(f(["c", "d"], 0), "c")

    def test_binary_subscr_tuple_int(self) -> None:
        def f(a: tuple[str, str], b: int) -> str:
            return a[b]

        specialize(f, lambda: f(("a", "b"), 0))

        if sys.version_info >= (3, 14):
            self.assertNotIn("BINARY_OP", opnames(f))
            self.assertIn("BINARY_OP_SUBSCR_TUPLE_INT", opnames(f))
        else:
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

    def test_compare_op_float_comparisons(self) -> None:
        import operator

        nan = float("nan")
        # Ordered, equal, and NaN inputs. Every ordering comparison involving a
        # NaN must be False, `nan == nan` must be False, and `nan != nan` must be
        # True, matching CPython.
        cases = [
            (2.5, 3.5),
            (3.5, 2.5),
            (2.5, 2.5),
            (-1.0, 1.0),
            (nan, 2.5),
            (2.5, nan),
            (nan, nan),
        ]

        entries = [
            ("<", operator.lt, lambda a, b: a < b),
            ("<=", operator.le, lambda a, b: a <= b),
            (">", operator.gt, lambda a, b: a > b),
            (">=", operator.ge, lambda a, b: a >= b),
            ("==", operator.eq, lambda a, b: a == b),
            ("!=", operator.ne, lambda a, b: a != b),
        ]
        for opname, ref, f in entries:
            specialize(f, lambda: f(1.5, 2.5))
            self.assertIn("COMPARE_OP_FLOAT", opnames(f), opname)
            for a, b in cases:
                self.assertEqual(f(a, b), ref(a, b), f"{a} {opname} {b}")

    def test_compare_op_float_branch(self) -> None:
        import operator

        nan = float("nan")
        # Same as test_compare_op_float_comparisons, but the comparison feeds an
        # `if` so the backend may fuse it into a conditional branch. The fused
        # branch picks its condition from the comparison opcode, so it must stay
        # NaN-correct and agree with the standalone comparison.
        cases = [
            (2.5, 3.5),
            (3.5, 2.5),
            (2.5, 2.5),
            (-1.0, 1.0),
            (nan, 2.5),
            (2.5, nan),
            (nan, nan),
        ]

        entries = [
            ("<", operator.lt, lambda a, b: True if a < b else False),
            ("<=", operator.le, lambda a, b: True if a <= b else False),
            (">", operator.gt, lambda a, b: True if a > b else False),
            (">=", operator.ge, lambda a, b: True if a >= b else False),
            ("==", operator.eq, lambda a, b: True if a == b else False),
            ("!=", operator.ne, lambda a, b: True if a != b else False),
        ]
        for opname, ref, f in entries:
            specialize(f, lambda: f(1.5, 2.5))
            self.assertIn("COMPARE_OP_FLOAT", opnames(f), opname)
            # Testing COMPARE_OP -> POP_JUMP_IF_FALSE control flow.
            self.assertIn("POP_JUMP_IF_FALSE", opnames(f), opname)
            for a, b in cases:
                self.assertEqual(f(a, b), ref(a, b), f"{a} {opname} {b}")

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

    def test_load_attr_module(self) -> None:
        s: ModuleType = sys

        def f() -> str:
            nonlocal s
            return s.argv[0]

        specialize(f, lambda: f())

        self.assertNotIn("LOAD_ATTR", opnames(f))
        self.assertIn("LOAD_ATTR_MODULE", opnames(f))
        self.assertEqual(f(), sys.argv[0])

    def test_store_subscr_dict(self) -> None:
        def f(a: dict[str, str], b: str, c: str) -> None:
            a[b] = c

        specialize(f, lambda: f({"a": "b"}, "a", "c"))

        self.assertNotIn("STORE_SUBSCR", opnames(f))
        self.assertIn("STORE_SUBSCR_DICT", opnames(f))

        d = {"a": "b"}
        f(d, "a", "c")
        self.assertEqual(d, {"a": "c"})

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

    @passUnless(sys.version_info >= (3, 14), "TO_BOOL was added in Python 3.13")
    def test_to_bool_bool(self) -> None:
        def f(a: bool) -> str:
            return "y" if a else "n"

        specialize(f, lambda: f(True))

        self.assertNotIn("TO_BOOL", opnames(f))
        self.assertIn("TO_BOOL_BOOL", opnames(f))
        self.assertEqual(f(True), "y")
        self.assertEqual(f(False), "n")

    @passUnless(sys.version_info >= (3, 14), "TO_BOOL was added in Python 3.13")
    def test_to_bool_int(self) -> None:
        def f(a: int) -> str:
            return "y" if a else "n"

        specialize(f, lambda: f(5))

        self.assertNotIn("TO_BOOL", opnames(f))
        self.assertIn("TO_BOOL_INT", opnames(f))
        self.assertEqual(f(5), "y")
        self.assertEqual(f(0), "n")

    @passUnless(sys.version_info >= (3, 14), "TO_BOOL was added in Python 3.13")
    def test_to_bool_list(self) -> None:
        def f(a: list[int]) -> str:
            return "y" if a else "n"

        specialize(f, lambda: f([1]))

        self.assertNotIn("TO_BOOL", opnames(f))
        self.assertIn("TO_BOOL_LIST", opnames(f))
        self.assertEqual(f([1]), "y")
        self.assertEqual(f([]), "n")

    @passUnless(sys.version_info >= (3, 14), "TO_BOOL was added in Python 3.13")
    def test_to_bool_none(self) -> None:
        def f(a: object) -> str:
            return "y" if a else "n"

        specialize(f, lambda: f(None))

        self.assertNotIn("TO_BOOL", opnames(f))
        self.assertIn("TO_BOOL_NONE", opnames(f))
        self.assertEqual(f(None), "n")

    @passUnless(sys.version_info >= (3, 14), "TO_BOOL was added in Python 3.13")
    def test_to_bool_str(self) -> None:
        def f(a: str) -> str:
            return "y" if a else "n"

        specialize(f, lambda: f("x"))

        self.assertNotIn("TO_BOOL", opnames(f))
        self.assertIn("TO_BOOL_STR", opnames(f))
        self.assertEqual(f("x"), "y")
        self.assertEqual(f(""), "n")

    def test_unpack_sequence_two_tuple(self) -> None:
        def f(li: tuple[str, str]) -> str:
            (a, _b) = li
            return a

        specialize(f, lambda: f(("a", "b")))

        self.assertNotIn("UNPACK_SEQUENCE", opnames(f))
        self.assertIn("UNPACK_SEQUENCE_TWO_TUPLE", opnames(f))
        self.assertEqual(f(("c", "d")), "c")

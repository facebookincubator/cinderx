# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import contextlib
import dis
import sys
from types import ModuleType
from typing import Callable, Iterator, TypeVar

import cinderx
import cinderx.jit
from cinderx.test_support import CinderXTestCase, passUnless


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
    bytecode = dis.Bytecode(func, adaptive=True)
    return [_all_opnames[insn.opcode] for insn in bytecode]


# Run the given function a certain number of times to ensure the specializing
# interpreter kicks in. Then compile it with cinder.
def specialize(
    func: Callable[..., TCallableRet], callable: Callable[[], TCallableRet]
) -> None:
    cinderx.jit.force_uncompile(func)

    # Reset adaptive bytecode for code object.  Otherwise it persists across refleak
    # runs and messes with expectations.
    func.__code__ = func.__code__.replace()

    cinderx.jit.jit_suppress(func)

    for _ in range(5):
        callable()

    cinderx.jit.jit_unsuppress(func)
    cinderx.jit.force_compile(func)


# The specialized opcodes under test only run in CinderX's eval loop, which
# needs the frame evaluator installed.
@contextlib.contextmanager
def frame_evaluator() -> Iterator[None]:
    if cinderx.is_frame_evaluator_installed():
        yield
        return

    cinderx.install_frame_evaluator()
    try:
        yield
    finally:
        cinderx.remove_frame_evaluator()


# Like specialize(), but stays interpreted so the interpreter's guards run.
def specialize_interpreted(
    func: Callable[..., TCallableRet], callable: Callable[[], TCallableRet]
) -> None:
    cinderx.jit.jit_suppress(func)

    for _ in range(5):
        callable()


class SpecializationTests(CinderXTestCase):
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

    def test_store_subscr_list_int_bounds(self) -> None:
        def f(a: list[str], b: int, value: str) -> None:
            a[b] = value

        specialize(f, lambda: f(["a", "b", "c"], 0, "x"))

        self.assertNotIn("STORE_SUBSCR", opnames(f))
        self.assertIn("STORE_SUBSCR_LIST_INT", opnames(f))

        seq = ["a", "b", "c"]
        f(seq, -len(seq), "x")
        self.assertEqual(seq, ["x", "b", "c"])
        with self.assertRaises(IndexError):
            f(seq, -len(seq) - 1, "x")

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
        seq = ("c", "d")
        self.assertEqual(f(seq, 0), "c")
        self.assertEqual(f(seq, -len(seq)), "c")
        with self.assertRaises(IndexError):
            f(seq, -len(seq) - 1)

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

    @passUnless(sys.version_info >= (3, 14), "3.12 only builds against Meta Python")
    def test_load_attr_nondescriptor_with_values(self) -> None:
        class C:
            attr: object = "class-attr"

        def f(o: C) -> object:
            return o.attr

        with frame_evaluator():
            specialize_interpreted(f, lambda: f(C()))

            self.assertNotIn("LOAD_ATTR", opnames(f))
            self.assertIn("LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES", opnames(f))

            # Setting the attribute on an instance bumps the shared keys, so the
            # load must deopt instead of returning the cached class attribute.
            o = C()
            o.attr = "instance-attr"
            self.assertEqual(f(o), "instance-attr")

    @passUnless(sys.version_info >= (3, 14), "3.12 only builds against Meta Python")
    def test_load_attr_method_with_values(self) -> None:
        class C:
            def m(self) -> str:
                return "class-method"

        def f(o: C) -> str:
            return o.m()

        with frame_evaluator():
            specialize_interpreted(f, lambda: f(C()))

            self.assertNotIn("LOAD_ATTR", opnames(f))
            self.assertIn("LOAD_ATTR_METHOD_WITH_VALUES", opnames(f))

            o = C()
            setattr(o, "m", lambda: "instance-attr")
            self.assertEqual(f(o), "instance-attr")

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
    def test_to_bool_bool_deopt(self) -> None:
        def f(a: object) -> str:
            return "y" if a else "n"

        specialize(f, lambda: f(True))

        self.assertNotIn("TO_BOOL", opnames(f))
        self.assertIn("TO_BOOL_BOOL", opnames(f))

        # TO_BOOL_BOOL keeps its operand as the result, so the guard is the
        # only thing stopping a non-bool from being compared against Py_True.
        self.assertEqual(f((1, 2)), "y")
        self.assertEqual(f(()), "n")
        self.assertEqual(f(5), "y")
        self.assertEqual(f(""), "n")

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

    def test_for_iter_range_unused_loop_variable_is_unboxed(self) -> None:
        def count(n: int) -> int:
            result = 0
            for _ in range(n):
                result += 1
            return result

        # Warm up so FOR_ITER specializes to FOR_ITER_RANGE before compiling.
        cinderx.jit.jit_suppress(count)
        for _ in range(100):
            count(10)
        cinderx.jit.jit_unsuppress(count)

        # Not perfect, but it tells us we're likely using unboxed integers.
        self.assertHIROpcodes(
            count,
            present=["GuardType", "IntBinaryOp"],
            absent=["PrimitiveBox"],
        )

        self.assertEqual(count(100), 100)

    def test_for_iter_range_reused_loop_variable(self) -> None:
        """
        Distilled from the nbody benchmark, this hit issues in the JIT with
        trying to merge boxed ints, unboxed ints, and nullptr.
        """

        class Value:
            __slots__ = ("value",)

            def __init__(self, value: float) -> None:
                self.value = value

        def update(values: list[Value], size: int) -> None:
            for i in range(size):
                current = values[i]
                current_value = current.value
                for j in range(i + 1, size):
                    other = values[j]
                    delta = current_value - other.value
                    current_value -= delta
                    other.value += delta
                current.value = current_value
            for i in range(size):
                current = values[i]
                current.value += 1.0

        warm_values = [Value(1.0), Value(2.0), Value(3.0)]
        cinderx.jit.jit_suppress(update)
        for _ in range(100):
            update(warm_values, len(warm_values))
        cinderx.jit.jit_unsuppress(update)

        self.assertTrue(cinderx.jit.force_compile(update))
        values = [Value(1.0), Value(2.0), Value(3.0)]
        update(values, len(values))
        self.assertEqual([value.value for value in values], [4.0, 3.0, 2.0])

    def test_for_iter_range_back_to_back_loops(self) -> None:
        # The empty-range check adds an edge that skips the loop entirely, so
        # the second loop's setup Snapshots must not keep naming the first
        # loop's guarded iterator: it has no definition on that edge.  The
        # empty cases below exercise the skip edge of each loop.
        def sums(n: int, m: int) -> int:
            s = 0
            for i in range(n):
                s += i
            for j in range(m):
                s += j
            return s

        # Warm up so FOR_ITER specializes to FOR_ITER_RANGE before compiling.
        cinderx.jit.jit_suppress(sums)
        for _ in range(100):
            sums(5, 7)
        cinderx.jit.jit_unsuppress(sums)

        self.assertTrue(cinderx.jit.force_compile(sums))
        self.assertEqual(sums(5, 7), 31)
        self.assertEqual(sums(0, 3), 3)
        self.assertEqual(sums(3, 0), 3)
        self.assertEqual(sums(0, 0), 0)

    def test_for_iter_range_shared_produce_block(self) -> None:
        """
        Test deopting in the middle of a FOR_ITER_RANGE.
        """

        class ForceDeopt:
            def __radd__(self, other: object) -> int:
                return 100

        def count(n: int, values: list[int | ForceDeopt]) -> int:
            result = 0
            for _ in range(n):
                result += values.pop(0)
            return result

        cinderx.jit.jit_suppress(count)
        for _ in range(100):
            count(3, [1, 1, 1])
        cinderx.jit.jit_unsuppress(count)

        self.assertTrue(cinderx.jit.force_compile(count))
        self.assertEqual(count(3, [1, 1, 1]), 3)
        self.assertEqual(count(2, [1, ForceDeopt()]), 100)

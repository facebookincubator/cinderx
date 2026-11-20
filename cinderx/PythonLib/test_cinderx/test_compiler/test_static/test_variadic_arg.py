# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import gc
import sys
from itertools import product
from types import FunctionType

import cinderx.jit
from cinderx.compiler.errors import TypedSyntaxError
from cinderx.test_support import no_background_compile

from .common import StaticTestBase


_REENTRANT_LOOKUP_SOURCE = """
calls = []

def target(prefix, value{default}):
    calls.append((prefix, value))
    return value

def caller(mapping):
    keepalive = [42]
    result = target("prefix", **mapping)
    return result, keepalive
"""


class VariadicArgTests(StaticTestBase):
    def test_load_iterable_arg(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float, e: float) -> int:
            return 7

        def y() -> int:
            p = ("hi", 0.1, 0.2)
            return x(1, 3, *p)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 0)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 1)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 2)
        self.assertNotInBytecode(y, "LOAD_ITERABLE_ARG", 3)
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertEqual(y_callable(), 7)

    def test_load_iterable_arg_default_overridden(self) -> None:
        codestr = """
            def x(a: int, b: int, c: str, d: float = 10.1, e: float = 20.1) -> bool:
                return bool(
                    a == 1
                    and b == 3
                    and c == "hi"
                    and d == 0.1
                    and e == 0.2
                )

            def y() -> bool:
                p = ("hi", 0.1, 0.2)
                return x(1, 3, *p)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertNotInBytecode(y, "LOAD_ITERABLE_ARG", 3)
        self.assertNotInBytecode(y, "LOAD_MAPPING_ARG", 3)
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertTrue(y_callable())

    def test_load_iterable_arg_multi_star(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float, e: float) -> int:
            return 7

        def y() -> int:
            p = (1, 3)
            q = ("hi", 0.1, 0.2)
            return x(*p, *q)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        # we should fallback to the normal Python compiler for this
        self.assertNotInBytecode(y, "LOAD_ITERABLE_ARG")
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertEqual(y_callable(), 7)

    def test_load_iterable_arg_star_not_last(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float, e: float) -> int:
            return 7

        def y() -> int:
            p = (1, 3, 'abc', 0.1)
            return x(*p, 1.0)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        # we should fallback to the normal Python compiler for this
        self.assertNotInBytecode(y, "LOAD_ITERABLE_ARG")
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertEqual(y_callable(), 7)

    def test_load_iterable_arg_failure(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float, e: float) -> int:
            return 7

        def y() -> int:
            p = ("hi", 0.1)
            return x(1, 3, *p)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 0)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 1)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 2)
        self.assertNotInBytecode(y, "LOAD_ITERABLE_ARG", 3)
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            with self.assertRaises(IndexError):
                y_callable()

    def test_load_iterable_arg_sequence(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float, e: float) -> int:
            return 7

        def y() -> int:
            p = ["hi", 0.1, 0.2]
            return x(1, 3, *p)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 0)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 1)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 2)
        self.assertNotInBytecode(y, "LOAD_ITERABLE_ARG", 3)
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertEqual(y_callable(), 7)

    def test_load_iterable_arg_sequence_1(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float, e: float) -> int:
            return 7

        def gen():
            for i in ["hi", 0.05, 0.2]:
                yield i

        def y() -> int:
            g = gen()
            return x(1, 3, *g)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 0)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 1)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 2)
        self.assertNotInBytecode(y, "LOAD_ITERABLE_ARG", 3)
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertEqual(y_callable(), 7)

    def test_load_iterable_arg_sequence_failure(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float, e: float) -> int:
            return 7

        def y() -> int:
            p = ["hi", 0.1]
            return x(1, 3, *p)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 0)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 1)
        self.assertInBytecode(y, "LOAD_ITERABLE_ARG", 2)
        self.assertNotInBytecode(y, "LOAD_ITERABLE_ARG", 3)
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            with self.assertRaises(IndexError):
                y_callable()

    def test_load_mapping_arg_jit(self) -> None:
        codestr = """
        def target(required, optional=None):
            return required, optional

        def caller(mapping):
            return target(**mapping)
        """
        with self.in_module(codestr) as mod:
            caller = mod.caller
            self.assertInBytecode(caller, "LOAD_MAPPING_ARG", 2)
            self.assertInBytecode(caller, "LOAD_MAPPING_ARG", 3)
            value = object()
            self.assertEqual(caller({"required": value}), (value, None))
            self.assert_jitted(caller)
            self.assertEqual(
                caller({"required": None, "optional": value}), (None, value)
            )
            with self.assertRaisesRegex(TypeError, "missing argument required"):
                caller({})
            with self.assertRaisesRegex(
                TypeError, r"argument after \*\* must be a dict, not list"
            ):
                caller([])

    def test_load_mapping_arg_jit_dict_subclass(self) -> None:
        codestr = """
        def target(value):
            return value

        def caller(mapping):
            return target(**mapping)
        """

        class DictSubclass(dict):
            def __getitem__(self, key: object) -> object:
                raise AssertionError("must use dict lookup, not __getitem__")

        with self.in_module(codestr) as mod:
            caller = mod.caller
            value = object()
            self.assertIs(caller(DictSubclass(value=value)), value)
            self.assert_jitted(caller)

    def test_load_mapping_arg_jit_lookup_error(self) -> None:
        codestr = """
        def target(value=None):
            return value

        def caller(mapping):
            return target(**mapping)
        """

        class Key:
            def __hash__(self) -> int:
                return hash("value")

            def __eq__(self, other: object) -> bool:
                raise RuntimeError("lookup failed")

        with self.in_module(codestr) as mod:
            caller = mod.caller
            with self.assertRaisesRegex(RuntimeError, "lookup failed"):
                caller({Key(): 42})
            self.assert_jitted(caller)

    def test_load_mapping_arg_jit_reentrant_frame(self) -> None:
        codestr = """
        def target(value):
            return value

        def caller(mapping):
            marker = 42
            result = target(**mapping)
            return result, marker, mapping
        """
        test = self
        mapping: dict[object, object] = {}

        class Key:
            def __hash__(self) -> int:
                return hash("value")

            def __eq__(self, other: object) -> bool:
                gc.collect()
                cinderx.jit.disable(deopt_all=True)
                frame = sys._getframe(1)
                test.assertEqual(frame.f_code.co_name, "caller")
                return other == "value"

        mapping[Key()] = "found"
        with self.in_module(codestr) as mod:
            caller = mod.caller
            self.assertEqual(
                caller({"value": "warmup"}), ("warmup", 42, {"value": "warmup"})
            )
            self.assert_jitted(caller)
            jit_enabled = cinderx.jit.is_enabled()
            try:
                self.assertEqual(caller(mapping), ("found", 42, mapping))
            finally:
                if jit_enabled:
                    cinderx.jit.enable()

    def _assert_lookup_traceback(self, error: Exception, caller: FunctionType) -> None:
        tb = error.__traceback__
        while tb is not None:
            if tb.tb_frame.f_code.co_name == caller.__name__:
                self.assertEqual(tb.tb_lineno, caller.__code__.co_firstlineno + 2)
                return
            tb = tb.tb_next
        self.fail("caller is missing from the lookup exception traceback")

    def _assert_reentrant_warmup(self, invoke: FunctionType, nested: bool) -> None:
        # A background compile of `outer` can run after `outer` has been
        # interpreted once but before `caller` has, and then prunes `caller` as
        # a cold callee.
        with no_background_compile():
            self.assertEqual(invoke({"value": "warmup"}), ("warmup", [42]))
        self.assert_jitted(invoke)
        if (
            nested
            and cinderx.jit.is_enabled()
            and cinderx.jit.get_compile_after_n_calls() == 0
            and cinderx.jit.is_hir_inliner_enabled()
        ):
            self.assertGreater(cinderx.jit.get_num_inlined_functions(invoke), 0)

    def _run_reentrant_lookup(
        self, default: str, mode: str, value: object, nested: bool = False
    ) -> tuple[object, list[object], list[str], tuple[type[Exception], str] | None]:
        comparisons: list[str] = []
        reentrant = False

        class Key:
            def __hash__(self) -> int:
                return hash("value")

            def __eq__(self, other: object) -> bool:
                comparisons.append(str(other))
                if reentrant:
                    gc.collect()
                    cinderx.jit.disable(deopt_all=True)
                if mode == "error":
                    raise RuntimeError("lookup failed")
                return mode == "hit"

        result: object = None
        error_info: tuple[type[Exception], str] | None = None
        source = _REENTRANT_LOOKUP_SOURCE.format(default=default)
        if nested:
            source += "\ndef outer(mapping):\n    return caller(mapping)\n"
            context = self.in_strict_module(source)
        else:
            context = self.in_module(source)
        with context as mod:
            caller = mod.caller
            invoke = mod.outer if nested else caller
            self._assert_reentrant_warmup(invoke, nested)
            comparisons.clear()
            mod.calls.clear()
            reentrant = True
            enabled = cinderx.jit.is_enabled()
            try:
                result, keepalive = invoke({Key(): value})
                self.assertEqual(keepalive, [42])
            except Exception as error:
                self._assert_lookup_traceback(error, caller)
                error_info = type(error), str(error)
            finally:
                if enabled:
                    cinderx.jit.enable()
            calls = mod.calls.copy()
        return result, calls, comparisons, error_info

    def _single_lookup_comparisons(self, mode: str) -> list[str]:
        comparisons: list[str] = []

        class Key:
            def __hash__(self) -> int:
                return hash("value")

            def __eq__(self, other: object) -> bool:
                comparisons.append(str(other))
                if mode == "error":
                    raise RuntimeError("lookup failed")
                return mode == "hit"

        mapping: dict[object, object] = {Key(): None}
        if mode == "error":
            with self.assertRaisesRegex(RuntimeError, "lookup failed"):
                mapping.get("value")
        else:
            mapping.get("value")
        return comparisons

    def test_load_mapping_arg_jit_reentrant_outcomes(self) -> None:
        marker = object()
        cases = (
            ("hit", "", marker, None),
            ("default", "=None", None, None),
            ("default", '= "fallback value"', "fallback value", None),
            ("missing", "", None, (TypeError, "missing argument value")),
            ("error", "=None", None, (RuntimeError, "lookup failed")),
        )
        for nested, (mode, default, expected, error) in product((False, True), cases):
            with self.subTest(mode=mode, default=default, nested=nested):
                result, calls, comparisons, error_info = self._run_reentrant_lookup(
                    default, mode, marker, nested
                )
                # A single dict probe can compare the same key more than once.
                self.assertEqual(comparisons, self._single_lookup_comparisons(mode))
                self.assertEqual(error_info, error)
                if error is None:
                    self.assertEqual(result, expected)
                    self.assertEqual(calls, [("prefix", expected)])
                else:
                    self.assertEqual(calls, [])

    def test_load_mapping_arg_reentrant_exception_handler(self) -> None:
        codestr = """
        def target(value):
            return value

        def caller(mapping):
            try:
                return target(**mapping)
            except TypeError as error:
                return str(error)
        """
        comparisons: list[str] = []

        class Key:
            def __hash__(self) -> int:
                return hash("value")

            def __eq__(self, other: object) -> bool:
                comparisons.append(str(other))
                gc.collect()
                cinderx.jit.disable(deopt_all=True)
                return False

        with self.in_module(codestr) as mod:
            self.assertEqual(mod.caller({"value": "warmup"}), "warmup")
            self.assert_jitted(mod.caller)
            enabled = cinderx.jit.is_enabled()
            try:
                self.assertEqual(mod.caller({Key(): None}), "missing argument value")
            finally:
                if enabled:
                    cinderx.jit.enable()
        self.assertEqual(comparisons, self._single_lookup_comparisons("missing"))

    def test_load_mapping_arg_jit_temporary(self) -> None:
        codestr = """
        class Value:
            pass

        def target(value):
            return value

        def caller():
            return target(**{"value": Value()})
        """
        with self.in_module(codestr) as mod:
            caller = mod.caller
            self.assertIsInstance(caller(), mod.Value)
            self.assert_jitted(caller)

    def test_load_mapping_arg(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float=-0.1, e: float=1.1, f: str="something") -> bool:
            return bool(f == "yo" and d == 1.0 and e == 1.1)

        def y() -> bool:
            d = {"d": 1.0}
            return x(1, 3, "hi", f="yo", **d)
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "LOAD_MAPPING_ARG", 3)
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertTrue(y_callable())

    def test_load_mapping_and_iterable_args_failure_1(self) -> None:
        """
        Fails because we don't supply enough positional args
        """

        codestr = """
        def x(a: int, b: int, c: str, d: float=2.2, e: float=1.1, f: str="something") -> bool:
            return bool(a == 1 and b == 3 and f == "yo" and d == 2.2 and e == 1.1)

        def y() -> bool:
            return x(1, 3, f="yo")
        """
        with self.assertRaisesRegex(
            SyntaxError, "Function foo.x expects a value for argument c"
        ):
            self.compile(codestr, modname="foo")

    def test_load_mapping_arg_failure(self) -> None:
        """
        Fails because we supply an extra kwarg
        """
        codestr = """
        def x(a: int, b: int, c: str, d: float=2.2, e: float=1.1, f: str="something") -> bool:
            return bool(a == 1 and b == 3 and f == "yo" and d == 2.2 and e == 1.1)

        def y() -> bool:
            return x(1, 3, "hi", f="yo", g="lol")
        """
        with self.assertRaisesRegex(
            TypedSyntaxError,
            "Given argument g does not exist in the definition of foo.x",
        ):
            self.compile(codestr, modname="foo")

    def test_load_mapping_arg_custom_class(self) -> None:
        """
        Fails because we supply a custom class for the mapped args, instead of a dict
        """
        codestr = """
        def x(a: int, b: int, c: str="hello") -> bool:
            return bool(a == 1 and b == 3 and c == "hello")

        class C:
            def __getitem__(self, key: str) -> str | None:
                if key == "c":
                    return "hi"

            def keys(self):
                return ["c"]

        def y() -> bool:
            return x(1, 3, **C())
        """
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            with self.assertRaisesRegex(
                TypeError, r"argument after \*\* must be a dict, not C"
            ):
                self.assertTrue(y_callable())

    def test_load_mapping_arg_missing_required(self) -> None:
        codestr = """
        def x(a, b):
            return a, b

        def y(d):
            return x(1, **d)
        """
        with self.in_module(codestr) as mod:
            self.assertInBytecode(mod.y, "LOAD_MAPPING_ARG", 2)
            with self.assertRaisesRegex(TypeError, "missing argument b"):
                mod.y({})

    def test_load_mapping_arg_use_defaults(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float=-0.1, e: float=1.1, f: str="something") -> bool:
            return bool(f == "yo" and d == -0.1 and e == 1.1)

        def y() -> bool:
            d = {"d": 1.0}
            return x(1, 3, "hi", f="yo")
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "LOAD_CONST", 1.1)
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertTrue(y_callable())

    def test_default_arg_non_const(self) -> None:
        codestr = """
        class C: pass
        def x(val=C()) -> C:
            return val

        def f() -> C:
            return x()
        """
        with self.in_module(codestr) as mod:
            f = mod.f
            self.assertInBytecode(f, self.CALL)

    def test_default_arg_non_const_kw_provided(self) -> None:
        codestr = """
        class C: pass
        def x(val:object=C()):
            return val

        def f():
            return x(val=42)
        """

        with self.in_module(codestr) as mod:
            f = mod.f
            self.assertEqual(f(), 42)

    def test_load_mapping_arg_order(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float=-0.1, e: float=1.1, f: str="something") -> bool:
            return bool(
                a == 1
                and b == 3
                and c == "hi"
                and d == 1.1
                and e == 3.3
                and f == "hmm"
            )

        stuff = []
        def q() -> float:
            stuff.append("q")
            return 1.1

        def r() -> float:
            stuff.append("r")
            return 3.3

        def s() -> str:
            stuff.append("s")
            return "hmm"

        def y() -> bool:
            return x(1, 3, "hi", f=s(), d=q(), e=r())
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "STORE_FAST", "_pystatic_.0._tmp__d")
        if sys.version_info >= (3, 16):
            self.assertInBytecode(
                y,
                "LOAD_FAST_BORROW_LOAD_FAST_BORROW",
                ("_pystatic_.0._tmp__d", "_pystatic_.0._tmp__e"),
            )
        elif sys.version_info >= (3, 14):
            self.assertInBytecode(
                y,
                "STORE_FAST_LOAD_FAST",
                ("_pystatic_.0._tmp__e", "_pystatic_.0._tmp__d"),
            )
        else:
            self.assertInBytecode(y, "LOAD_FAST", "_pystatic_.0._tmp__d")
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertTrue(y_callable())
            self.assertEqual(["s", "q", "r"], mod.stuff)

    def test_load_mapping_arg_order_with_variadic_kw_args(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float=-0.1, e: float=1.1, f: str="something", g: str="look-here") -> bool:
            return bool(
                a == 1
                and b == 3
                and c == "hi"
                and d == 1.1
                and e == 3.3
                and f == "hmm"
                and g == "overridden"
            )

        stuff = []
        def q() -> float:
            stuff.append("q")
            return 1.1

        def r() -> float:
            stuff.append("r")
            return 3.3

        def s() -> str:
            stuff.append("s")
            return "hmm"

        def y() -> bool:
            kw = {"g": "overridden"}
            return x(1, 3, "hi", f=s(), **kw, d=q(), e=r())
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertInBytecode(y, "STORE_FAST", "_pystatic_.0._tmp__d")
        if sys.version_info >= (3, 16):
            self.assertInBytecode(
                y,
                "LOAD_FAST_BORROW_LOAD_FAST_BORROW",
                ("_pystatic_.0._tmp__d", "_pystatic_.0._tmp__e"),
            )
        elif sys.version_info >= (3, 14):
            self.assertInBytecode(
                y,
                "STORE_FAST_LOAD_FAST",
                ("_pystatic_.0._tmp__e", "_pystatic_.0._tmp__d"),
            )
        else:
            self.assertInBytecode(y, "LOAD_FAST", "_pystatic_.0._tmp__d")
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertTrue(y_callable())
            self.assertEqual(["s", "q", "r"], mod.stuff)

    def test_load_mapping_arg_order_with_variadic_kw_args_one_positional(self) -> None:
        codestr = """
        def x(a: int, b: int, c: str, d: float=-0.1, e: float=1.1, f: str="something", g: str="look-here") -> bool:
            return bool(
                a == 1
                and b == 3
                and c == "hi"
                and d == 1.1
                and e == 3.3
                and f == "hmm"
                and g == "overridden"
            )

        stuff = []
        def q() -> float:
            stuff.append("q")
            return 1.1

        def r() -> float:
            stuff.append("r")
            return 3.3

        def s() -> str:
            stuff.append("s")
            return "hmm"


        def y() -> bool:
            kw = {"g": "overridden"}
            return x(1, 3, "hi", 1.1, f=s(), **kw, e=r())
        """
        y = self.find_code(self.compile(codestr, modname="foo"), name="y")
        self.assertNotInBytecode(y, "STORE_FAST", "_pystatic_.0._tmp__d")
        self.assertNotInBytecode(y, "LOAD_FAST", "_pystatic_.0._tmp__d")
        with self.in_module(codestr) as mod:
            y_callable = mod.y
            self.assertTrue(y_callable())
            self.assertEqual(["s", "r"], mod.stuff)

    def test_load_mapping_arg_stack_effect(self) -> None:
        codestr = """
        def g(x=None) -> None:
            pass

        def f():
            return [
                g(**{})
                for i in ()
            ]
        """
        with self.in_module(codestr) as mod:
            f = mod.f
            self.assertInBytecode(f, "LOAD_MAPPING_ARG", 3)
            self.assertEqual(f(), [])

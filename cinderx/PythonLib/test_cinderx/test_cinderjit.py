# Copyright (c) Meta Platforms, Inc. and affiliates.

import asyncio
import faulthandler
import gc
import itertools
import shutil
import subprocess
import sys
import tempfile
import textwrap
import traceback
import unittest
import warnings
import weakref

from pathlib import Path

AT_LEAST_312 = sys.version_info[:2] >= (3, 12)

if not AT_LEAST_312:
    import _testcindercapi

import cinderx

cinderx.init()
import cinderx.jit

import cinderx.test_support as cinder_support
from cinderx.compiler.consts import CO_FUTURE_BARRY_AS_BDFL, CO_SUPPRESS_JIT
from cinderx.jit import (
    force_compile,
    force_uncompile,
    is_jit_compiled,
    jit_suppress,
    jit_unsuppress,
)
from cinderx.test_support import (
    CINDERX_PATH,
    compiles_after_one_call,
    ENCODING,
    run_in_subprocess,
    skip_unless_jit,
)

from .common import failUnlessHasOpcodes, with_globals

try:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        from .test_compiler.test_static.common import StaticTestBase
except ImportError:
    from test_compiler.test_static.common import StaticTestBase


class TestException(Exception):
    pass


def firstlineno(func):
    return func.__code__.co_firstlineno


@cinder_support.failUnlessJITCompiled
def get_stack():
    z = 1 + 1  # noqa: F841
    stack = traceback.extract_stack()
    return stack


@cinder_support.failUnlessJITCompiled
def get_stack_twice():
    stacks = []
    stacks.append(get_stack())
    stacks.append(get_stack())
    return stacks


@cinder_support.failUnlessJITCompiled
def get_stack2():
    z = 2 + 2  # noqa: F841
    stack = traceback.extract_stack()
    return stack


@cinder_support.failUnlessJITCompiled
def get_stack_siblings():
    return [get_stack(), get_stack2()]


@cinder_support.failUnlessJITCompiled
def get_stack_multi():
    stacks = []
    stacks.append(traceback.extract_stack())
    z = 1 + 1  # noqa: F841
    stacks.append(traceback.extract_stack())
    return stacks


@cinder_support.failUnlessJITCompiled
def call_get_stack_multi():
    x = 1 + 1  # noqa: F841
    return get_stack_multi()


@cinder_support.failUnlessJITCompiled
def func_to_be_inlined(x, y):
    return x + y


@cinder_support.failUnlessJITCompiled
def func_with_defaults(x=1, y=2):
    return x + y


@cinder_support.failUnlessJITCompiled
def func_with_varargs(x, *args):
    return x


@cinder_support.failUnlessJITCompiled
def func():
    a = func_to_be_inlined(2, 3)
    b = func_with_defaults()
    c = func_with_varargs(1, 2, 3)
    return a + b + c


@cinder_support.failUnlessJITCompiled
def func_with_defaults_that_will_change(x=1, y=2):
    return x + y


@cinder_support.failUnlessJITCompiled
def change_defaults():
    func_with_defaults_that_will_change.__defaults__ = (4, 5)


@cinder_support.failUnlessJITCompiled
def func_that_change_defaults():
    change_defaults()
    return func_with_defaults_that_will_change()


class InlinedFunctionTests(unittest.TestCase):
    @jit_suppress
    @unittest.skipIf(
        not cinderx.jit.is_hir_inliner_enabled(),
        "meaningless without HIR inliner enabled",
    )
    def test_deopt_when_func_defaults_change(self) -> None:
        self.assertEqual(
            cinderx.jit.get_num_inlined_functions(func_that_change_defaults), 2
        )
        self.assertEqual(func_that_change_defaults(), 9)


class InlineCacheStatsTests(unittest.TestCase):
    @jit_suppress
    @unittest.skipIf(
        not cinderx.jit.is_inline_cache_stats_collection_enabled(),
        "meaningless without inline cache stats collection enabled",
    )
    def test_load_method_cache_stats(self) -> None:
        # Clear inline cache stats of any collected data from importing
        # builtin modules
        cinderx.jit.get_and_clear_inline_cache_stats()

        import linecache

        class BinOps:
            def instance_mul(self, x, y):
                return x * y

            @staticmethod
            def mul(x, y):
                return x * y

        @cinder_support.failUnlessJITCompiled
        def trigger_load_method_with_stats():
            a = BinOps()
            a.instance_mul(100, 1)
            a.mul(100, 1)  # This should be a cache miss.

            # This should be a cache miss.
            b = linecache.getline("abc", 123)  # noqa: F841

            return a

        trigger_load_method_with_stats()
        stats = cinderx.jit.get_and_clear_inline_cache_stats()

        load_method_stats = stats["load_method_stats"]
        relevant_load_method_stats = list(
            filter(
                lambda stat: "test_cinderjit" in stat["filename"]
                and stat["method"] == "trigger_load_method_with_stats",
                load_method_stats,
            )
        )
        self.assertTrue(len(relevant_load_method_stats) == 3)
        misses = [cache["cache_misses"] for cache in relevant_load_method_stats]
        load_method_cache_misses = {k: v for miss in misses for k, v in miss.items()}
        self.assertEqual(
            load_method_cache_misses,
            {
                "test_cinderx.test_cinderjit:BinOps.mul": {
                    "count": 1,
                    "reason": "Uncategorized",
                },
                "module.getline": {"count": 1, "reason": "WrongTpGetAttro"},
            },
        )


class InlinedFunctionLineNumberTests(unittest.TestCase):
    @jit_suppress
    @unittest.skipIf(
        not cinderx.jit.is_hir_inliner_enabled(),
        "meaningless without HIR inliner enabled",
    )
    def test_line_numbers_with_sibling_inlined_functions(self) -> None:
        """Verify that line numbers are correct when function calls are inlined in the same
        expression"""
        # Calls to get_stack and get_stack2 should be inlined
        self.assertEqual(cinderx.jit.get_num_inlined_functions(get_stack_siblings), 2)
        stacks = get_stack_siblings()
        # Call to get_stack
        self.assertEqual(stacks[0][-1].lineno, firstlineno(get_stack) + 3)
        self.assertEqual(stacks[0][-2].lineno, firstlineno(get_stack_siblings) + 2)
        # Call to get_stack2
        self.assertEqual(stacks[1][-1].lineno, firstlineno(get_stack2) + 3)
        self.assertEqual(stacks[1][-2].lineno, firstlineno(get_stack_siblings) + 2)

    @jit_suppress
    @unittest.skipIf(
        not cinderx.jit.is_hir_inliner_enabled(),
        "meaningless without HIR inliner enabled",
    )
    def test_line_numbers_at_multiple_points_in_inlined_functions(self) -> None:
        """Verify that line numbers are are correct at different points in an inlined
        function"""
        # Call to get_stack_multi should be inlined
        self.assertEqual(cinderx.jit.get_num_inlined_functions(call_get_stack_multi), 1)
        stacks = call_get_stack_multi()
        self.assertEqual(stacks[0][-1].lineno, firstlineno(get_stack_multi) + 3)
        self.assertEqual(stacks[0][-2].lineno, firstlineno(call_get_stack_multi) + 3)
        self.assertEqual(stacks[1][-1].lineno, firstlineno(get_stack_multi) + 5)
        self.assertEqual(stacks[1][-2].lineno, firstlineno(call_get_stack_multi) + 3)

    @jit_suppress
    @unittest.skipIf(
        not cinderx.jit.is_hir_inliner_enabled(),
        "meaningless without HIR inliner enabled",
    )
    def test_inline_function_stats(self) -> None:
        self.assertEqual(cinderx.jit.get_num_inlined_functions(func), 2)
        stats = cinderx.jit.get_inlined_functions_stats(func)
        self.assertEqual(
            {
                "num_inlined_functions": 2,
                "failure_stats": {
                    "HasVarargs": {"test_cinderx.test_cinderjit:func_with_varargs"}
                },
            },
            stats,
        )

    @jit_suppress
    @unittest.skipIf(
        not cinderx.jit.is_hir_inliner_enabled(),
        "meaningless without HIR inliner enabled",
    )
    def test_line_numbers_with_multiple_inlined_calls(self) -> None:
        """Verify that line numbers are correct for inlined calls that appear
        in different statements
        """
        # Call to get_stack should be inlined twice
        self.assertEqual(cinderx.jit.get_num_inlined_functions(get_stack_twice), 2)
        stacks = get_stack_twice()
        # First call to double
        self.assertEqual(stacks[0][-1].lineno, firstlineno(get_stack) + 3)
        self.assertEqual(stacks[0][-2].lineno, firstlineno(get_stack_twice) + 3)
        # Second call to double
        self.assertEqual(stacks[1][-1].lineno, firstlineno(get_stack) + 3)
        self.assertEqual(stacks[1][-2].lineno, firstlineno(get_stack_twice) + 4)


class FaulthandlerTracebackTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def f1(self, fd):
        self.f2(fd)

    @cinder_support.failUnlessJITCompiled
    def f2(self, fd):
        self.f3(fd)

    @cinder_support.failUnlessJITCompiled
    def f3(self, fd):
        faulthandler.dump_traceback(fd)

    def test_dumptraceback(self) -> None:
        expected = [
            f'  File "{__file__}", line {firstlineno(self.f3) + 2} in f3',
            f'  File "{__file__}", line {firstlineno(self.f2) + 2} in f2',
            f'  File "{__file__}", line {firstlineno(self.f1) + 2} in f1',
        ]
        with tempfile.TemporaryFile() as f:
            self.f1(f.fileno())
            f.seek(0)
            output = f.read().decode("ascii")
            lines = output.split("\n")
            self.assertGreaterEqual(len(lines), len(expected) + 1)
            # Ignore first line, which is 'Current thread: ...'
            self.assertEqual(lines[1:4], expected)


def _simpleFunc(a, b):
    return a, b


class _CallableObj:
    def __call__(self, a, b):
        return self, a, b


class CallKWArgsTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def test_call_basic_function_pos_and_kw(self) -> None:
        r = _simpleFunc(1, b=2)
        self.assertEqual(r, (1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_basic_function_kw_only(self) -> None:
        r = _simpleFunc(b=2, a=1)
        self.assertEqual(r, (1, 2))

        r = _simpleFunc(a=1, b=2)
        self.assertEqual(r, (1, 2))

    @staticmethod
    def _f1(a, b):
        return a, b

    @cinder_support.failUnlessJITCompiled
    def test_call_class_static_pos_and_kw(self) -> None:
        r = CallKWArgsTests._f1(1, b=2)
        self.assertEqual(r, (1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_class_static_kw_only(self) -> None:
        r = CallKWArgsTests._f1(b=2, a=1)
        self.assertEqual(r, (1, 2))

    def _f2(self, a, b):
        return self, a, b

    @cinder_support.failUnlessJITCompiled
    def test_call_method_kw_and_pos(self) -> None:
        r = self._f2(1, b=2)
        self.assertEqual(r, (self, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_method_kw_only(self) -> None:
        r = self._f2(b=2, a=1)
        self.assertEqual(r, (self, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_bound_method_kw_and_pos(self) -> None:
        f = self._f2
        r = f(1, b=2)
        self.assertEqual(r, (self, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_bound_method_kw_only(self) -> None:
        f = self._f2
        r = f(b=2, a=1)
        self.assertEqual(r, (self, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_obj_kw_and_pos(self) -> None:
        o = _CallableObj()
        r = o(1, b=2)
        self.assertEqual(r, (o, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_obj_kw_only(self) -> None:
        o = _CallableObj()
        r = o(b=2, a=1)
        self.assertEqual(r, (o, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_c_func(self) -> None:
        self.assertEqual(__import__("sys", globals=None), sys)


class CallExTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def test_call_dynamic_kw_dict(self) -> None:
        r = _simpleFunc(**{"b": 2, "a": 1})
        self.assertEqual(r, (1, 2))

    class _DummyMapping:
        def keys(self):
            return ("a", "b")

        def __getitem__(self, k):
            return {"a": 1, "b": 2}[k]

    @cinder_support.failUnlessJITCompiled
    def test_call_dynamic_kw_dict_dummy(self) -> None:
        r = _simpleFunc(**CallExTests._DummyMapping())
        self.assertEqual(r, (1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_dynamic_pos_tuple(self) -> None:
        r = _simpleFunc(*(1, 2))
        self.assertEqual(r, (1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_dynamic_pos_list(self) -> None:
        r = _simpleFunc(*[1, 2])
        self.assertEqual(r, (1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_dynamic_pos_and_kw(self) -> None:
        r = _simpleFunc(*(1,), **{"b": 2})
        self.assertEqual(r, (1, 2))

    @cinder_support.failUnlessJITCompiled
    def _doCall(self, args, kwargs):
        return _simpleFunc(*args, **kwargs)

    def test_invalid_kw_type(self) -> None:
        err = r"_simpleFunc\(\) argument after \*\* must be a mapping, not int"
        with self.assertRaisesRegex(TypeError, err):
            self._doCall([], 1)

    @skip_unless_jit("Exposes interpreter reference leak")
    def test_invalid_pos_type(self) -> None:
        err = r"_simpleFunc\(\) argument after \* must be an iterable, not int"
        with self.assertRaisesRegex(TypeError, err):
            self._doCall(1, {})

    @staticmethod
    def _f1(a, b):
        return a, b

    @cinder_support.failUnlessJITCompiled
    def test_call_class_static_pos_and_kw(self) -> None:
        r = CallExTests._f1(*(1,), **{"b": 2})
        self.assertEqual(r, (1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_class_static_kw_only(self) -> None:
        r = CallKWArgsTests._f1(**{"b": 2, "a": 1})
        self.assertEqual(r, (1, 2))

    def _f2(self, a, b):
        return self, a, b

    @cinder_support.failUnlessJITCompiled
    def test_call_method_kw_and_pos(self) -> None:
        r = self._f2(*(1,), **{"b": 2})
        self.assertEqual(r, (self, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_method_kw_only(self) -> None:
        r = self._f2(**{"b": 2, "a": 1})
        self.assertEqual(r, (self, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_bound_method_kw_and_pos(self) -> None:
        f = self._f2
        r = f(*(1,), **{"b": 2})
        self.assertEqual(r, (self, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_bound_method_kw_only(self) -> None:
        f = self._f2
        r = f(**{"b": 2, "a": 1})
        self.assertEqual(r, (self, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_obj_kw_and_pos(self) -> None:
        o = _CallableObj()
        r = o(*(1,), **{"b": 2})
        self.assertEqual(r, (o, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_obj_kw_only(self) -> None:
        o = _CallableObj()
        r = o(**{"b": 2, "a": 1})
        self.assertEqual(r, (o, 1, 2))

    @cinder_support.failUnlessJITCompiled
    def test_call_c_func_pos_only(self) -> None:
        self.assertEqual(len(*([2],)), 1)

    @cinder_support.failUnlessJITCompiled
    def test_call_c_func_pos_and_kw(self) -> None:
        self.assertEqual(__import__(*("sys",), **{"globals": None}), sys)


class SetNonDataDescrAttrTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("STORE_ATTR")
    def set_foo(self, obj, val):
        obj.foo = val

    def setUp(self):
        class Descr:
            def __init__(self, name):
                self.name = name

            def __get__(self, obj, typ):
                return obj.__dict__[self.name]

        self.descr_type = Descr
        self.descr = Descr("foo")

        class Test:
            foo = self.descr

        self.obj = Test()

    def test_set_when_changed_to_data_descr(self) -> None:
        # uncached
        self.set_foo(self.obj, 100)
        self.assertEqual(self.obj.foo, 100)

        # cached
        self.set_foo(self.obj, 200)
        self.assertEqual(self.obj.foo, 200)

        # convert into a data descriptor
        def setter(self, obj, val):
            self.invoked = True

        self.descr.__class__.__set__ = setter

        # setter doesn't modify the object, so obj.foo shouldn't change
        self.set_foo(self.obj, 300)
        self.assertEqual(self.obj.foo, 200)
        self.assertTrue(self.descr.invoked)


class GetSetNonDataDescrAttrTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_ATTR")
    def get_foo(self, obj):
        return obj.foo

    def setUp(self):
        class NonDataDescr:
            def __init__(self, val):
                self.val = val
                self.invoked_count = 0
                self.set_dict = True

            def __get__(self, obj, typ):
                self.invoked_count += 1
                if self.set_dict:
                    obj.__dict__["foo"] = self.val
                return self.val

        self.descr_type = NonDataDescr
        self.descr = NonDataDescr("testing 123")

        class Test:
            foo = self.descr

        self.obj = Test()

    def test_get(self) -> None:
        # uncached
        self.assertEqual(self.get_foo(self.obj), "testing 123")
        self.assertEqual(self.descr.invoked_count, 1)

        # cached; __get__ should not be invoked as there is now a shadowing
        # entry in obj's __dict__
        self.assertEqual(self.get_foo(self.obj), "testing 123")
        self.assertEqual(self.descr.invoked_count, 1)

        # cached; __get__ should be invoked as there is not a shadowing
        # entry in obj2's __dict__
        obj2 = self.obj.__class__()
        self.assertEqual(self.get_foo(obj2), "testing 123")
        self.assertEqual(self.descr.invoked_count, 2)

    def test_get_when_changed_to_data_descr(self) -> None:
        # uncached
        self.assertEqual(self.get_foo(self.obj), "testing 123")
        self.assertEqual(self.descr.invoked_count, 1)

        # cached; __get__ should not be invoked as there is now a shadowing
        # entry in obj's __dict__
        self.assertEqual(self.get_foo(self.obj), "testing 123")
        self.assertEqual(self.descr.invoked_count, 1)

        # Convert descriptor into a data descr by modifying its type
        def setter(self, obj, val):
            pass

        self.descr.__class__.__set__ = setter

        # cached; __get__ should be invoked as self.descr is now a data descr
        self.assertEqual(self.get_foo(self.obj), "testing 123")
        self.assertEqual(self.descr.invoked_count, 2)

    def test_get_when_changed_to_classvar(self) -> None:
        # Don't set anything in the instance dict when the descriptor is
        # invoked. This ensures we don't early exit and bottom out into the
        # descriptor case.
        self.descr.set_dict = False

        # uncached
        self.assertEqual(self.get_foo(self.obj), "testing 123")
        self.assertEqual(self.descr.invoked_count, 1)

        # cached
        self.assertEqual(self.get_foo(self.obj), "testing 123")
        self.assertEqual(self.descr.invoked_count, 2)

        # Convert descriptor into a plain old value by changing the
        # descriptor's type
        class ClassVar:
            pass

        self.descr.__class__ = ClassVar

        # Cached; type check on descriptor's type should fail
        self.assertIs(self.get_foo(self.obj), self.descr)
        self.assertEqual(self.descr.invoked_count, 2)


class ClosureTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def test_cellvar(self) -> None:
        a = 1

        def foo():
            return a

        self.assertEqual(foo(), 1)

    @cinder_support.failUnlessJITCompiled
    def test_two_cellvars(self) -> None:
        a = 1
        b = 2

        def g():
            return a + b

        self.assertEqual(g(), 3)

    @cinder_support.failUnlessJITCompiled
    def test_cellvar_argument(self) -> None:
        def foo():
            self.assertEqual(1, 1)

        foo()

    @cinder_support.failUnlessJITCompiled
    def test_cellvar_argument_modified(self) -> None:
        self_ = self

        def foo():
            nonlocal self
            self = 1

        self_.assertIs(self, self_)

        foo()

        self_.assertEqual(self, 1)

    @cinder_support.failUnlessJITCompiled
    def _cellvar_unbound(self):
        b = a  # noqa: F821, F841
        a = 1

        def g():
            return a

    def test_cellvar_unbound(self) -> None:
        with self.assertRaises(UnboundLocalError) as ctx:
            self._cellvar_unbound()

        self.assertEqual(
            str(ctx.exception),
            (
                "cannot access local variable 'a' where it is not associated with a value"
                if AT_LEAST_312
                else "local variable 'a' referenced before assignment"
            ),
        )

    def test_freevars(self) -> None:
        x = 1

        @cinder_support.failUnlessJITCompiled
        def nested():
            return x

        x = 2

        self.assertEqual(nested(), 2)

    def test_freevars_multiple_closures(self) -> None:
        def get_func(a):
            @cinder_support.failUnlessJITCompiled
            def f():
                return a

            return f

        f1 = get_func(1)
        f2 = get_func(2)

        self.assertEqual(f1(), 1)
        self.assertEqual(f2(), 2)

    def test_nested_func(self) -> None:
        @cinder_support.failUnlessJITCompiled
        def add(a, b):
            return a + b

        self.assertEqual(add(1, 2), 3)
        self.assertEqual(add("eh", "bee"), "ehbee")

    @staticmethod
    def make_adder(a):
        @cinder_support.failUnlessJITCompiled
        def add(b):
            return a + b

        return add

    def test_nested_func_with_closure(self) -> None:
        add_3 = self.make_adder(3)
        add_7 = self.make_adder(7)

        self.assertEqual(add_3(10), 13)
        self.assertEqual(add_7(12), 19)
        self.assertEqual(add_3(add_7(-100)), -90)
        with self.assertRaises(TypeError):
            add_3("ok")

    def test_nested_func_with_different_globals(self) -> None:
        @cinder_support.failUnlessJITCompiled
        @with_globals({"A_GLOBAL_CONSTANT": 0xDEADBEEF})
        def return_global():
            return A_GLOBAL_CONSTANT  # noqa: F821

        self.assertEqual(return_global(), 0xDEADBEEF)

        return_other_global = with_globals({"A_GLOBAL_CONSTANT": 0xFACEB00C})(
            return_global
        )
        self.assertEqual(return_other_global(), 0xFACEB00C)

        self.assertEqual(return_global(), 0xDEADBEEF)
        self.assertEqual(return_other_global(), 0xFACEB00C)

    def test_nested_func_outlives_parent(self) -> None:
        @cinder_support.failUnlessJITCompiled
        def nested(x):
            @cinder_support.failUnlessJITCompiled
            def inner(y):
                return x + y

            return inner

        nested_ref = weakref.ref(nested)
        add_5 = nested(5)
        nested = None
        self.assertIsNone(nested_ref())
        self.assertEqual(add_5(10), 15)


class TempNameTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def _tmp_name(self, a, b):
        tmp1 = "hello"
        c = a + b  # noqa: F841
        return tmp1

    def test_tmp_name(self) -> None:
        self.assertEqual(self._tmp_name(1, 2), "hello")

    @cinder_support.failUnlessJITCompiled
    def test_tmp_name2(self) -> None:
        v0 = 5
        self.assertEqual(v0, 5)


class JITCompileCrasherRegressionTests(StaticTestBase):
    @cinder_support.failUnlessJITCompiled
    def test_isinstance_optimization(self) -> None:
        return isinstance(None, int) and True

    @cinder_support.failUnlessJITCompiled
    def _fstring(self, flag, it1, it2):
        for a in it1:
            for b in it2:  # noqa: B007
                if flag:
                    return f"{a}"

    def test_fstring_no_fmt_spec_in_nested_loops_and_if(self) -> None:
        self.assertEqual(self._fstring(True, [1], [1]), "1")

    @cinder_support.failUnlessJITCompiled
    async def _sharedAwait(self, x, y, z):
        return await (x() if y else z())

    def test_shared_await(self) -> None:
        async def zero():
            return 0

        async def one():
            return 1

        with self.assertRaises(StopIteration) as exc:
            self._sharedAwait(zero, True, one).send(None)
        self.assertEqual(exc.exception.value, 0)

        with self.assertRaises(StopIteration) as exc:
            self._sharedAwait(zero, False, one).send(None)
        self.assertEqual(exc.exception.value, 1)

    @cinder_support.failUnlessJITCompiled
    def load_method_on_maybe_defined_value(self):
        # This function exists to make sure that we don't crash the compiler
        # when attempting to optimize load methods that occur on maybe-defined
        # values.
        try:
            pass
        except:  # noqa: B001
            x = 1
        return x.__index__()

    def test_load_method_on_maybe_defined_value(self) -> None:
        with self.assertRaises(NameError):
            self.load_method_on_maybe_defined_value()

    @run_in_subprocess
    def test_condbranch_codegen(self) -> None:
        codestr = """
            from __static__ import cbool
            from typing import Optional


            class Foo:
                def __init__(self, x: bool) -> None:
                    y = cbool(x)
                    self.start_offset_us: float = 0.0
                    self.y: cbool = y
        """
        with self.in_module(codestr) as mod:
            if hasattr(gc, "immortalize_heap"):
                gc.immortalize_heap()
            force_compile(mod.Foo.__init__)
            mod.Foo(True)

    def test_restore_materialized_parent_pyframe_in_gen_throw(self) -> None:
        # This reproduces a bug that causes the top frame in the shadow stack
        # to be out of sync with the top frame on the Python stack.
        #
        # When a generator that is thrown into is yielding from another generator
        # it does the following (see `_gen_throw` in genobject.c):
        #
        # 1. Save the top of the Python and shadow stacks to local variables.
        # 2. Set the top of Python and shadow stacks to the respective frames
        #    belonging to the generator.
        # 3. Throw the exception into the value from which it is yielding.
        # 4. Restore the top of the Python and shadow stacks to the values
        #    that were saved locally in (1).
        #
        # When running in shadow frame mode the Python frame for the shadow
        # frame at the top of the stack in (1) may be materialized in (3).
        # When the frame is materialized, the local copy that is used in (4)
        # will be incorrect and needs to be updated to match the materialized
        # frame.
        #
        # When run under shadow-frame mode when the CancelledError is thrown
        # into c, the following happens:
        #
        # 1. Execution enters `awaitable_throw` in classloader.c.
        # 2. `awaitable_throw` ends up calling `_gen_throw` for `c`.
        # 3. At this point, the top of the shadow stack is the shadow
        #    frame for `a` (not `b`, since `b` awaits a non-coroutine it
        #    does not do the save/restore dance for TOS). The top
        #    of the Python stack is NULL, since `a` has not had its
        #    frame materialized. These are saved as locals in `_gen_throw`.
        # 4. `c` throws into `d`, which materializes the Python frame
        #    for `a`.
        # 5. Execution returns to `_gen_throw` for `c`, which goes about
        #    restoring the top of the shadow stack and python stack. The top of
        #    the shadow stack hasn't changed, however, the top of Python stack
        #    has. We need to be careful to update the top of the Python stack
        #    to the materialized Python frame. Otherwise we'd restore it
        #    to the saved value, NULL, and the shadow stack and Python stack
        #    would be out of sync.
        # 6. `awaitable_throw` ends up invoking `ctxmgrwrp_exit`, which calls
        #    `PyEval_GetFrame`. `PyEval_GetFrame` materializes the Python
        #    frames for the shadow stack and returns `tstate->frame`.
        #    If the TOS gets out of sync in 5, we'll either fail consistency
        #    checks in debug mode, or return NULL from `PyEval_GetFrame`.
        #
        # Note that this particular repro requires using ContextDecorator
        # in non-static code.
        from __static__ import ContextDecorator

        async def a(child_fut, main_fut, box):
            return await b(child_fut, main_fut, box)

        async def b(child_fut, main_fut, box):
            return await c(child_fut, main_fut, box)

        @ContextDecorator()
        async def c(child_fut, main_fut, box):
            return await d(child_fut, main_fut, box)

        async def d(child_fut, main_fut, box):
            main_fut.set_result(True)
            try:
                await child_fut
            except:  # noqa: B001
                # force the frame to be materialized
                box[0].cr_frame
                raise

        async def main():
            child_fut = asyncio.Future()
            main_fut = asyncio.Future()
            box = [None]
            coro = a(child_fut, main_fut, box)
            box[0] = coro
            t = asyncio.create_task(coro)
            # wait for d to have started
            await main_fut

            t.cancel()
            await t

        with self.assertRaises(asyncio.CancelledError):
            asyncio.run(main())

        if compiles_after_one_call():
            self.assertTrue(is_jit_compiled(a))
            self.assertTrue(is_jit_compiled(b))
            self.assertTrue(is_jit_compiled(c.__wrapped__))
            self.assertTrue(is_jit_compiled(d))

    def test_delete_fast_return(self) -> None:
        def foo(hmm):
            try:
                return hmm
            finally:
                del hmm

        force_compile(foo)
        self.assertEqual(foo([5]), [5])


class DelObserver:
    def __init__(self, id, cb):
        self.id = id
        self.cb = cb

    def __del__(self):
        self.cb(self.id)


class UnwindStateTests(unittest.TestCase):
    DELETED = []

    def setUp(self):
        self.DELETED.clear()
        self.addCleanup(lambda: self.DELETED.clear())

    def get_del_observer(self, id):
        return DelObserver(id, lambda i: self.DELETED.append(i))

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("RAISE_VARARGS")
    def _copied_locals(self, a):
        b = c = a  # noqa: F841
        raise RuntimeError()

    def test_copied_locals_in_frame(self) -> None:
        try:
            self._copied_locals("hello")
        except RuntimeError as re:
            f_locals = re.__traceback__.tb_next.tb_frame.f_locals
            self.assertEqual(
                f_locals, {"self": self, "a": "hello", "b": "hello", "c": "hello"}
            )

    @cinder_support.failUnlessJITCompiled
    def _raise_with_del_observer_on_stack(self):
        for x in (1 for i in [self.get_del_observer(1)]):  # noqa: B007
            raise RuntimeError()

    def test_decref_stack_objects(self) -> None:
        """Items on stack should be decrefed on unwind."""
        try:
            self._raise_with_del_observer_on_stack()
        except RuntimeError:
            deleted = list(self.DELETED)
        else:
            self.fail("should have raised RuntimeError")
        self.assertEqual(deleted, [1])

    @cinder_support.failUnlessJITCompiled
    def _raise_with_del_observer_on_stack_and_cell_arg(self):
        for x in (self for i in [self.get_del_observer(1)]):  # noqa: B007
            raise RuntimeError()

    def test_decref_stack_objs_with_cell_args(self) -> None:
        # Regression test for a JIT bug in which the unused locals slot for a
        # local-which-is-a-cell would end up getting populated on unwind with
        # some unrelated stack object, preventing it from being decrefed.
        try:
            self._raise_with_del_observer_on_stack_and_cell_arg()
        except RuntimeError:
            deleted = list(self.DELETED)
        else:
            self.fail("should have raised RuntimeError")
        self.assertEqual(deleted, [1])


class ImportTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def test_import_name(self) -> None:
        import math

        self.assertEqual(int(math.pow(1, 2)), 1)

    @cinder_support.failUnlessJITCompiled
    def _fail_to_import_name(self):
        import non_existent_module  # noqa: F401

    def test_import_name_failure(self) -> None:
        with self.assertRaises(ModuleNotFoundError):
            self._fail_to_import_name()

    @cinder_support.failUnlessJITCompiled
    def test_import_from(self) -> None:
        from math import pow as math_pow

        self.assertEqual(int(math_pow(1, 2)), 1)

    @cinder_support.failUnlessJITCompiled
    def _fail_to_import_from(self):
        from math import non_existent_attr  # noqa: F401

    def test_import_from_failure(self) -> None:
        with self.assertRaises(ImportError):
            self._fail_to_import_from()


class RaiseTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("RAISE_VARARGS")
    def _jitRaise(self, exc):
        raise exc

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("RAISE_VARARGS")
    def _jitRaiseCause(self, exc, cause):
        raise exc from cause

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("RAISE_VARARGS")
    def _jitReraise(self):
        raise

    def test_raise_type(self) -> None:
        with self.assertRaises(ValueError):
            self._jitRaise(ValueError)

    def test_raise_value(self) -> None:
        with self.assertRaises(ValueError) as exc:
            self._jitRaise(ValueError(1))
        self.assertEqual(exc.exception.args, (1,))

    def test_raise_with_cause(self) -> None:
        cause = ValueError(2)
        cause_tb_str = f"{cause.__traceback__}"
        with self.assertRaises(ValueError) as exc:
            self._jitRaiseCause(ValueError(1), cause)
        self.assertIs(exc.exception.__cause__, cause)
        self.assertEqual(f"{exc.exception.__cause__.__traceback__}", cause_tb_str)

    def test_reraise(self) -> None:
        original_raise = ValueError(1)
        with self.assertRaises(ValueError) as exc:
            try:
                raise original_raise
            except ValueError:
                self._jitReraise()
        self.assertIs(exc.exception, original_raise)

    def test_reraise_of_nothing(self) -> None:
        with self.assertRaises(RuntimeError) as exc:
            self._jitReraise()
        self.assertEqual(exc.exception.args, ("No active exception to reraise",))


class SpecializeCCallTests(unittest.TestCase):
    """
    The JIT performs direct calls of C functions with CallStatic when possible.
    """

    @cinder_support.failUnlessJITCompiled
    def _c_func_that_sets_pyerr(self):
        s = "abc"
        return s.removeprefix(1)

    def test_c_call_error_raised(self) -> None:
        with self.assertRaises(TypeError):
            self._c_func_that_sets_pyerr()


class UnpackSequenceTests(unittest.TestCase):
    @failUnlessHasOpcodes("UNPACK_SEQUENCE")
    @cinder_support.failUnlessJITCompiled
    def _unpack_arg(self, seq, which):
        a, b, c, d = seq
        if which == "a":
            return a
        if which == "b":
            return b
        if which == "c":
            return c
        return d

    @failUnlessHasOpcodes("UNPACK_EX")
    @cinder_support.failUnlessJITCompiled
    def _unpack_ex_arg(self, seq, which):
        a, b, *c, d = seq
        if which == "a":
            return a
        if which == "b":
            return b
        if which == "c":
            return c
        return d

    def test_unpack_tuple(self) -> None:
        self.assertEqual(self._unpack_arg(("eh", "bee", "see", "dee"), "b"), "bee")
        self.assertEqual(self._unpack_arg((3, 2, 1, 0), "c"), 1)

    def test_unpack_tuple_wrong_size(self) -> None:
        with self.assertRaises(ValueError):
            self._unpack_arg((1, 2, 3, 4, 5), "a")

    def test_unpack_list(self) -> None:
        self.assertEqual(self._unpack_arg(["one", "two", "three", "four"], "a"), "one")

    def test_unpack_gen(self) -> None:
        def gen():
            yield "first"
            yield "second"
            yield "third"
            yield "fourth"

        self.assertEqual(self._unpack_arg(gen(), "d"), "fourth")

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("UNPACK_EX")
    def _unpack_not_iterable(self):
        (a, b, *c) = 1

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("UNPACK_EX")
    def _unpack_insufficient_values(self):
        (a, b, *c) = [1]  # noqa: F841

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("UNPACK_EX")
    def _unpack_insufficient_values_after(self):
        (a, *b, c, d) = [1, 2]  # noqa: F841

    def test_unpack_ex(self) -> None:
        with self.assertRaises(TypeError):
            self._unpack_not_iterable()
        with self.assertRaises(ValueError):
            self._unpack_insufficient_values()
        with self.assertRaises(ValueError):
            self._unpack_insufficient_values_after()

        seq = [1, 2, 3, 4, 5, 6]
        self.assertEqual(self._unpack_ex_arg(seq, "a"), 1)
        self.assertEqual(self._unpack_ex_arg(seq, "b"), 2)
        self.assertEqual(self._unpack_ex_arg(seq, "c"), [3, 4, 5])
        self.assertEqual(self._unpack_ex_arg(seq, "d"), 6)

    def test_unpack_sequence_with_iterable(self) -> None:
        class C:
            def __init__(self, value):
                self.value = value

            def __iter__(self):
                return iter(self.value)

        seq = (1, 2, 3, 4)
        self.assertEqual(self._unpack_arg(C(seq), "a"), 1)
        self.assertEqual(self._unpack_arg(C(seq), "b"), 2)
        self.assertEqual(self._unpack_arg(C(seq), "c"), 3)
        self.assertEqual(self._unpack_arg(C(seq), "d"), 4)
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            self._unpack_arg(C(()), "a")

    def test_unpack_ex_with_iterable(self) -> None:
        class C:
            def __init__(self, value):
                self.value = value

            def __iter__(self):
                return iter(self.value)

        seq = (1, 2, 3, 4, 5, 6)
        self.assertEqual(self._unpack_ex_arg(C(seq), "a"), 1)
        self.assertEqual(self._unpack_ex_arg(C(seq), "b"), 2)
        self.assertEqual(self._unpack_ex_arg(C(seq), "c"), [3, 4, 5])
        self.assertEqual(self._unpack_ex_arg(C(seq), "d"), 6)
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            self._unpack_ex_arg(C(()), "a")


class DeleteSubscrTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("DELETE_SUBSCR")
    def _delit(self, container, key):
        del container[key]

    def test_builtin_types(self) -> None:
        li = [1, 2, 3]
        self._delit(li, 1)
        self.assertEqual(li, [1, 3])

        d = {"foo": 1, "bar": 2}
        self._delit(d, "foo")
        self.assertEqual(d, {"bar": 2})

    def test_custom_type(self) -> None:
        class CustomContainer:
            def __init__(self):
                self.item = None

            def __delitem__(self, item):
                self.item = item

        c = CustomContainer()
        self._delit(c, "foo")
        self.assertEqual(c.item, "foo")

    def test_missing_key(self) -> None:
        d = {"foo": 1}
        with self.assertRaises(KeyError):
            self._delit(d, "bar")

    def test_custom_error(self) -> None:
        class CustomContainer:
            def __delitem__(self, item):
                raise Exception("testing 123")

        c = CustomContainer()
        with self.assertRaisesRegex(Exception, "testing 123"):
            self._delit(c, "foo")


class DeleteFastTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("DELETE_FAST")
    def _del(self):
        x = 2
        del x

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("DELETE_FAST")
    def _del_arg(self, a):
        del a

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("DELETE_FAST")
    def _del_and_raise(self):
        x = 2
        del x
        return x

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("DELETE_FAST")
    def _del_arg_and_raise(self, a):
        del a
        return a  # noqa: F821

    @failUnlessHasOpcodes("DELETE_FAST")
    @cinder_support.failUnlessJITCompiled
    def _del_ex_no_raise(self):
        try:
            return min(1, 2)
        except Exception as e:  # noqa: F841
            pass

    @failUnlessHasOpcodes("DELETE_FAST")
    @cinder_support.failUnlessJITCompiled
    def _del_ex_raise(self):
        try:
            raise Exception()
        except Exception as e:  # noqa: F841
            pass
        return e  # noqa: F821

    def test_del_local(self) -> None:
        self.assertEqual(self._del(), None)

    def test_del_arg(self) -> None:
        self.assertEqual(self._del_arg(42), None)

    def test_del_and_raise(self) -> None:
        with self.assertRaises(NameError):
            self._del_and_raise()

    def test_del_arg_and_raise(self) -> None:
        with self.assertRaises(NameError):
            self.assertEqual(self._del_arg_and_raise(42), None)

    def test_del_ex_no_raise(self) -> None:
        self.assertEqual(self._del_ex_no_raise(), 1)

    def test_del_ex_raise(self) -> None:
        with self.assertRaises(NameError):
            self.assertEqual(self._del_ex_raise(), 42)


class DictSubscrTests(unittest.TestCase):
    def test_int_custom_class(self) -> None:
        class C:
            def __init__(self, value):
                self.value = value

            def __eq__(self, other):
                raise RuntimeError("no way!!")

            def __hash__(self):
                return hash(self.value)

        c = C(333)
        d = {}
        d[c] = 1
        with self.assertRaises(RuntimeError):
            d[333]

    def test_unicode_custom_class(self) -> None:
        class C:
            def __init__(self, value):
                self.value = value

            def __eq__(self, other):
                raise RuntimeError("no way!!")

            def __hash__(self):
                return hash(self.value)

        c = C("x")
        d = {}
        d[c] = 1
        with self.assertRaises(RuntimeError):
            d["x"]


class KeywordOnlyArgTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def f1(self, *, val=10):
        return val

    @cinder_support.failUnlessJITCompiled
    def f2(self, which, *, y=10, z=20):
        if which == 0:
            return y
        elif which == 1:
            return z
        return which

    @cinder_support.failUnlessJITCompiled
    def f3(self, which, *, y, z=20):
        if which == 0:
            return y
        elif which == 1:
            return z
        return which

    @cinder_support.failUnlessJITCompiled
    def f4(self, which, *, y, z=20, **kwargs):
        if which == 0:
            return y
        elif which == 1:
            return z
        elif which == 2:
            return kwargs
        return which

    def test_kwonly_arg_passed_as_positional(self) -> None:
        msg = "takes 1 positional argument but 2 were given"
        with self.assertRaisesRegex(TypeError, msg):
            self.f1(100)
        msg = "takes 2 positional arguments but 3 were given"
        with self.assertRaisesRegex(TypeError, msg):
            self.f3(0, 1)

    def test_kwonly_args_with_kwdefaults(self) -> None:
        self.assertEqual(self.f1(), 10)
        self.assertEqual(self.f1(val=20), 20)
        self.assertEqual(self.f2(0), 10)
        self.assertEqual(self.f2(0, y=20), 20)
        self.assertEqual(self.f2(1), 20)
        self.assertEqual(self.f2(1, z=30), 30)

    def test_kwonly_args_without_kwdefaults(self) -> None:
        self.assertEqual(self.f3(0, y=10), 10)
        self.assertEqual(self.f3(1, y=10), 20)
        self.assertEqual(self.f3(1, y=10, z=30), 30)

    def test_kwonly_args_and_varkwargs(self) -> None:
        self.assertEqual(self.f4(0, y=10), 10)
        self.assertEqual(self.f4(1, y=10), 20)
        self.assertEqual(self.f4(1, y=10, z=30, a=40), 30)
        self.assertEqual(self.f4(2, y=10, z=30, a=40, b=50), {"a": 40, "b": 50})


class ClassA:
    z = 100
    x = 41

    def g(self, a):
        return 42 + a

    @classmethod
    def cls_g(cls, a):
        return 100 + a


class ClassB(ClassA):
    def f(self, a):
        return super().g(a=a)

    def f_2arg(self, a):
        return super().g(a=a)

    @classmethod
    def cls_f(cls, a):
        return super().cls_g(a=a)

    @classmethod
    def cls_f_2arg(cls, a):
        return super().cls_g(a=a)

    @property
    def x(self):
        return super().x + 1

    @property
    def x_2arg(self):
        return super().x + 1


class SuperAccessTest(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def test_super_method(self) -> None:
        self.assertEqual(ClassB().f(1), 43)
        self.assertEqual(ClassB().f_2arg(1), 43)
        self.assertEqual(ClassB.cls_f(99), 199)
        self.assertEqual(ClassB.cls_f_2arg(99), 199)

    @cinder_support.failUnlessJITCompiled
    def test_super_method_kwarg(self) -> None:
        self.assertEqual(ClassB().f(1), 43)
        self.assertEqual(ClassB().f_2arg(1), 43)
        self.assertEqual(ClassB.cls_f(1), 101)
        self.assertEqual(ClassB.cls_f_2arg(1), 101)

    @cinder_support.failUnlessJITCompiled
    def test_super_attr(self) -> None:
        self.assertEqual(ClassB().x, 42)
        self.assertEqual(ClassB().x_2arg, 42)


class RegressionTests(StaticTestBase):
    # Detects an issue in the backend where the Store instruction generated 32-
    # bit memory writes for 64-bit constants.
    def test_store_of_64bit_immediates(self) -> None:
        codestr = """
            from __static__ import int64, box
            class Cint64:
                def __init__(self):
                    self.a: int64 = 0x5555555555555555

            def testfunc():
                c = Cint64()
                c.a = 2
                return box(c.a) == 2
        """
        with self.in_module(codestr) as mod:
            testfunc = mod.testfunc
            self.assertTrue(testfunc())

            if compiles_after_one_call():
                self.assertTrue(is_jit_compiled(testfunc))


@skip_unless_jit("Requires cinderjit module")
class CinderJitModuleTests(StaticTestBase):
    def test_bad_disable(self) -> None:
        with self.assertRaises(TypeError):
            cinderx.jit.disable(1, 2, 3)

    def test_jit_suppress(self) -> None:
        @jit_suppress
        def x():
            pass

        self.assertEqual(x.__code__.co_flags & CO_SUPPRESS_JIT, CO_SUPPRESS_JIT)

    def test_jit_suppress_static(self) -> None:
        codestr = """
            import cinderx.jit

            @cinderx.jit.jit_suppress
            def f():
                return True

            def g():
                return True
        """
        with self.in_module(codestr) as mod:
            f = mod.f
            g = mod.g
            self.assertTrue(f())
            self.assertTrue(g())

            self.assertFalse(is_jit_compiled(f))

            if compiles_after_one_call():
                self.assertTrue(is_jit_compiled(g))

    @unittest.skipIf(
        not cinderx.jit.is_hir_inliner_enabled(),
        "meaningless without HIR inliner enabled",
    )
    def test_num_inlined_functions(self) -> None:
        codestr = """
            import cinderx.jit

            @cinderx.jit.jit_suppress
            def f():
                return True

            def g():
                return f()
        """
        with self.in_module(codestr) as mod:
            f = mod.f
            g = mod.g
            self.assertTrue(g())

            self.assertFalse(is_jit_compiled(f))

            if compiles_after_one_call():
                self.assertTrue(is_jit_compiled(g))

            self.assertEqual(cinderx.jit.get_num_inlined_functions(g), 1)

    @unittest.skipIf(
        (
            cinderx.jit.auto_jit_threshold() == 0
            or cinderx.jit.auto_jit_threshold() > 10000
        )
        and not cinderx.jit.is_compile_all(),
        "Expecting the JIT to be compiling a bunch of code automatically",
    )
    def test_max_code_size_slow(self) -> None:
        code = textwrap.dedent(
            """
            import cinderx.jit
            for i in range(2000):
                exec(f'''
            def junk{i}(j):
                j = j + 1
                s = f'dogs {i} ' + str(j)
                if s == '23':
                    j += 2
                return j*2+{i}
            ''')
            x = 0
            for i in range(2000):
                exec(f'x *= junk{i}(i)')
            max_bytes = cinderx.jit.get_allocator_stats()["max_bytes"]
            used_bytes = cinderx.jit.get_allocator_stats()["used_bytes"]
            print(f'max_size: {max_bytes}')
            print(f'used_size: {used_bytes}')
        """
        )
        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            codepath.write_text(code)

            def run_test(asserts_func, params):
                args = [sys.executable, "-X", "jit-all"]
                args.extend(params)
                args.append("mod.py")
                proc = subprocess.run(
                    args,
                    cwd=tmp,
                    stdout=subprocess.PIPE,
                    encoding=ENCODING,
                    env={"PYTHONPATH": CINDERX_PATH},
                )
                self.assertEqual(proc.returncode, 0, proc)
                actual_stdout = [x.strip() for x in proc.stdout.split("\n")]
                asserts_func(actual_stdout)

            def zero_asserts(actual_stdout):
                expected_stdout = "max_size: 0"
                self.assertEqual(actual_stdout[0], expected_stdout)
                self.assertIn("used_size", actual_stdout[1])
                used_size = int(actual_stdout[1].split(" ")[1])
                self.assertGreater(used_size, 0)

            def onek_asserts(actual_stdout):
                expected_stdout = "max_size: 1024"
                self.assertEqual(actual_stdout[0], expected_stdout)
                self.assertIn("used_size", actual_stdout[1])
                used_size = int(actual_stdout[1].split(" ")[1])
                self.assertGreater(used_size, 1024)
                # This is a bit fragile because it depends on what the initial 'zeroth'
                # allocation is; we assume < 200K.
                self.assertLess(used_size, 1024 * 200)

            run_test(zero_asserts, ["-X", "jit-max-code-size=0"])
            run_test(
                zero_asserts,
                ["-X", "jit-max-code-size=0", "-X", "jit-huge-pages=0"],
            )
            run_test(
                zero_asserts,
                [
                    "-X",
                    "jit-max-code-size=0",
                    "-X",
                    "jit-multiple-code-sections=1",
                    "-X",
                    "jit-hot-code-section-size=1048576",
                    "-X",
                    "jit-cold-code-section-size=1048576",
                ],
            )
            run_test(onek_asserts, ["-X", "jit-max-code-size=1024"])
            run_test(
                onek_asserts,
                ["-X", "jit-max-code-size=1024", "-X", "jit-huge-pages=0"],
            )
            run_test(
                onek_asserts,
                [
                    "-X",
                    "jit-max-code-size=1024",
                    "-X",
                    "jit-multiple-code-sections=1",
                    "-X",
                    "jit-hot-code-section-size=1048576",
                    "-X",
                    "jit-cold-code-section-size=1048576",
                ],
            )

    def test_max_code_size_fast(self) -> None:
        code = textwrap.dedent(
            """
            import cinderx.jit
            max_bytes = cinderx.jit.get_allocator_stats()["max_bytes"]
            print(f'max_size: {max_bytes}')
        """
        )
        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            codepath.write_text(code)

            def run_proc():
                proc = subprocess.run(
                    args,
                    cwd=tmp,
                    stdout=subprocess.PIPE,
                    encoding=ENCODING,
                    env={"PYTHONPATH": CINDERX_PATH},
                )
                self.assertEqual(proc.returncode, 0, proc)
                actual_stdout = [x.strip() for x in proc.stdout.split("\n")]
                return actual_stdout[0]

            args = [sys.executable, "-X", "jit-all", "mod.py"]
            self.assertEqual(run_proc(), "max_size: 0")
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1234567",
                "mod.py",
            ]
            self.assertEqual(run_proc(), "max_size: 1234567")
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1k",
                "mod.py",
            ]
            self.assertEqual(run_proc(), "max_size: 1024")
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1K",
                "mod.py",
            ]
            self.assertEqual(run_proc(), "max_size: 1024")
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1m",
                "mod.py",
            ]
            self.assertEqual(run_proc(), "max_size: 1048576")
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1M",
                "mod.py",
            ]
            self.assertEqual(run_proc(), "max_size: 1048576")
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1g",
                "mod.py",
            ]
            self.assertEqual(run_proc(), "max_size: 1073741824")
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1G",
                "mod.py",
            ]
            self.assertEqual(run_proc(), "max_size: 1073741824")

            def run_proc():
                proc = subprocess.run(
                    args,
                    cwd=tmp,
                    stderr=subprocess.PIPE,
                    encoding=ENCODING,
                    env={"PYTHONPATH": CINDERX_PATH},
                )
                self.assertEqual(proc.returncode, -6, proc)
                return proc.stderr

            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=-1",
                "mod.py",
            ]
            self.assertIn("Invalid unsigned integer in input string: '-1'", run_proc())
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1.1",
                "mod.py",
            ]
            self.assertIn("Invalid unsigned integer in input string: '1.1'", run_proc())
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=dogs",
                "mod.py",
            ]
            self.assertIn("Invalid character in input string", run_proc())
            args = [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-max-code-size=1152921504606846976g",
                "mod.py",
            ]
            self.assertIn(
                "Unsigned Integer overflow in input string: '1152921504606846976g'",
                run_proc(),
            )


class DeleteAttrTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("DELETE_ATTR")
    def del_foo(self, obj):
        del obj.foo

    def test_delete_attr(self) -> None:
        class C:
            pass

        c = C()
        c.foo = "bar"
        self.assertEqual(c.foo, "bar")
        self.del_foo(c)
        with self.assertRaises(AttributeError):
            c.foo

    def test_delete_attr_raises(self) -> None:
        class C:
            @property
            def foo(self):
                return "hi"

        c = C()
        self.assertEqual(c.foo, "hi")
        with self.assertRaises(AttributeError):
            self.del_foo(c)


class OtherTests(unittest.TestCase):
    @unittest.skipIf(
        not cinderx.jit.is_enabled(),
        "meaningless without JIT enabled",
    )
    def test_mlock_profiler_dependencies(self) -> None:
        cinderx.jit.mlock_profiler_dependencies()

    @unittest.skipUnless(cinderx.jit.is_enabled(), "not jitting")
    def test_page_in_profiler_dependencies(self) -> None:
        qualnames = cinderx.jit.page_in_profiler_dependencies()
        self.assertTrue(len(qualnames) > 0)


class GetIterForIterTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("FOR_ITER", "GET_ITER")
    def doit(self, iterable):
        for _ in iterable:
            pass
        return 42

    def test_iterate_through_builtin(self) -> None:
        self.assertEqual(self.doit([1, 2, 3]), 42)

    def test_custom_iterable(self) -> None:
        class MyIterable:
            def __init__(self, limit):
                self.idx = 0
                self.limit = limit

            def __iter__(self):
                return self

            def __next__(self):
                if self.idx == self.limit:
                    raise StopIteration
                retval = self.idx
                self.idx += 1
                return retval

        it = MyIterable(5)
        self.assertEqual(self.doit(it), 42)
        self.assertEqual(it.idx, it.limit)

    def test_iteration_raises_error(self) -> None:
        class MyException(Exception):
            pass

        class MyIterable:
            def __init__(self):
                self.idx = 0

            def __iter__(self):
                return self

            def __next__(self):
                if self.idx == 3:
                    raise MyException(f"raised error on idx {self.idx}")
                self.idx += 1
                return 1

        with self.assertRaisesRegex(MyException, "raised error on idx 3"):
            self.doit(MyIterable())

    def test_iterate_generator(self) -> None:
        x = None

        def gen():
            nonlocal x
            yield 1
            yield 2
            yield 3
            x = 42

        self.doit(gen())
        self.assertEqual(x, 42)


class SetUpdateTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("BUILD_SET", "SET_UPDATE")
    def doit_unchecked(self, iterable):
        return {*iterable}

    def doit(self, iterable):
        result = self.doit_unchecked(iterable)
        self.assertIs(type(result), set)
        return result

    def test_iterate_non_iterable_raises_type_error(self) -> None:
        with self.assertRaisesRegex(TypeError, "'int' object is not iterable"):
            self.doit(42)

    def test_iterate_set_builds_set(self) -> None:
        self.assertEqual(self.doit({1, 2, 3}), {1, 2, 3})

    def test_iterate_dict_builds_set(self) -> None:
        self.assertEqual(
            self.doit({"hello": "world", "goodbye": "world"}), {"hello", "goodbye"}
        )

    def test_iterate_getitem_iterable_builds_set(self) -> None:
        class C:
            def __getitem__(self, index):
                if index < 4:
                    return index
                raise IndexError

        self.assertEqual(self.doit(C()), {0, 1, 2, 3})

    def test_iterate_iter_iterable_builds_set(self) -> None:
        class C:
            def __iter__(self):
                return iter([1, 2, 3])

        self.assertEqual(self.doit(C()), {1, 2, 3})


# TASK(T125845248): After D38227343 is landed and support for COMPARE_OP is in,
# remove UnpackSequenceTests entirely. It will then be covered by the other
# UnpackSequenceTests above.
class UnpackSequenceTestsWithoutCompare(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("UNPACK_SEQUENCE")
    def doit(self, iterable):
        x, y = iterable
        return x

    def test_unpack_sequence_with_tuple(self) -> None:
        self.assertEqual(self.doit((1, 2)), 1)
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            self.doit(())

    def test_unpack_sequence_with_list(self) -> None:
        self.assertEqual(self.doit([1, 2]), 1)
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            self.doit([])

    def test_unpack_sequence_with_iterable(self) -> None:
        class C:
            def __init__(self, value):
                self.value = value

            def __iter__(self):
                return iter(self.value)

        self.assertEqual(self.doit(C((1, 2))), 1)
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            self.doit(C(()))


# TASK(T125845248): After D38227343 is landed and support for COMPARE_OP is in,
# remove UnpackExTests entirely. It will then be covered by UnpackSequenceTests
# above.
class UnpackExTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("UNPACK_EX")
    def doit(self, iterable):
        x, *y = iterable
        return x

    def test_unpack_ex_with_tuple(self) -> None:
        self.assertEqual(self.doit((1, 2)), 1)
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            self.doit(())

    def test_unpack_ex_with_list(self) -> None:
        self.assertEqual(self.doit([1, 2]), 1)
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            self.doit([])

    def test_unpack_ex_with_iterable(self) -> None:
        class C:
            def __init__(self, value):
                self.value = value

            def __iter__(self):
                return iter(self.value)

        self.assertEqual(self.doit(C((1, 2))), 1)
        with self.assertRaisesRegex(ValueError, "not enough values to unpack"):
            self.doit(C(()))


class StoreSubscrTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("STORE_SUBSCR")
    def doit(self, obj, key, value):
        obj[key] = value

    def test_store_subscr_with_list_sets_item(self) -> None:
        obj = [1, 2, 3]
        self.doit(obj, 1, "hello")
        self.assertEqual(obj, [1, "hello", 3])

    def test_store_subscr_with_dict_sets_item(self) -> None:
        obj = {"hello": "cinder"}
        self.doit(obj, "hello", "world")
        self.assertEqual(obj, {"hello": "world"})

    def test_store_subscr_calls_setitem(self) -> None:
        class C:
            def __init__(self):
                self.called = None

            def __setitem__(self, key, value):
                self.called = (key, value)

        obj = C()
        self.doit(obj, "hello", "world")
        self.assertEqual(obj.called, ("hello", "world"))

    def test_store_subscr_deopts_on_exception(self) -> None:
        class C:
            def __setitem__(self, key, value):
                raise TestException("hello")

        obj = C()
        with self.assertRaisesRegex(TestException, "hello"):
            self.doit(obj, 1, 2)


class FormatValueTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("BUILD_STRING", "FORMAT_VALUE")
    def doit(self, obj):
        return f"hello{obj}world"

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("BUILD_STRING", "FORMAT_VALUE")
    def doit_repr(self, obj):
        return f"hello{obj!r}world"

    def test_format_value_calls_str(self) -> None:
        class C:
            def __str__(self):
                return "foo"

        self.assertEqual(self.doit(C()), "hellofooworld")

    def test_format_value_calls_str_with_exception(self) -> None:
        class C:
            def __str__(self):
                raise TestException("no")

        with self.assertRaisesRegex(TestException, "no"):
            self.assertEqual(self.doit(C()))

    def test_format_value_calls_repr(self) -> None:
        class C:
            def __repr__(self):
                return "bar"

        self.assertEqual(self.doit_repr(C()), "hellobarworld")

    def test_format_value_calls_repr_with_exception(self) -> None:
        class C:
            def __repr__(self):
                raise TestException("no")

        with self.assertRaisesRegex(TestException, "no"):
            self.assertEqual(self.doit_repr(C()))


class ListExtendTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LIST_EXTEND")
    def extend_list(self, it):
        return [1, *it]

    def test_list_extend_with_list(self) -> None:
        self.assertEqual(self.extend_list([2, 3, 4]), [1, 2, 3, 4])

    def test_list_extend_with_iterable(self) -> None:
        class A:
            def __init__(self, value):
                self.value = value

            def __iter__(self):
                return iter(self.value)

        extended_list = self.extend_list(A([2, 3]))
        self.assertEqual(type(extended_list), list)
        self.assertEqual(extended_list, [1, 2, 3])

    def test_list_extend_with_non_iterable_raises_type_error(self) -> None:
        err_msg = r"Value after \* must be an iterable, not int"
        with self.assertRaisesRegex(TypeError, err_msg):
            self.extend_list(1)


class SetupWithException(Exception):
    pass


class SetupWithTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("WITH_EXCEPT_START")
    def with_returns_value(self, mgr):
        with mgr as x:
            return x

    def test_with_calls_enter_and_exit(self) -> None:
        class MyCtxMgr:
            def __init__(self):
                self.enter_called = False
                self.exit_args = None

            def __enter__(self):
                self.enter_called = True
                return self

            def __exit__(self, typ, val, tb):
                self.exit_args = (typ, val, tb)
                return False

        mgr = MyCtxMgr()
        self.assertEqual(self.with_returns_value(mgr), mgr)
        self.assertTrue(mgr.enter_called)
        self.assertEqual(mgr.exit_args, (None, None, None))

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("WITH_EXCEPT_START")
    def with_raises(self, mgr):
        with mgr:
            raise SetupWithException("foo")
        return 100

    def test_with_calls_enter_and_exit_exc(self) -> None:
        class MyCtxMgr:
            def __init__(self, should_suppress_exc):
                self.exit_args = None
                self.should_suppress_exc = should_suppress_exc

            def __enter__(self):
                return self

            def __exit__(self, typ, val, tb):
                self.exit_args = (typ, val, tb)
                return self.should_suppress_exc

        mgr = MyCtxMgr(should_suppress_exc=False)
        with self.assertRaisesRegex(SetupWithException, "foo"):
            self.with_raises(mgr)
        self.assertEqual(mgr.exit_args[0], SetupWithException)
        self.assertTrue(isinstance(mgr.exit_args[1], SetupWithException))
        self.assertNotEqual(mgr.exit_args[2], None)

        mgr = MyCtxMgr(should_suppress_exc=True)
        self.assertEqual(self.with_raises(mgr), 100)


class ListToTupleTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def it_to_tup(self, it):
        return (*it,)

    def test_list_to_tuple_returns_tuple(self) -> None:
        new_tup = self.it_to_tup([1, 2, 3, 4])
        self.assertEqual(type(new_tup), tuple)
        self.assertEqual(new_tup, (1, 2, 3, 4))


class CompareTests(unittest.TestCase):
    class Incomparable:
        def __lt__(self, other):
            raise TestException("no lt")

    class NonIterable:
        def __iter__(self):
            raise TestException("no iter")

    class NonIndexable:
        def __getitem__(self, idx):
            raise TestException("no getitem")

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("COMPARE_OP")
    def compare_op(self, left, right):
        return left < right

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("CONTAINS_OP")
    def compare_in(self, left, right):
        return left in right

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("CONTAINS_OP")
    def compare_not_in(self, left, right):
        return left not in right

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("IS_OP")
    def compare_is(self, left, right):
        return left is right

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("IS_OP")
    def compare_is_not(self, left, right):
        return left is not right

    def test_compare_op(self) -> None:
        self.assertTrue(self.compare_op(3, 4))
        self.assertFalse(self.compare_op(3, 3))
        with self.assertRaisesRegex(TestException, "no lt"):
            self.compare_op(self.Incomparable(), 123)

    def test_contains_op(self) -> None:
        self.assertTrue(self.compare_in(3, [1, 2, 3]))
        self.assertFalse(self.compare_in(4, [1, 2, 3]))
        with self.assertRaisesRegex(TestException, "no iter"):
            self.compare_in(123, self.NonIterable())
        with self.assertRaisesRegex(TestException, "no getitem"):
            self.compare_in(123, self.NonIndexable())
        self.assertTrue(self.compare_not_in(4, [1, 2, 3]))
        self.assertFalse(self.compare_not_in(3, [1, 2, 3]))
        with self.assertRaisesRegex(TestException, "no iter"):
            self.compare_not_in(123, self.NonIterable())
        with self.assertRaisesRegex(TestException, "no getitem"):
            self.compare_not_in(123, self.NonIndexable())

    def test_is_op(self) -> None:
        obj = object()
        self.assertTrue(self.compare_is(obj, obj))
        self.assertFalse(self.compare_is(obj, 1))
        self.assertTrue(self.compare_is_not(obj, 1))
        self.assertFalse(self.compare_is_not(obj, obj))


class MatchTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("MATCH_SEQUENCE")
    def match_sequence(self, s: tuple) -> bool:
        match s:
            case (*b, 8, 9, 4, 5):  # noqa: F841
                return True
            case _:
                return False

    def test_match_sequence(self) -> None:
        self.assertTrue(self.match_sequence((1, 2, 3, 7, 8, 9, 4, 5)))
        self.assertFalse(self.match_sequence((1, 2, 3, 4, 5, 6, 7, 8)))

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("MATCH_KEYS", "MATCH_MAPPING")
    def match_keys(self, m: dict) -> bool:
        match m:
            case {"id": 1}:
                return True
            case _:
                return False

    def test_match_keys(self) -> None:
        self.assertTrue(self.match_keys({"id": 1}))
        self.assertFalse(self.match_keys({"id": 2}))

    class A:
        __match_args__ = "id"

        def __init__(self, id):
            self.id = id

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("MATCH_CLASS")
    def match_class(self, a: A) -> bool:
        match a:
            case self.A(id=2):
                return True
            case _:
                return False

    def test_match_class(self) -> None:
        self.assertTrue(self.match_class(self.A(2)))
        self.assertFalse(self.match_class(self.A(3)))

    class Point:
        __match_args__ = 123

        def __init__(self, x, y):
            self.x = x
            self.y = y

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("MATCH_CLASS")
    def match_class_exc(self):
        x, y = 5, 5
        point = self.Point(x, y)
        # will raise because Point.__match_args__ is not a tuple
        match point:
            case self.Point(x, y):
                pass

    def test_match_class_exc(self) -> None:
        with self.assertRaises(TypeError):
            self.match_class_exc()


class CopyDictWithoutKeysTest(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def match_rest(self, obj):
        match obj:
            case {**rest}:
                return rest

    def test_rest_with_empty_dict_returns_empty_dict(self) -> None:
        obj = {}
        result = self.match_rest(obj)
        self.assertIs(type(result), dict)
        self.assertEqual(result, {})
        self.assertIsNot(result, obj)

    def test_rest_with_nonempty_dict_returns_dict_copy(self) -> None:
        obj = {"x": 1}
        result = self.match_rest(obj)
        self.assertIs(type(result), dict)
        self.assertEqual(result, {"x": 1})
        self.assertIsNot(result, obj)

    @cinder_support.failUnlessJITCompiled
    def match_keys_and_rest(self, obj):
        match obj:
            case {"x": 1, **rest}:
                return rest

    def test_keys_and_rest_with_empty_dict_does_not_match(self) -> None:
        result = self.match_keys_and_rest({})
        self.assertIs(result, None)

    def test_keys_and_rest_with_matching_dict_returns_rest(self) -> None:
        obj = {"x": 1, "y": 2}
        result = self.match_keys_and_rest(obj)
        self.assertIs(type(result), dict)
        self.assertEqual(result, {"y": 2})

    def test_with_mappingproxy_returns_dict(self) -> None:
        class C:
            x = 1
            y = 2

        obj = C.__dict__
        self.assertEqual(obj.__class__.__name__, "mappingproxy")
        result = self.match_keys_and_rest(obj)
        self.assertIs(type(result), dict)
        self.assertEqual(result["y"], 2)

    def test_with_abstract_mapping(self) -> None:
        import collections.abc

        class C(collections.abc.Mapping):
            def __iter__(self):
                return iter(("x", "y"))

            def __len__(self):
                return 2

            def __getitem__(self, key):
                if key == "x":
                    return 1
                if key == "y":
                    return 2
                raise RuntimeError("getitem", key)

        obj = C()
        result = self.match_keys_and_rest(obj)
        self.assertIs(type(result), dict)
        self.assertEqual(result, {"y": 2})

    def test_raising_exception_propagates(self) -> None:
        import collections.abc

        class C(collections.abc.Mapping):
            def __iter__(self):
                return iter(("x", "y"))

            def __len__(self):
                return 2

            def __getitem__(self, key):
                raise RuntimeError(f"__getitem__ called with {key}")

        obj = C()
        with self.assertRaisesRegex(RuntimeError, "__getitem__ called with x"):
            self.match_keys_and_rest(obj)


def builtins_getter():
    return _testcindercapi._pyeval_get_builtins()


@unittest.skipIf(AT_LEAST_312, "T214641462: _testcindercapi is only in 3.10.cinder")
class GetBuiltinsTests(unittest.TestCase):
    def test_get_builtins(self) -> None:
        new_builtins = {}
        new_globals = {
            "_testcindercapi": _testcindercapi,
            "__builtins__": new_builtins,
        }
        func = with_globals(new_globals)(builtins_getter)
        force_compile(func)
        self.assertIs(func(), new_builtins)


def globals_getter():
    return globals()


class GetGlobalsTests(unittest.TestCase):
    def test_get_globals(self) -> None:
        new_globals = dict(globals())
        func = with_globals(new_globals)(globals_getter)
        force_compile(func)
        self.assertIs(func(), new_globals)


@unittest.skipIf(AT_LEAST_312, "T214641462: _testcindercapi is only in 3.10.cinder")
class MergeCompilerFlagTests(unittest.TestCase):
    def make_func(self, src, compile_flags=0):
        code = compile(src, "<string>", "exec", compile_flags)
        glbls = {"_testcindercapi": _testcindercapi}
        exec(code, glbls)
        return glbls["func"]

    def run_test(self, callee_src):
        # By default, compile/PyEval_MergeCompilerFlags inherits the compiler
        # flags from the code object of the calling function. We want to ensure
        # that this works even if the function calling compile has no
        # associated Python frame (i.e. it's jitted and we're running in
        # shadow-frame mode).  We arrange a scenario like the following, where
        # callee has a compiler flag that caller does not.
        #
        #   caller (doesn't have CO_FUTURE_BARRY_AS_BDFL)
        #     |
        #     +--- callee (has CO_FUTURE_BARRY_AS_BDFL)
        #            |
        #            +--- compile
        flag = CO_FUTURE_BARRY_AS_BDFL
        caller_src = """
def func(callee):
  return callee()
"""
        caller = self.make_func(caller_src)
        # Force the caller to not be jitted so that it always has a Python
        # frame
        caller = jit_suppress(caller)
        self.assertEqual(caller.__code__.co_flags & flag, 0)

        callee = self.make_func(callee_src, CO_FUTURE_BARRY_AS_BDFL)
        self.assertEqual(callee.__code__.co_flags & flag, flag)
        force_compile(callee)
        flags = caller(callee)
        self.assertEqual(flags & flag, flag)

    def test_merge_compiler_flags(self) -> None:
        """Test that PyEval_MergeCompilerFlags retrieves the compiler flags of the
        calling function."""
        src = """
def func():
  return _testcindercapi._pyeval_merge_compiler_flags()
"""
        self.run_test(src)

    def test_compile_inherits_compiler_flags(self) -> None:
        """Test that compile inherits the compiler flags of the calling function."""
        src = """
def func():
  code = compile('1 + 1', '<string>', 'eval')
  return code.co_flags
"""
        self.run_test(src)


class LoadMethodEliminationTests(unittest.TestCase):
    def lme_test_func(self, flag=False):
        return "{}{}".format(
            1,
            "" if not flag else " flag",
        )

    def test_multiple_call_method_same_load_method(self) -> None:
        self.assertEqual(self.lme_test_func(), "1")
        self.assertEqual(self.lme_test_func(True), "1 flag")
        if compiles_after_one_call():
            self.assertTrue(is_jit_compiled(LoadMethodEliminationTests.lme_test_func))


@unittest.skipUnless(
    cinderx.jit.is_enabled(), "Tests functionality on cinderjit module"
)
class HIROpcodeCountTests(unittest.TestCase):
    def test_hir_opcode_count(self) -> None:
        def f1():
            return 5

        def func():
            return f1() + f1()

        force_compile(func)
        self.assertEqual(func(), 10)

        ops = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertIsInstance(ops, dict)
        self.assertEqual(ops.get("Return"), 1)
        self.assertEqual(ops.get("BinaryOp"), 1)
        self.assertGreaterEqual(ops.get("Decref"), 2)


@unittest.skipUnless(cinderx.jit.is_enabled(), "Testing the cinderjit module itself")
class ForceUncompileTests(unittest.TestCase):
    def test_basic(self) -> None:
        def f(x: int) -> int:
            return x + 1

        self.assertFalse(is_jit_compiled(f))

        self.assertTrue(force_compile(f))
        self.assertTrue(is_jit_compiled(f))

        self.assertTrue(force_uncompile(f))
        self.assertFalse(is_jit_compiled(f))


@unittest.skipUnless(cinderx.jit.is_enabled(), "Testing the cinderjit module itself")
class LazyCompileTests(unittest.TestCase):
    def test_basic(self) -> None:
        def foo(a, b):
            return a + b

        self.assertFalse(is_jit_compiled(foo))
        self.assertTrue(cinderx.jit.lazy_compile(foo))
        foo(1, 2)
        self.assertTrue(is_jit_compiled(foo))


@unittest.skipUnless(cinderx.jit.is_enabled(), "Testing the cinderjit module itself")
class JITSuppressTests(unittest.TestCase):
    def test_basic(self) -> None:
        def f(x: int) -> int:
            return x + 1

        self.assertFalse(is_jit_compiled(f))

        jit_suppress(f)
        f(1)

        with self.assertRaisesRegex(RuntimeError, "CANNOT_SPECIALIZE"):
            force_compile(f)

        self.assertFalse(is_jit_compiled(f))

        jit_unsuppress(f)
        force_compile(f)
        self.assertTrue(is_jit_compiled(f))


@unittest.skipUnless(cinderx.jit.is_enabled(), "Testing the cinderjit module itself")
class BadArgumentTests(unittest.TestCase):
    def test_is_compiled(self) -> None:
        with self.assertRaises(TypeError):
            is_jit_compiled(None)
        with self.assertRaises(TypeError):
            is_jit_compiled(5)
        with self.assertRaises(TypeError):
            is_jit_compiled(is_jit_compiled)

    def test_force_compile(self) -> None:
        with self.assertRaises(TypeError):
            force_compile(None)
        with self.assertRaises(TypeError):
            force_compile(5)
        with self.assertRaises(TypeError):
            force_compile(is_jit_compiled)

    def test_force_uncompile(self) -> None:
        with self.assertRaises(TypeError):
            force_uncompile(None)
        with self.assertRaises(TypeError):
            force_uncompile(5)
        with self.assertRaises(TypeError):
            force_uncompile(is_jit_compiled)

    def test_lazy_compile(self) -> None:
        with self.assertRaises(TypeError):
            cinderx.jit.lazy_compile(None)
        with self.assertRaises(TypeError):
            cinderx.jit.lazy_compile(5)
        with self.assertRaises(TypeError):
            cinderx.jit.lazy_compile(is_jit_compiled)

    def test_jit_suppress(self) -> None:
        with self.assertRaises(TypeError):
            jit_suppress(None)
        with self.assertRaises(TypeError):
            jit_suppress(5)
        with self.assertRaises(TypeError):
            jit_suppress(is_jit_compiled)

    def test_jit_unsuppress(self) -> None:
        with self.assertRaises(TypeError):
            jit_unsuppress(None)
        with self.assertRaises(TypeError):
            jit_unsuppress(5)
        with self.assertRaises(TypeError):
            jit_unsuppress(is_jit_compiled)


@unittest.skipUnless(cinderx.jit.is_enabled(), "Testing the cinderjit module itself")
class CompileTimeTests(unittest.TestCase):
    """
    Test the Cinder APIs that report time spent compiling.
    """

    def test_compile_time(self) -> None:
        # This function is known to have a lengthy compile time.
        from sre_compile import _compile

        # It's probably already compiled as part of regular startup, but just in case
        # let's make sure.
        force_compile(_compile)

        # This will only work if the function takes more than 1ms to compile.  Use the
        # output from PYTHONJITDEBUG=1 to see if that is the case.
        self.assertGreater(cinderx.jit.get_compilation_time(), 0)
        self.assertGreater(cinderx.jit.get_function_compilation_time(_compile), 0)


@unittest.skipUnless(cinderx.jit.is_enabled(), "Testing the cinderjit module itself")
class LocalsBuiltinTests(unittest.TestCase):
    def test_locals_not_compiled(self) -> None:
        def foo():
            locals()

        self.assertFalse(is_jit_compiled(foo))
        with self.assertRaises(RuntimeError):
            force_compile(foo)
        self.assertFalse(is_jit_compiled(foo))


if __name__ == "__main__":
    unittest.main()

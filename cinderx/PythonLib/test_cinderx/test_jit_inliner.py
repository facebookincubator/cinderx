# Copyright (c) Meta Platforms, Inc. and affiliates.

import platform
import traceback
import unittest

import cinderx.jit
from cinderx.jit import jit_suppress
from cinderx.test_support import failUnlessJITCompiled, passIf, passUnless

INLINER: bool = cinderx.jit.is_hir_inliner_enabled()


def firstlineno(func):
    return func.__code__.co_firstlineno


@failUnlessJITCompiled
def func_to_be_inlined(x, y):
    return x + y


@failUnlessJITCompiled
def func_with_defaults(x=1, y=2):
    return x + y


@failUnlessJITCompiled
def func_with_varargs(x, *args):
    return x


@failUnlessJITCompiled
def func():
    a = func_to_be_inlined(2, 3)
    b = func_with_defaults()
    c = func_with_varargs(1, 2, 3)
    return a + b + c


@failUnlessJITCompiled
def get_stack():
    z = 1 + 1  # noqa: F841
    stack = traceback.extract_stack()
    return stack


@failUnlessJITCompiled
def get_stack_twice():
    stacks = []
    stacks.append(get_stack())
    stacks.append(get_stack())
    return stacks


@failUnlessJITCompiled
def get_stack2():
    z = 2 + 2  # noqa: F841
    stack = traceback.extract_stack()
    return stack


@failUnlessJITCompiled
def get_stack_siblings():
    return [get_stack(), get_stack2()]


@failUnlessJITCompiled
def get_stack_multi():
    stacks = []
    stacks.append(traceback.extract_stack())
    z = 1 + 1  # noqa: F841
    stacks.append(traceback.extract_stack())
    return stacks


@failUnlessJITCompiled
def call_get_stack_multi():
    x = 1 + 1  # noqa: F841
    return get_stack_multi()


@failUnlessJITCompiled
def func_with_defaults_that_will_change(x=1, y=2):
    return x + y


@failUnlessJITCompiled
def change_defaults():
    func_with_defaults_that_will_change.__defaults__ = (4, 5)


@failUnlessJITCompiled
def func_that_change_defaults():
    change_defaults()
    return func_with_defaults_that_will_change()


@passUnless(INLINER, "Testing the inliner")
class InlinedFunctionTests(unittest.TestCase):
    @jit_suppress
    @passIf(platform.system() == "Windows", "Currently crashing on Windows")
    def test_deopt_when_func_defaults_change(self) -> None:
        self.assertEqual(
            cinderx.jit.get_num_inlined_functions(func_that_change_defaults), 2
        )
        self.assertEqual(func_that_change_defaults(), 9)

    @jit_suppress
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
    def test_inline_function_stats(self) -> None:
        self.assertEqual(cinderx.jit.get_num_inlined_functions(func), 2)
        stats = cinderx.jit.get_inlined_functions_stats(func)
        self.assertEqual(stats.get("num_inlined_functions"), 2)
        failure_stats = stats.get("failure_stats") or {}
        assert isinstance(failure_stats, dict)
        self.assertNotEqual(failure_stats, {})
        has_varargs = failure_stats.get("HasVarargs") or set()
        assert isinstance(has_varargs, set)
        self.assertNotEqual(has_varargs, {})
        self.assertEqual(len(has_varargs), 1, repr(has_varargs))
        self.assertIn(
            "test_cinderx.test_jit_inliner:func_with_varargs",
            next(iter(has_varargs)),
        )

    @jit_suppress
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


if __name__ == "__main__":
    unittest.main()

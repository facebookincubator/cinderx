# Copyright (c) Meta Platforms, Inc. and affiliates.

import traceback
import unittest

import cinderx.jit
from cinderx.jit import jit_suppress
from cinderx.test_support import failUnlessJITCompiled, passIf, passUnless

INLINER: bool = cinderx.jit.is_hir_inliner_enabled()

# Cold-call pruning ranks callsites by interpreted call counts, so the tests for
# it need to warm functions up in the interpreter before compiling.  If the
# runtime is configured to auto-compile functions by call count, those calls
# stop being counted and the setup can't be established, so skip in that case.
AUTO_COMPILES: bool = cinderx.jit.get_compile_after_n_calls() is not None


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


def add(a: int, b: int) -> int:
    return a + b


def multiply(a: int, b: int) -> int:
    # Manual loop to avoid overhead of range object.
    prod = 0
    i = 0
    while i < b:
        prod = add(prod, a)
        i += 1
    return prod


def fused_multiply_add(a: int, b: int, c: int) -> int:
    return add(multiply(a, b), c)


def chain_leaf(x: int) -> int:
    return x + 1


def chain_mid(x: int) -> int:
    return chain_leaf(x) + 1


def chain_top(x: int) -> int:
    return chain_mid(x) + 1


def call_chain(x: int) -> int:
    return chain_top(x)


def fact(n: int) -> int:
    if n <= 1:
        return 1
    return n * fact(n - 1)


def is_even(n: int) -> bool:
    if n == 0:
        return True
    return is_odd(n - 1)


def is_odd(n: int) -> bool:
    if n == 0:
        return False
    return is_even(n - 1)


# Functions for the cold-call pruning tests.  `hot_callee` is called on every
# invocation of the caller while `cold_callee` is called rarely, so the call to
# cold_callee is a cold callsite that should be pruned.
def hot_callee(x: int) -> int:
    return x + 1


def cold_callee(x: int) -> int:
    return x + 2


def caller_with_cold_call(x: int, take_cold: bool) -> int:
    result = hot_callee(x)
    if take_cold:
        result += cold_callee(x)
    return result


# Functions for pruning a cold call discovered transitively.  `warm_branch` is
# hot and gets inlined into its caller; the call to `cold_leaf` lives inside
# `warm_branch`, so it is only discovered (and must be pruned) at a deeper inline
# depth.
def cold_leaf(x: int) -> int:
    return x + 1


def warm_branch(x: int, take_cold: bool) -> int:
    total = x
    if take_cold:
        total += cold_leaf(x)
    return total


def caller_of_warm_branch(x: int, take_cold: bool) -> int:
    return warm_branch(x, take_cold) + 1


# Regression model for a JIT inliner crash.  `flag_set_loop` is a `while True:` loop
# whose only exit is the KeyError raised by the lookup.  There is no reachable `return`
# inside the loop so when it is inlined its merged exit block has no predecessors and is
# unreachable.  The inliner used to run CopyPropagation and CleanCFG over that stale
# block (left behind because it can't be removed during the inlining loop), which
# dereferenced freed instructions and crashed.
_FLAG_VALUES = {"a": 1, "b": 2, "i": 4, "L": 8}
_LOCALE = 8
_GLOBAL = 16


class FlagSource:
    def __init__(self, chars: str) -> None:
        self._chars = chars
        self.pos = 0

    def get(self) -> str:
        ch = self._chars[self.pos]
        self.pos += 1
        return ch

    def match(self, ch: str) -> bool:
        if self.pos < len(self._chars) and self._chars[self.pos] == ch:
            self.pos += 1
            return True
        return False


class FlagInfo:
    inline_locale: bool = False


def flag_set_loop(source: FlagSource) -> int:
    flags = 0
    saved_pos = 0
    try:
        while True:
            saved_pos = source.pos
            ch = source.get()
            flags |= _FLAG_VALUES[ch]
    except KeyError:
        source.pos = saved_pos
    return flags


def parse_flags(source: FlagSource, info: FlagInfo) -> tuple[int, int]:
    flags_on = flag_set_loop(source)
    if source.match("-"):
        flags_off = flag_set_loop(source)
        if not flags_off:
            raise ValueError("no flags after '-'")
    else:
        flags_off = 0
    if flags_on & _LOCALE:
        info.inline_locale = True
    return flags_on, flags_off


def parse_flags_subpattern(source: FlagSource, info: FlagInfo) -> tuple[int, int]:
    flags_on, flags_off = parse_flags(source, info)
    if flags_off & _GLOBAL:
        raise ValueError("cannot turn off global flag")
    if flags_on & flags_off:
        raise ValueError("flag turned on and off")
    flags_on &= ~_GLOBAL
    return flags_on, flags_off


@passUnless(INLINER, "Testing the inliner")
class InlinedFunctionTests(unittest.TestCase):
    @jit_suppress
    def test_deopt_when_func_defaults_change(self) -> None:
        self.assertEqual(
            cinderx.jit.get_num_inlined_functions(func_that_change_defaults), 2
        )
        self.assertEqual(func_that_change_defaults(), 9)

    @jit_suppress
    def test_recursive_inline_no_calls(self) -> None:
        """Verify that functions can be inlined recursively."""

        # None of the functions have been called, expect all callees to be treated equally.
        cinderx.jit.force_compile(fused_multiply_add)

        # Two top-level calls plus the call to add() from multiply(), means there should
        # be three inlined functions.
        self.assertTrue(cinderx.jit.is_jit_compiled(fused_multiply_add))
        self.assertEqual(cinderx.jit.get_num_inlined_functions(fused_multiply_add), 3)

        self.assertEqual(fused_multiply_add(10, 3, 2), 32)

    @jit_suppress
    def test_deep_transitive_chain(self) -> None:
        """A multi-level call chain is inlined all the way down, and the
        transitively inlined code still computes the right result."""
        cinderx.jit.force_compile(call_chain)

        # call_chain -> chain_top -> chain_mid -> chain_leaf, so chain_top,
        # chain_mid, and chain_leaf should all be inlined.
        self.assertTrue(cinderx.jit.is_jit_compiled(call_chain))
        self.assertEqual(cinderx.jit.get_num_inlined_functions(call_chain), 3)
        self.assertEqual(call_chain(10), 13)

    @jit_suppress
    def test_direct_recursion_is_not_unrolled(self) -> None:
        """A directly recursive function is inlined at most once; the recursive
        call inside the inlined copy is detected and left as a call rather than
        unrolled."""
        cinderx.jit.force_compile(fact)

        self.assertTrue(cinderx.jit.is_jit_compiled(fact))
        # Inlined exactly once -- not zero, not unbounded.
        self.assertEqual(cinderx.jit.get_num_inlined_functions(fact), 1)

        # The recursive call back into fact is recorded as a recursion failure.
        stats = cinderx.jit.get_inlined_functions_stats(fact)
        failure_stats = stats.get("failure_stats") or {}
        assert isinstance(failure_stats, dict)
        recursive = failure_stats.get("IsRecursive") or set()
        assert isinstance(recursive, set)
        self.assertTrue(
            any("test_cinderx.test_jit_inliner:fact" in name for name in recursive),
            repr(recursive),
        )

        # The compiled function still produces correct results.
        self.assertEqual(fact(5), 120)

    @jit_suppress
    def test_mutual_recursion_is_not_unrolled(self) -> None:
        """Mutually recursive functions are detected and not unrolled forever."""
        cinderx.jit.force_compile(is_even)

        self.assertTrue(cinderx.jit.is_jit_compiled(is_even))
        # is_odd then is_even are inlined (one cycle) before the recursion back
        # to is_odd is detected and stopped.
        self.assertEqual(cinderx.jit.get_num_inlined_functions(is_even), 2)

        stats = cinderx.jit.get_inlined_functions_stats(is_even)
        failure_stats = stats.get("failure_stats") or {}
        assert isinstance(failure_stats, dict)
        recursive = failure_stats.get("IsRecursive") or set()
        assert isinstance(recursive, set)
        self.assertTrue(
            any("test_cinderx.test_jit_inliner:is_odd" in name for name in recursive),
            repr(recursive),
        )

        # Correctness of the mutually-recursive computation.
        self.assertTrue(is_even(10))
        self.assertFalse(is_even(7))

    @jit_suppress
    @passIf(AUTO_COMPILES, "Needs interpreted call counts to rank/prune calls")
    def test_cold_call_is_pruned(self) -> None:
        """A callsite that is cold relative to its caller is pruned and not
        inlined, even though the same function inlined elsewhere would fit."""
        # Warm up interpreted call counts: hot_callee runs on every iteration,
        # cold_callee only rarely, while the caller runs many times.
        for i in range(2000):
            caller_with_cold_call(i, i % 1000 == 0)

        # The warmup must have run in the interpreter for call counts to exist.
        self.assertFalse(cinderx.jit.is_jit_compiled(caller_with_cold_call))

        cinderx.jit.force_compile(caller_with_cold_call)
        self.assertTrue(cinderx.jit.is_jit_compiled(caller_with_cold_call))

        # hot_callee is as hot as the caller and is inlined.  cold_callee is
        # called far less often than the caller (a ratio well over the cold-call
        # threshold), so it is pruned -- only one function is inlined, not two.
        self.assertEqual(
            cinderx.jit.get_num_inlined_functions(caller_with_cold_call), 1
        )
        # Still computes the right result.
        self.assertEqual(caller_with_cold_call(10, True), (10 + 1) + (10 + 2))

    @jit_suppress
    @passIf(AUTO_COMPILES, "Needs interpreted call counts to rank/prune calls")
    def test_cold_call_is_pruned_at_deeper_inline_depth(self) -> None:
        """A cold callsite is pruned even when it is only discovered
        transitively, at a deeper inline depth inside an inlined callee."""
        # Warm up: caller and warm_branch run on every iteration, cold_leaf
        # (called from inside warm_branch) only rarely.
        for i in range(2000):
            caller_of_warm_branch(i, i % 1000 == 0)

        self.assertFalse(cinderx.jit.is_jit_compiled(caller_of_warm_branch))

        cinderx.jit.force_compile(caller_of_warm_branch)
        self.assertTrue(cinderx.jit.is_jit_compiled(caller_of_warm_branch))

        # warm_branch is inlined into the caller.  The call to cold_leaf lives
        # inside warm_branch (inline depth 1), and is pruned because cold_leaf is
        # cold relative to warm_branch -- so only warm_branch is inlined.
        self.assertEqual(
            cinderx.jit.get_num_inlined_functions(caller_of_warm_branch), 1
        )
        self.assertEqual(caller_of_warm_branch(10, True), (10 + (10 + 1)) + 1)

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
    def test_inlining_callee_without_reachable_return(self) -> None:
        """Inlining a function whose only loop exit is an exception (so it has
        no reachable return) leaves an unreachable exit block in the caller.
        The inliner must drop that block before running CopyPropagation/CleanCFG;
        otherwise those passes walk freed instructions and crash."""
        cinderx.jit.force_compile(parse_flags_subpattern)
        self.assertTrue(cinderx.jit.is_jit_compiled(parse_flags_subpattern))

        # parse_flags is inlined, and flag_set_loop is inlined transitively
        # through it (once for flags_on, and the source has no "-" so flags_off
        # stays a constant), exercising the unreachable-exit-block path.
        self.assertGreater(
            cinderx.jit.get_num_inlined_functions(parse_flags_subpattern), 1
        )

        # "abiX" turns on flags a|b|i = 7; the trailing "X" is not a flag, so
        # the lookup raises KeyError and ends the loop.  There is no "-", so
        # flags_off is 0.
        self.assertEqual(parse_flags_subpattern(FlagSource("abiX"), FlagInfo()), (7, 0))

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

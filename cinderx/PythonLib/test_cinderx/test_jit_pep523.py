# Copyright (c) Meta Platforms, Inc. and affiliates.

"""
Tests that JIT-compiled code doesn't bypass a PEP 523 frame evaluator, such as
the one Dynamo (`torch.compile`) installs.

`_testinternalcapi.set_eval_frame_record()` stands in for Dynamo's evaluator: it
records the name of every function frame it evaluates.
"""

import functools
import os
import sys
import time
import unittest
from collections.abc import Callable, Iterator
from contextlib import contextmanager

import cinderx
import cinderx.jit
from cinderx.test_support import passIf, passUnless, run_in_fresh_process

try:
    import _testinternalcapi
except ImportError:
    _testinternalcapi = None


# Only on 3.12 does the recording evaluator hand frames on to CinderX's
# evaluator, the way Dynamo's does.
HAS_RECORDING_EVALUATOR: bool = (
    _testinternalcapi is not None
    and hasattr(_testinternalcapi, "set_eval_frame_record")
    and sys.version_info[:2] == (3, 12)
)

# Set to run the tests marked @known_bypass as ordinary tests.
STRICT: bool = os.environ.get("CINDERX_TEST_PEP523_STRICT") == "1"

COMPILE_AFTER_N_CALLS = 10
CALLS = 100


def known_bypass(test: Callable[..., None]) -> Callable[..., None]:
    """
    Marks a test that fails because of a known bypass. It passes while it
    fails, and fails once the bypass is fixed, so the marker gets removed.
    """
    if STRICT:
        return test

    @functools.wraps(test)
    def wrapper(self: unittest.TestCase) -> None:
        try:
            test(self)
        except AssertionError:
            return
        raise AssertionError(f"{test.__name__} now passes; remove @known_bypass")

    return wrapper


def _enable_jit(background: bool = False) -> None:
    cinderx.jit.enable()
    cinderx.jit.compile_after_n_calls(COMPILE_AFTER_N_CALLS)
    cinderx.jit.background_compile(background)


def _is_compiled(func: Callable[..., object]) -> bool:
    return cinderx.jit.get_compiled_size(func) > 0


@contextmanager
def _recording_evaluator() -> Iterator[list[str]]:
    record: list[str] = []
    # pyrefly: ignore [missing-attribute]
    _testinternalcapi.set_eval_frame_record(record)
    try:
        yield record
    finally:
        # pyrefly: ignore [missing-attribute]
        _testinternalcapi.set_eval_frame_default()


def _call_n(func: Callable[[int], int], n: int) -> None:
    for i in range(n):
        func(i)


def _callee(x: int) -> int:
    return x * 2


def _caller(x: int) -> int:
    return _callee(x) + 1


@passUnless(HAS_RECORDING_EVALUATOR, "Needs a recording PEP 523 evaluator")
@passIf(cinderx.is_prefork_build(), "Prefork builds compile differently")
class JitPep523Test(unittest.TestCase):
    @run_in_fresh_process
    @known_bypass
    def test_compiled_while_evaluator_installed(self) -> None:
        _enable_jit()

        def func(x: int) -> int:
            return x + 1

        with _recording_evaluator() as record:
            _call_n(func, CALLS)
        self.assertEqual(record.count("func"), CALLS)

    @run_in_fresh_process
    @known_bypass
    def test_compiled_in_background_while_evaluator_installed(self) -> None:
        _enable_jit(background=True)

        def func(x: int) -> int:
            return x + 1

        with _recording_evaluator() as record:
            deadline = time.monotonic() + 30
            while not _is_compiled(func) and time.monotonic() < deadline:
                _call_n(func, CALLS)
            if not _is_compiled(func):
                self.skipTest("The background compile didn't finish")
            record.clear()
            _call_n(func, CALLS)
        self.assertEqual(record.count("func"), CALLS)

    @run_in_fresh_process
    @known_bypass
    def test_compiled_before_evaluator_installed(self) -> None:
        # Dynamo only installs its evaluator around the code it manages, so
        # functions often get compiled while it isn't installed.
        _enable_jit()

        def func(x: int) -> int:
            return x + 1

        _call_n(func, CALLS)
        with _recording_evaluator() as record:
            _call_n(func, CALLS)
        self.assertEqual(record.count("func"), CALLS)

    @run_in_fresh_process
    @known_bypass
    def test_inlined_callee(self) -> None:
        _enable_jit()
        cinderx.jit.enable_hir_inliner()
        with _recording_evaluator() as record:
            _call_n(_caller, CALLS)
        if cinderx.jit.get_num_inlined_functions(_caller) == 0:
            self.skipTest("_callee was not inlined")
        self.assertEqual(record.count("_caller"), CALLS)
        self.assertEqual(record.count("_callee"), CALLS)

    @run_in_fresh_process
    @known_bypass
    def test_new_closure_of_compiled_code(self) -> None:
        _enable_jit()

        def make() -> Callable[[int], int]:
            def inner(x: int) -> int:
                return x + 1

            return inner

        with _recording_evaluator() as record:
            _call_n(make(), CALLS)
            record.clear()
            _call_n(make(), CALLS)
        self.assertEqual(record.count("inner"), CALLS)

    @run_in_fresh_process
    def test_compiled_while_evaluator_installed_then_removed(self) -> None:
        _enable_jit()

        def func(x: int) -> int:
            return x + 1

        with _recording_evaluator():
            _call_n(func, CALLS)
        self.assertEqual([func(i) for i in range(CALLS)], list(range(1, CALLS + 1)))


if __name__ == "__main__":
    unittest.main()

# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe

import ctypes
import importlib
import multiprocessing
import sys
import tempfile
import unittest

from contextlib import contextmanager
from pathlib import Path

from typing import Callable, Sequence, TypeVar

from cinderx.jit import (
    force_compile,
    # Note: This should use cinderx.jit.is_enabled().
    INSTALLED as CINDERJIT_ENABLED,
    is_enabled as is_jit_enabled,
    is_jit_compiled,
)

try:
    import cinder

    def hasCinderX() -> bool:
        return True

except ImportError:

    def hasCinderX() -> bool:
        return False


def get_cinderjit_xargs() -> list[str]:
    args = []
    for k, v in sys._xoptions.items():
        if not k.startswith("jit"):
            continue
        elif v is True:
            args.extend(["-X", k])
        else:
            args.extend(["-X", f"{k}={v}"])
    return args


def get_await_stack(coro):
    """Return the chain of coroutines reachable from coro via its awaiter"""

    stack = []
    awaiter = cinder._get_coro_awaiter(coro)
    while awaiter is not None:
        stack.append(awaiter)
        awaiter = cinder._get_coro_awaiter(awaiter)
    return stack


def verify_stack(
    testcase: unittest.TestCase, stack: Sequence[str], expected: Sequence[str]
) -> None:
    n = len(expected)
    frames = stack[-n:]
    testcase.assertEqual(len(frames), n, "Callstack had less frames than expected")

    for actual, expected in zip(frames, expected):
        testcase.assertTrue(
            actual.endswith(expected),
            f"The actual frame {actual} doesn't refer to the expected function {expected}",
        )


def _identity(obj: object) -> object:
    return obj


def skip_if_jit(reason: str) -> object:
    return unittest.skip(reason) if is_jit_enabled() else _identity


def skip_unless_jit(reason: str) -> object:
    return unittest.skip(reason) if not is_jit_enabled() else _identity


def failUnlessJITCompiled(func):
    """
    Fail a test if the JIT is enabled but the test body wasn't JIT-compiled.
    """
    if not CINDERJIT_ENABLED:
        return func

    try:
        # force_compile raises a RuntimeError if compilation fails. If it does,
        # defer raising an exception to when the decorated function runs.
        force_compile(func)
    except RuntimeError as re:
        if re.args == ("PYJIT_RESULT_NOT_ON_JITLIST",):
            # We generally only run tests with a jitlist under
            # Tools/scripts/jitlist_bisect.py. In that case, we want to allow
            # the decorated function to run under the interpreter to determine
            # if it's the function the JIT is handling incorrectly.
            return func

        # re is cleared at the end of the except block but we need the value
        # when wrapper() is eventually called.
        exc: RuntimeError = re

        def wrapper(*args: object) -> None:
            raise RuntimeError(
                f"JIT compilation of {func.__qualname__} failed with {exc}"
            )

        return wrapper

    return func


def skip_unless_lazy_imports(
    reason: str = "Depends on Lazy Imports being enabled",
) -> object:
    if not hasattr(importlib, "set_lazy_imports"):
        return unittest.skip(reason)
    return _identity


def is_asan_build() -> bool:
    try:
        ctypes.pythonapi.__asan_init
        return True
    except AttributeError:
        return False


# This is long because ASAN + JIT + subprocess + the Python compiler can be
# pretty slow in CI.
SUBPROCESS_TIMEOUT_SEC = 100 if is_asan_build() else 5


@contextmanager
def temp_sys_path():
    with tempfile.TemporaryDirectory() as tmpdir:
        _orig_sys_modules = sys.modules
        sys.modules = _orig_sys_modules.copy()
        _orig_sys_path = sys.path[:]
        sys.path.insert(0, tmpdir)
        try:
            yield Path(tmpdir)
        finally:
            sys.path[:] = _orig_sys_path
            sys.modules = _orig_sys_modules


TRet = TypeVar("TRet")


def run_in_subprocess(func: Callable[..., TRet]) -> Callable[..., TRet]:
    """
    Run a function in a subprocess.  This enables modifying process state in a
    test without affecting other test functions.
    """

    queue: multiprocessing.Queue = multiprocessing.Queue()

    def wrapper(queue: multiprocessing.Queue, *args: object) -> None:
        result = func(*args)
        queue.put(result, timeout=SUBPROCESS_TIMEOUT_SEC)

    def wrapped(*args: object) -> TRet:
        p = multiprocessing.Process(target=wrapper, args=(queue, *args))
        p.start()
        value = queue.get(timeout=SUBPROCESS_TIMEOUT_SEC)
        p.join(timeout=SUBPROCESS_TIMEOUT_SEC)
        return value

    return wrapped

# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import os
import sys
import unittest

import cinderx.jit
from cinderx.test_support import (
    passIf,
    run_in_fresh_process,
    skip_if_ft,
    skip_unless_jit,
)


@skip_unless_jit("Requires JIT compilation")
@passIf(sys.version_info < (3, 14), "Requires deferred annotations")
class BatchPreloadTests(unittest.TestCase):
    @run_in_fresh_process
    def test_code_replacement_during_preload_single_worker(self) -> None:
        self._check_code_replacement(1)

    @run_in_fresh_process
    def test_code_replacement_during_preload_multi_worker(self) -> None:
        self._check_code_replacement(2)

    def _check_code_replacement(self, workers: int) -> None:
        cinderx.jit.background_compile(False)
        cinderx.jit.enable_emit_type_annotation_guards()
        annotation_calls = []

        def replacement(x: int) -> int:
            return x + 10

        def annotation() -> type:
            annotation_calls.append(True)
            # Whichever function preloads first invalidates itself, the other
            # queued function, and both nested-code registrations.
            a.__code__ = replacement.__code__
            b.__code__ = replacement.__code__
            return int

        # pyre-ignore[11]: Intentional deferred annotation evaluation.
        def a(x: annotation()) -> int:
            def inner() -> int:
                return 1

            return x + inner()

        # pyre-ignore[11]: Intentional deferred annotation evaluation.
        def b(x: annotation()) -> int:
            def inner() -> int:
                return 2

            return x + inner()

        for func in (a, b):
            name = f"{__name__}:{func.__qualname__}"
            cinderx.jit.append_jit_list(name)
            cinderx.jit.append_jit_list(f"{name}.<locals>.inner")

        self.assertTrue(cinderx.jit.precompile_all(workers=workers))
        self.assertEqual(annotation_calls, [True])
        for func in (a, b):
            self.assertFalse(cinderx.jit.is_jit_compiled(func))
            self.assertEqual(func(1), 11)
            self.assertTrue(cinderx.jit.is_jit_compiled(func))

    @run_in_fresh_process
    def test_annotation_error_leaves_replacement_compilable(self) -> None:
        cinderx.jit.background_compile(False)
        cinderx.jit.enable_emit_type_annotation_guards()

        def annotation() -> type:
            raise RuntimeError("batch annotation failed")

        # pyre-ignore[11]: Intentional deferred annotation evaluation.
        def func(x: annotation()) -> int:
            return x

        def replacement(x: int) -> int:
            return x + 10

        cinderx.jit.append_jit_list(f"{__name__}:{func.__qualname__}")
        with self.assertRaisesRegex(RuntimeError, "batch annotation failed"):
            cinderx.jit.precompile_all(workers=2)
        func.__code__ = replacement.__code__
        func.__annotations__ = {"x": int, "return": int}
        self.assertEqual(func(1), 11)
        self.assertTrue(cinderx.jit.is_jit_compiled(func))

    @skip_if_ft("Fork coverage is for the GIL build")
    @passIf(not hasattr(os, "fork"), "fork is unavailable")
    @run_in_fresh_process
    def test_fork_during_batch_preload(self) -> None:
        cinderx.jit.background_compile(False)
        cinderx.jit.enable_emit_type_annotation_guards()
        child_pid = None

        def replacement(x: int) -> int:
            return x + 10

        def annotation() -> type:
            nonlocal child_pid
            child_pid = os.fork()
            func.__code__ = replacement.__code__
            return int

        # pyre-ignore[11]: Intentional deferred annotation evaluation.
        def func(x: annotation()) -> int:
            return x

        cinderx.jit.append_jit_list(f"{__name__}:{func.__qualname__}")
        child_status = 1
        try:
            self.assertTrue(cinderx.jit.precompile_all(workers=2))
            self.assertFalse(cinderx.jit.is_jit_compiled(func))
            self.assertEqual(func(1), 11)
            self.assertTrue(cinderx.jit.is_jit_compiled(func))
            child_status = 0
        finally:
            if child_pid == 0:
                os._exit(child_status)
            if child_pid is not None:
                _, status = os.waitpid(child_pid, 0)
                self.assertEqual(os.waitstatus_to_exitcode(status), 0)

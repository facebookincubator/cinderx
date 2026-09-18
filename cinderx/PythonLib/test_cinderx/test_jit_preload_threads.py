# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import os
import sys
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor

import cinderx.jit
from cinderx.test_support import passIf, run_in_fork, skip_unless_jit


@skip_unless_jit("Requires JIT compilation")
@passIf(sys.version_info < (3, 14), "Requires deferred annotations")
class OverlappingPreloadTests(unittest.TestCase):
    @run_in_fork
    def test_synchronous_preloads_finish_out_of_order(self) -> None:
        self._check_overlapping_preloads(background=False)

    @run_in_fork
    def test_background_preloads_finish_out_of_order(self) -> None:
        self._check_overlapping_preloads(background=True)

    def _check_overlapping_preloads(self, *, background: bool) -> None:
        """A starts, B starts, A finishes, then B finishes.

        B invalidates A while both are preloading. After A returns, the main
        thread invalidates B. Both must observe the change and compile again.
        """
        cinderx.jit.enable_emit_type_annotation_guards()
        cinderx.jit.background_compile(background)
        entered_a = threading.Event()
        entered_b = threading.Event()
        release_b = threading.Event()

        def replacement(x: int) -> int:
            return x + 10

        def annotation_a() -> type:
            entered_a.set()
            self.assertTrue(entered_b.wait(30))
            return int

        def annotation_b() -> type:
            a.__code__ = replacement.__code__
            entered_b.set()
            self.assertTrue(release_b.wait(30))
            return int

        # pyre-ignore[11]: Intentional deferred annotation evaluation.
        def a(x: annotation_a()) -> int:
            return x + 1

        # pyre-ignore[11]: Intentional deferred annotation evaluation.
        def b(x: annotation_b()) -> int:
            return x + 2

        for func in (a, b):
            if background:
                cinderx.jit.append_jit_list(f"{__name__}:{func.__qualname__}")
            else:
                self.assertTrue(cinderx.jit.lazy_compile(func))

        with ThreadPoolExecutor(max_workers=2) as pool:
            future_a = pool.submit(a, 1)
            try:
                self.assertTrue(entered_a.wait(30))
                future_b = pool.submit(b, 1)
                self.assertEqual(future_a.result(timeout=30), 11)
                # A has returned while B is still preloading. B's tracker must
                # survive A's cleanup and observe a mutation on this thread.
                b.__code__ = replacement.__code__
                release_b.set()
                self.assertEqual(future_b.result(timeout=30), 11)
            finally:
                entered_b.set()
                release_b.set()

        cinderx.jit.wait_for_background_compiles()
        for func in (a, b):
            self.assertEqual(func(1), 11)
            cinderx.jit.wait_for_background_compiles()
            self.assertTrue(cinderx.jit.is_jit_compiled(func))

        # Verify deleting a unit after out-of-order preloads does not touch stale state.
        def transient() -> None:
            pass

        del transient

    @run_in_fork
    def test_fork_preserves_current_preload(self) -> None:
        """A preload that forks must still track code changes in both processes."""
        cinderx.jit.enable_emit_type_annotation_guards()
        cinderx.jit.background_compile(False)
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
            return x + 1

        self.assertTrue(cinderx.jit.lazy_compile(func))
        child_status = 1
        try:
            # The first call forks during preload and invalidates the old code.
            # The second must compile the replacement in parent and child.
            self.assertEqual(func(1), 11)
            self.assertEqual(func(1), 11)
            self.assertTrue(cinderx.jit.is_jit_compiled(func))
            child_status = 0
        finally:
            if child_pid == 0:
                os._exit(child_status)
            if child_pid is not None:
                _, status = os.waitpid(child_pid, 0)
                self.assertEqual(os.waitstatus_to_exitcode(status), 0)

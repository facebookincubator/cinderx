# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import os
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor

import cinderx.jit
from cinderx.test_support import run_in_subprocess


def fibonacci(n: int) -> int:
    if n < 2:
        return n
    return fibonacci(n - 1) + fibonacci(n - 2)


class FunctionWatcherTest(unittest.TestCase):
    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_concurrent_qualname_updates(self) -> None:
        worker_count = 10
        iterations = 1_000
        start = threading.Barrier(worker_count)

        def target() -> None:
            pass

        cinderx.jit.jit_suppress(target)
        self.assertFalse(cinderx.jit.is_jit_compiled(target))

        @cinderx.jit.jit_suppress
        def update_qualname(worker: int) -> None:
            start.wait()
            for iteration in range(iterations):
                target.__qualname__ = f"target_{worker}_{iteration}"

        # Unsynchronized updates can underflow the old qualname's refcount.
        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            list(executor.map(update_qualname, range(worker_count)))

        self.assertFalse(cinderx.jit.is_jit_compiled(target))
        self.assertRegex(target.__qualname__, r"^target_\d+_\d+$")


class JITCompilationTest(unittest.TestCase):
    def setUp(self):
        self.bg_compile = cinderx.jit.get_background_compile()
        cinderx.jit.background_compile(False)

    def tearDown(self):
        cinderx.jit.background_compile(self.bg_compile)

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_concurrent_force_compile(self) -> None:
        worker_count = 10
        iterations = 10
        start = threading.Barrier(worker_count)

        cinderx.jit.jit_suppress(fibonacci)
        expected = fibonacci(11)
        cinderx.jit.jit_unsuppress(fibonacci)
        self.assertFalse(cinderx.jit.is_jit_compiled(fibonacci))

        def recompile_and_call(_: int) -> list[int]:
            values: list[int] = []
            start.wait()

            for _ in range(iterations):
                cinderx.jit.force_uncompile(fibonacci)
                cinderx.jit.force_compile(fibonacci)
                values.append(fibonacci(11))

            return values

        cinderx.jit.jit_suppress(recompile_and_call)

        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            results = list(executor.map(recompile_and_call, range(worker_count)))

        self.assertEqual(
            [value for worker in results for value in worker],
            [expected] * (worker_count * iterations),
        )
        self.assertTrue(cinderx.jit.is_jit_compiled(fibonacci))

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_concurrent_calls_trigger_jit_compilation(self) -> None:
        cinderx.jit.compile_after_n_calls(3)

        worker_count = 10
        start = threading.Barrier(worker_count)

        @cinderx.jit.jit_suppress
        def call_fibonacci(_: int) -> int:
            start.wait()
            for _ in range(100):
                fibonacci(11)
            return fibonacci(11)

        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            results = list(executor.map(call_fibonacci, range(worker_count)))

        self.assertEqual(results, [89] * worker_count)
        self.assertTrue(cinderx.jit.is_jit_compiled(fibonacci))

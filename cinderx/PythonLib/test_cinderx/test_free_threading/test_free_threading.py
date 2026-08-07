# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import os
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor

import cinderx.jit
from cinderx.test_support import run_in_subprocess


class FibonacciTest(unittest.TestCase):
    def setUp(self):
        self.bg_compile = cinderx.jit.get_background_compile()
        cinderx.jit.background_compile(False)

    def tearDown(self):
        cinderx.jit.background_compile(self.bg_compile)

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_concurrent_calls_trigger_jit_compilation(self) -> None:
        cinderx.jit.compile_after_n_calls(3)

        def fibonacci(n: int) -> int:
            if n < 2:
                return n
            return fibonacci(n - 1) + fibonacci(n - 2)

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

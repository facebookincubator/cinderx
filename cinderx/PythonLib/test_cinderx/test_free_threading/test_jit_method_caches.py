# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Free-threaded JIT regression tests for method inline caches."""

import threading
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor

import cinderx.jit
from cinderx.test_support import (
    CinderXTestCase,
    FREE_THREADING_BUILD,
    passUnless,
    run_in_subprocess,
)


class MutableMethodTarget:
    value: int

    def method(self) -> int:
        return self.value


def method_one(self: MutableMethodTarget) -> int:
    return 42


def method_two(self: MutableMethodTarget) -> int:
    return 43


class JITMethodCacheTest(CinderXTestCase):
    def assert_compiled_load_method_is_uncached(self, func: Callable[[], int]) -> None:
        self.assertHIROpcodes(
            func,
            present=["LoadMethod"],
            absent=["LoadMethodCached"],
        )

    def assert_concurrent_method_updates_do_not_corrupt_results(
        self,
        load_method: Callable[[], int],
    ) -> None:
        worker_count = 10
        reader_count = worker_count // 2
        writer_count = worker_count - reader_count
        iterations = 5_000
        start = threading.Barrier(worker_count)

        @cinderx.jit.jit_suppress
        def reader() -> bool:
            start.wait()
            for _ in range(iterations):
                if load_method() not in (42, 43):
                    return False
            return True

        @cinderx.jit.jit_suppress
        def writer(worker: int) -> None:
            methods = (method_one, method_two)
            start.wait()
            for i in range(iterations):
                setattr(  # noqa: B010
                    MutableMethodTarget,
                    "method",
                    methods[(i + worker) % len(methods)],
                )

        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            reader_futures = [executor.submit(reader) for _ in range(reader_count)]
            writer_futures = [executor.submit(writer, i) for i in range(writer_count)]

            self.assertEqual(
                [future.result() for future in reader_futures],
                [True] * reader_count,
            )
            for future in writer_futures:
                future.result()

        self.assertIn(load_method(), (42, 43))

    @passUnless(FREE_THREADING_BUILD, "requires free-threaded build")
    @run_in_subprocess
    def test_load_method_is_not_cached_under_free_threading(self) -> None:
        """Exercise method invalidation without JIT method inline caches."""
        target = MutableMethodTarget()
        target.value = 42

        def load_method() -> int:
            return target.method()

        self.assert_compiled_load_method_is_uncached(load_method)
        self.assert_concurrent_method_updates_do_not_corrupt_results(load_method)

# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Free-threaded JIT regression tests for closure cell access."""

import gc
import queue
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor

import cinderx.jit
from cinderx.test_support import run_in_subprocess


class JITCellTest(unittest.TestCase):
    """Exercises closure cells shared by concurrently executing JIT code."""

    @run_in_subprocess
    def test_concurrent_load_deref(self) -> None:
        """A loaded cell value must remain alive while writers replace it."""
        value = "w"
        worker_count = 10
        reader_count = worker_count // 2
        writer_count = worker_count - reader_count
        iterations = 10_000
        start = threading.Barrier(worker_count)

        def reader() -> bool:
            start.wait()
            for _ in range(iterations):
                current = value
                # Access after LOAD_DEREF exposes a borrowed-reference race if a
                # writer replaces and releases the cell value in between.
                if not current.startswith("w"):
                    return False
            return True

        def writer(prefix: str) -> None:
            nonlocal value
            start.wait()
            for i in range(iterations):
                value = f"w{prefix}_{i}"

        self.assertTrue(cinderx.jit.force_compile(reader))
        self.assertTrue(cinderx.jit.force_compile(writer))

        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            reader_futures = [executor.submit(reader) for _ in range(reader_count)]
            writer_futures = [
                executor.submit(writer, str(worker)) for worker in range(writer_count)
            ]

            self.assertEqual(
                [future.result() for future in reader_futures],
                [True] * reader_count,
            )
            for future in writer_futures:
                future.result()

        self.assertTrue(value.startswith("w"))

    @run_in_subprocess
    def test_concurrent_store_deref_releases_each_value_once(self) -> None:
        """Concurrent cell stores must transfer ownership exactly once."""
        finalized: queue.SimpleQueue[int] = queue.SimpleQueue()

        class Value:
            def __init__(self, identifier: int) -> None:
                self.identifier = identifier

            def __del__(self) -> None:
                finalized.put(self.identifier)

        value: Value | None = Value(-1)
        worker_count = 5
        iterations = 10_000
        start = threading.Barrier(worker_count)

        def writer(offset: int) -> None:
            nonlocal value
            start.wait()
            for i in range(iterations):
                value = Value(offset + i)

        self.assertTrue(cinderx.jit.force_compile(writer))

        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            futures = [
                executor.submit(writer, worker * iterations)
                for worker in range(worker_count)
            ]
            self.assertEqual(
                [future.result() for future in futures],
                [None] * worker_count,
            )

        value = None
        gc.collect()
        released = []
        while not finalized.empty():
            released.append(finalized.get())

        # Missing or duplicate identifiers reveal a lost or duplicated ownership
        # transfer when concurrent STORE_DEREF operations replace the same value.
        self.assertCountEqual(released, [-1, *range(iterations * worker_count)])

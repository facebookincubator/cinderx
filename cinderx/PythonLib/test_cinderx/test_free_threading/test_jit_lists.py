# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Free-threaded JIT regression tests for list subscripts."""

import dis
import threading
import unittest
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor

import cinderx.jit
from cinderx.test_support import FREE_THREADING_BUILD, run_in_subprocess


class JITListTest(unittest.TestCase):
    def warm_up_list_opcode(
        self,
        read_item: Callable[[list[str]], str],
    ) -> None:
        values = ["w"]
        cinderx.jit.jit_suppress(read_item)
        for _ in range(100):
            read_item(values)
        cinderx.jit.jit_unsuppress(read_item)
        opnames = {
            instruction.opname
            for instruction in dis.get_instructions(read_item, adaptive=True)
        }
        self.assertIn("BINARY_OP_SUBSCR_LIST_INT", opnames)

    def exercise_concurrent_access(
        self,
        read_item: Callable[[list[str]], str],
    ) -> None:
        worker_count = 10
        reader_count = worker_count // 2
        writer_count = worker_count - reader_count
        iterations = 10_000
        start = threading.Barrier(worker_count)
        values = ["w"]

        @cinderx.jit.jit_suppress
        def reader() -> bool:
            start.wait()
            for _ in range(iterations):
                if not read_item(values).startswith("w"):
                    return False
            return True

        @cinderx.jit.jit_suppress
        def writer(prefix: str) -> None:
            start.wait()
            for i in range(iterations):
                values[0] = f"w{prefix}_{i}"

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

        self.assertTrue(values[0].startswith("w"))

    @run_in_subprocess
    def test_concurrent_subscript_without_specialized_opcodes(self) -> None:
        """Keep generic HIR when compiling an adaptive list opcode."""
        cinderx.jit.disable_specialized_opcodes()

        def read_item(values: list[str]) -> str:
            return values[0]

        self.warm_up_list_opcode(read_item)

        self.assertTrue(cinderx.jit.force_compile(read_item))
        opcode_counts = cinderx.jit.get_function_hir_opcode_counts(read_item)
        if opcode_counts is None:
            self.fail("No HIR opcode counts for compiled read_item")
        self.assertIn("BinaryOp", opcode_counts)
        self.assertNotIn("ListSubscr", opcode_counts)
        self.assertNotIn("LoadArrayItem", opcode_counts)

        self.exercise_concurrent_access(read_item)

    @unittest.skipUnless(FREE_THREADING_BUILD, "requires free-threaded build")
    @run_in_subprocess
    def test_concurrent_subscript_with_simplify(self) -> None:
        """Use the owned-reference ListSubscr path for exact lists."""
        cinderx.jit.enable_specialized_opcodes()

        def read_item(values: list[str]) -> str:
            return values[0]

        self.warm_up_list_opcode(read_item)

        self.assertTrue(cinderx.jit.force_compile(read_item))
        opcode_counts = cinderx.jit.get_function_hir_opcode_counts(read_item)
        if opcode_counts is None:
            self.fail("No HIR opcode counts for compiled read_item")
        self.assertIn("ListSubscr", opcode_counts)
        self.assertNotIn("LoadArrayItem", opcode_counts)

        self.exercise_concurrent_access(read_item)

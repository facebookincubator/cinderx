# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Free-threaded JIT regression tests for dict subscripts."""

import dis
import threading
import unittest
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor

import cinderx.jit
from cinderx.test_support import run_in_subprocess


class JITDictTest(unittest.TestCase):
    def warm_up_dict_opcode(
        self,
        read_item: Callable[[dict[str, str]], str],
    ) -> None:
        values = {"key": "w"}
        cinderx.jit.jit_suppress(read_item)
        for _ in range(100):
            read_item(values)
        cinderx.jit.jit_unsuppress(read_item)
        opnames = {
            instruction.opname
            for instruction in dis.get_instructions(read_item, adaptive=True)
        }
        self.assertIn("BINARY_OP_SUBSCR_DICT", opnames)

    def exercise_concurrent_access(
        self,
        read_item: Callable[[dict[str, str]], str],
    ) -> None:
        worker_count = 10
        reader_count = worker_count // 2
        writer_count = worker_count - reader_count
        iterations = 10_000
        start = threading.Barrier(worker_count)
        values = {"key": "w"}

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
                values["key"] = f"w{prefix}_{i}"
                # Unique transient keys force periodic key-table replacement.
                transient_key = f"transient_{prefix}_{i}"
                values[transient_key] = "w"
                del values[transient_key]

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

        self.assertTrue(values["key"].startswith("w"))

    def exercise_concurrent_reads(
        self,
        read_item: Callable[[dict[str, str]], str],
        values: dict[str, str],
        expected: str,
    ) -> int:
        worker_count = 10
        iterations = 10_000
        start = threading.Barrier(worker_count)

        @cinderx.jit.jit_suppress
        def reader(_: int) -> bool:
            start.wait()
            for _ in range(iterations):
                if read_item(values) != expected:
                    return False
            return True

        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            results = list(executor.map(reader, range(worker_count)))

        self.assertEqual(results, [True] * worker_count)
        return worker_count * iterations

    @run_in_subprocess
    def test_concurrent_subscript_without_specialized_opcodes(self) -> None:
        """Keep generic HIR when compiling an adaptive dict opcode."""
        cinderx.jit.disable_specialized_opcodes()

        def read_item(values: dict[str, str]) -> str:
            return values["key"]

        self.warm_up_dict_opcode(read_item)

        self.assertTrue(cinderx.jit.force_compile(read_item))
        opcode_counts = cinderx.jit.get_function_hir_opcode_counts(read_item)
        if opcode_counts is None:
            self.fail("No HIR opcode counts for compiled read_item")
        self.assertIn("BinaryOp", opcode_counts)
        self.assertNotIn("DictSubscr", opcode_counts)

        self.exercise_concurrent_access(read_item)

    @run_in_subprocess
    def test_concurrent_subscript_with_simplify(self) -> None:
        """Stress DictSubscr during concurrent value and key-table mutation."""
        cinderx.jit.enable_specialized_opcodes()

        def read_item(values: dict[str, str]) -> str:
            return values["key"]

        self.warm_up_dict_opcode(read_item)

        self.assertTrue(cinderx.jit.force_compile(read_item))
        opcode_counts = cinderx.jit.get_function_hir_opcode_counts(read_item)
        if opcode_counts is None:
            self.fail("No HIR opcode counts for compiled read_item")
        self.assertIn("DictSubscr", opcode_counts)
        self.assertNotIn("BinaryOp", opcode_counts)

        self.exercise_concurrent_access(read_item)

    @run_in_subprocess
    def test_concurrent_deopt_on_guard_failure(self) -> None:
        """Concurrent dict-subclass calls deopt at the exact-dict guard."""
        cinderx.jit.enable_specialized_opcodes()

        def read_item(values: dict[str, str]) -> str:
            return values["key"]

        self.warm_up_dict_opcode(read_item)
        self.assertTrue(cinderx.jit.force_compile(read_item))

        class OverrideDict(dict[str, str]):
            def __getitem__(self, key: str) -> str:
                return "override"

        cinderx.jit.get_and_clear_runtime_stats()
        call_count = self.exercise_concurrent_reads(
            read_item,
            OverrideDict(key="stored"),
            "override",
        )
        deopts = cinderx.jit.get_and_clear_runtime_stats()["deopt"]
        if not isinstance(deopts, list):
            self.fail("Deopt runtime stats are not a list")
        read_item_deopts = [
            deopt
            for deopt in deopts
            if deopt["normal"]["func_qualname"] == read_item.__qualname__
        ]
        self.assertTrue(read_item_deopts)
        for deopt in read_item_deopts:
            self.assertEqual(deopt["normal"]["reason"], "GuardFailure")
            self.assertEqual(deopt["normal"]["description"], "GuardType")
        self.assertEqual(
            sum(deopt["int"]["count"] for deopt in read_item_deopts), call_count
        )

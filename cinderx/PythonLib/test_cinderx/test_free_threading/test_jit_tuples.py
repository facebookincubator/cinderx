# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Free-threaded JIT regression tests for tuple subscripts."""

import dis
import threading
from collections.abc import Callable, Sequence
from concurrent.futures import ThreadPoolExecutor

import cinderx.jit
from cinderx.test_support import CinderXTestCase, run_in_subprocess


class JITTupleTest(CinderXTestCase):
    def warm_up_tuple_opcode(
        self,
        read_item: Callable[[Sequence[object]], object],
    ) -> None:
        values = (object(),)
        # Specialize in CPython first so Simplify can use the resulting type
        # guards to emit LoadArrayItem.
        cinderx.jit.jit_suppress(read_item)
        for _ in range(100):
            read_item(values)
        cinderx.jit.jit_unsuppress(read_item)
        opnames = {
            instruction.opname
            for instruction in dis.get_instructions(read_item, adaptive=True)
        }
        self.assertIn("BINARY_OP_SUBSCR_TUPLE_INT", opnames)

    def exercise_concurrent_reads(
        self,
        read_item: Callable[[Sequence[object]], object],
        values: Sequence[object],
    ) -> int:
        worker_count = 10
        iterations = 10_000
        start = threading.Barrier(worker_count)
        expected = values[0]

        @cinderx.jit.jit_suppress
        def reader(_: int) -> bool:
            start.wait()
            for _ in range(iterations):
                if read_item(values) is not expected:
                    return False
            return True

        with ThreadPoolExecutor(max_workers=worker_count) as executor:
            results = list(executor.map(reader, range(worker_count)))

        self.assertEqual(results, [True] * worker_count)
        return worker_count * iterations

    @run_in_subprocess
    def test_concurrent_subscript_without_specialized_opcodes(self) -> None:
        """Keep generic HIR when compiling an adaptive tuple opcode."""
        cinderx.jit.disable_specialized_opcodes()

        def read_item(values: Sequence[object]) -> object:
            return values[0]

        self.warm_up_tuple_opcode(read_item)
        self.assertHIROpcodes(
            read_item,
            present=["BinaryOp"],
            absent=["LoadArrayItem"],
        )

        values = (object(),)
        self.exercise_concurrent_reads(read_item, values)

    @run_in_subprocess
    def test_concurrent_subscript_with_simplify(self) -> None:
        """Keep the direct LoadArrayItem path for exact tuples.

        Exact tuples are immutable, so Simplify can retain the borrowed
        LoadArrayItem path that mutable lists cannot use in free-threaded
        builds. The HIR assertions pin that optimization decision.
        """
        cinderx.jit.enable_specialized_opcodes()

        def read_item(values: Sequence[object]) -> object:
            return values[0]

        self.warm_up_tuple_opcode(read_item)
        self.assertHIROpcodes(
            read_item,
            present=["LoadArrayItem"],
            absent=["BinaryOp"],
        )

        values = (object(),)
        self.exercise_concurrent_reads(read_item, values)

    @run_in_subprocess
    def test_concurrent_deopt_on_guard_failure(self) -> None:
        """Concurrent list calls deopt at the exact-tuple guard."""
        cinderx.jit.enable_specialized_opcodes()

        def read_item(values: Sequence[object]) -> object:
            return values[0]

        self.warm_up_tuple_opcode(read_item)
        self.assertTrue(cinderx.jit.force_compile(read_item))

        cinderx.jit.get_and_clear_runtime_stats()
        call_count = self.exercise_concurrent_reads(read_item, [object()])
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

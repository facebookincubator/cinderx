# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import gc
import multiprocessing
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from typing import Any

import cinderx

from cinderx.test_support import CINDERX_PATH, ENCODING, passUnless


def _run_gc_settings(result_queue: "multiprocessing.Queue[tuple[int, Any]]") -> None:
    """Child process function to test gc.get_threshold()."""
    try:
        result = gc.get_threshold()
        result_queue.put((0, result))
    except Exception as e:
        result_queue.put((1, str(e)))


def _run_parallel_gc_settings(
    result_queue: "multiprocessing.Queue[tuple[int, Any]]",
) -> None:
    """Child process function to test cinderx.get_parallel_gc_settings()."""
    try:
        result = cinderx.get_parallel_gc_settings()
        result_queue.put((0, result))
    except Exception as e:
        result_queue.put((1, str(e)))


@passUnless(cinderx.has_parallel_gc(), "Testing Parallel GC Enablement")
class TestEnablingParallelGc(unittest.TestCase):
    def test_gc_settings(self) -> None:
        # Set environment variables for the child process
        os.environ["PARALLEL_GC_ENABLED"] = "1"
        os.environ["PARALLEL_GC_THRESHOLD_GEN0"] = "1000"
        os.environ["PARALLEL_GC_THRESHOLD_GEN1"] = "5"
        os.environ["PARALLEL_GC_THRESHOLD_GEN2"] = "5"

        try:
            ctx = multiprocessing.get_context("spawn")
            result_queue: multiprocessing.Queue[tuple[int, Any]] = ctx.Queue()
            proc = ctx.Process(target=_run_gc_settings, args=(result_queue,))
            proc.start()
            proc.join(timeout=30)

            self.assertEqual(proc.exitcode, 0)
            returncode, result = result_queue.get(timeout=5)
            self.assertEqual(returncode, 0)
            self.assertEqual(result, (1000, 5, 5))
        finally:
            proc.terminate()
            del os.environ["PARALLEL_GC_ENABLED"]
            del os.environ["PARALLEL_GC_THRESHOLD_GEN0"]
            del os.environ["PARALLEL_GC_THRESHOLD_GEN1"]
            del os.environ["PARALLEL_GC_THRESHOLD_GEN2"]

    def test_parallel_gc_settings(self) -> None:
        # Set environment variables for the child process
        os.environ["PARALLEL_GC_ENABLED"] = "1"
        os.environ["PARALLEL_GC_NUM_THREADS"] = "4"
        os.environ["PARALLEL_GC_MIN_GENERATION"] = "2"

        try:
            ctx = multiprocessing.get_context("spawn")
            result_queue: multiprocessing.Queue[tuple[int, Any]] = ctx.Queue()
            proc = ctx.Process(target=_run_parallel_gc_settings, args=(result_queue,))
            proc.start()
            proc.join(timeout=30)

            self.assertEqual(proc.exitcode, 0)
            returncode, result = result_queue.get(timeout=5)
            self.assertEqual(returncode, 0)
            self.assertEqual(result, {"num_threads": 4, "min_generation": 2})
        finally:
            proc.terminate()
            del os.environ["PARALLEL_GC_ENABLED"]
            del os.environ["PARALLEL_GC_NUM_THREADS"]
            del os.environ["PARALLEL_GC_MIN_GENERATION"]

    def test_parallel_gc_failure_high_min_gen_number(self) -> None:
        codestr = textwrap.dedent("""
            import cinderx
            def g():
                return cinderx.get_parallel_gc_settings()
            print(g())
        """)
        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            codepath.write_text(codestr)
            args = [sys.executable]
            args.append("mod.py")
            proc = subprocess.run(
                args,
                cwd=tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding=ENCODING,
                env={
                    "PYTHONPATH": CINDERX_PATH,
                    "PARALLEL_GC_ENABLED": "1",
                    "PARALLEL_GC_NUM_THREADS": "4",
                    "PARALLEL_GC_MIN_GENERATION": "10",
                },
            )
            self.assertEqual(proc.returncode, 1, proc)


if __name__ == "__main__":
    unittest.main()

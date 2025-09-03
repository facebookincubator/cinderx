# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import cinderx

from cinderx.test_support import CINDERX_PATH, ENCODING


@unittest.skipUnless(cinderx.has_parallel_gc(), "Testing Parallel GC Enablement")
class TestEnablingParallelGc(unittest.TestCase):
    def test_gc_settings(self) -> None:
        codestr = textwrap.dedent("""
            import gc
            def g():
                return gc.get_threshold()
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
                encoding=ENCODING,
                env={
                    "PYTHONPATH": CINDERX_PATH,
                    "PARALLEL_GC_ENABLED": "1",
                    "PARALLEL_GC_THRESHOLD_GEN0": "1000",
                    "PARALLEL_GC_THRESHOLD_GEN1": "5",
                    "PARALLEL_GC_THRESHOLD_GEN2": "5",
                },
            )
            self.assertEqual(proc.returncode, 0, proc)
            actual_stdout = list(proc.stdout.strip().split("\n"))
            self.assertEqual(actual_stdout, ["(1000, 5, 5)"])

    def test_parallel_gc_settings(self) -> None:
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
                encoding=ENCODING,
                env={
                    "PYTHONPATH": CINDERX_PATH,
                    "PARALLEL_GC_ENABLED": "1",
                    "PARALLEL_GC_NUM_THREADS": "4",
                    "PARALLEL_GC_MIN_GENERATION": "2",
                },
            )
            self.assertEqual(proc.returncode, 0, proc)
            actual_stdout = list(proc.stdout.strip().split("\n"))
            self.assertEqual(actual_stdout, ["{'num_threads': 4, 'min_generation': 2}"])

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

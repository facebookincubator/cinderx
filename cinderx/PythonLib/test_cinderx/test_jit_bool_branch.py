# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Regression test for gh-115: a JIT-compiled branch warmed on bools
must not lose its bool guard; without it truthy non-bools take the
false path."""

import subprocess
import sys
import unittest

import cinderx

cinderx.init()

from cinderx.test_support import ENCODING, subprocess_env


def run_snippet(source: str) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(
        [sys.executable, "-c", source],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding=ENCODING,
        env=subprocess_env(),
    )


class JitBoolBranchTests(unittest.TestCase):
    @unittest.skipUnless(sys.version_info >= (3, 14), "3.14+ opcodes")
    def test_jit_branch_on_non_bool_after_bool_warmup(self) -> None:
        proc = run_snippet(
            """if 1:
            import cinderx.jit
            cinderx.jit.auto()
            # Keep the warm-up interpreted so it specializes TO_BOOL_BOOL
            # before we compile.
            cinderx.jit.compile_after_n_calls(10**9)

            def check(x):
                if x:
                    return "T"
                return "F"

            for _ in range(600):
                assert check(True) == "T"
                assert check(False) == "F"

            assert cinderx.jit.force_compile(check)

            for val, want in [
                (True, "T"), (False, "F"),
                ((1, 2), "T"), ((), "F"),
                (5, "T"), ("", "F"),
                ({"k": 1}, "T"),
            ]:
                got = check(val)
                assert got == want, f"check({val!r}) = {got!r}, want {want!r}"
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stdout)


if __name__ == "__main__":
    unittest.main()

# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""All JIT "size limit" tests in one place.

Two kinds of limit are covered:
  * Per-function LIR size (``jit-max-lir-instrs`` / ``jit-max-lir-blocks``),
    which abort native code generation and fall back to the interpreter.
  * Total emitted machine-code size (``jit-max-code-size``), which stops the
    JIT from compiling more functions once the process has emitted too much.

Limits that are only settable at startup are exercised in fresh subprocesses;
``max_code_size`` also has a runtime setter, exercised in-process.
"""

import signal
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from typing import Callable

import cinderx.jit
from cinderx.jit import force_compile, is_jit_compiled
from cinderx.test_support import ENCODING, passUnless, subprocess_env


# Compiling any real function produces more than one LIR instruction and, for a
# branchy function, more than one LIR basic block, so these tiny limits are
# guaranteed to trip regardless of Python version or architecture.
_TINY_INSTRS_LIMIT = "jit-max-lir-instrs=1"
_TINY_BLOCKS_LIMIT = "jit-max-lir-blocks=1"
_WINDOWS_STATUS_STACK_BUFFER_OVERRUN = 0xC0000409

# A trivial straight-line function: >1 LIR instruction, 1 LIR basic block.
_STRAIGHT_LINE = """
import cinderx.jit as jit


def sample(x):
    return x + 1


try:
    jit.force_compile(sample)
except RuntimeError:
    # force_compile raises when compilation aborts; the JIT should still fall
    # back to the interpreter rather than propagate a hard failure.
    pass

assert not jit.is_jit_compiled(sample), "expected interpreter fallback"
assert sample(41) == 42, "interpreter execution produced wrong result"
print("FALLBACK_OK")
"""

# A branchy function: multiple LIR basic blocks.
_BRANCHY = """
import cinderx.jit as jit


def branchy(x):
    if x > 0:
        return x * 2
    return -x


try:
    jit.force_compile(branchy)
except RuntimeError:
    pass

assert not jit.is_jit_compiled(branchy), "expected interpreter fallback"
assert branchy(5) == 10, "interpreter execution produced wrong result"
assert branchy(-3) == 3, "interpreter execution produced wrong result"
print("FALLBACK_OK")
"""

# Control: the same straight-line function compiles fine under the default
# limits, proving the fallback above is caused by the limit and nothing else.
_STRAIGHT_LINE_CONTROL = """
import cinderx.jit as jit


def sample(x):
    return x + 1


jit.force_compile(sample)
assert jit.is_jit_compiled(sample), "expected JIT compilation under default limits"
assert sample(41) == 42
print("COMPILED_OK")
"""


class SizeLimitTest(unittest.TestCase):
    """Size-limit tests that run in fresh subprocesses.

    The LIR limits are only settable at startup, and the max-code-size tests
    want a pristine allocator, so each case spawns a new interpreter with the
    relevant ``-X`` flags rather than mutating this process's JIT state.
    """

    def _run_child(
        self, source: str, extra_args: list[str]
    ) -> subprocess.CompletedProcess[str]:
        args = [sys.executable, *extra_args, "-c", source]
        return subprocess.run(
            args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding=ENCODING,
            env=subprocess_env(),
        )

    def _assert_child_printed(
        self, proc: subprocess.CompletedProcess[str], token: str
    ) -> None:
        self.assertEqual(
            proc.returncode,
            0,
            f"child failed\nstdout={proc.stdout!r}\nstderr={proc.stderr!r}",
        )
        self.assertIn(token, proc.stdout)

    def test_lir_instrs_limit_triggers_fallback(self) -> None:
        proc = self._run_child(_STRAIGHT_LINE, ["-X", _TINY_INSTRS_LIMIT])
        self._assert_child_printed(proc, "FALLBACK_OK")

    def test_lir_blocks_limit_triggers_fallback(self) -> None:
        proc = self._run_child(_BRANCHY, ["-X", _TINY_BLOCKS_LIMIT])
        self._assert_child_printed(proc, "FALLBACK_OK")

    def test_compiles_under_default_limit(self) -> None:
        proc = self._run_child(_STRAIGHT_LINE_CONTROL, [])
        self._assert_child_printed(proc, "COMPILED_OK")

    def test_max_code_size_slow(self) -> None:
        # TODO(T240152676): Improve stability of this test
        call_limit = cinderx.jit.get_compile_after_n_calls()
        if call_limit is None or call_limit > 10000:
            # Expecting the JIT to be compiling a bunch of code automatically
            return

        code = textwrap.dedent(
            """
            import cinderx.jit
            for i in range(2000):
                exec(f'''
            def junk{i}(j):
                j = j + 1
                s = f'dogs {i} ' + str(j)
                if s == '23':
                    j += 2
                return j*2+{i}
            ''')
            # Call a handful of functions enough times to exceed the
            # jit-auto threshold (up to 1000) so the JIT compiles them.
            # The remaining functions are called once each below.
            for _rep in range(1100):
                junk0(0)
                junk1(1)
            x = 0
            for i in range(2000):
                exec(f'x *= junk{i}(i)')
            max_bytes = cinderx.jit.get_allocator_stats()["max_bytes"]
            used_bytes = cinderx.jit.get_allocator_stats()["used_bytes"]
            print(f'max_size: {max_bytes}')
            print(f'used_size: {used_bytes}')
        """
        )
        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            codepath.write_text(code)

            def run_test(
                asserts_func: Callable[[list[str]], None], params: list[str]
            ) -> None:
                args = [sys.executable]
                args.extend(params)
                args.append("mod.py")
                proc = subprocess.run(
                    args,
                    cwd=tmp,
                    stdout=subprocess.PIPE,
                    encoding=ENCODING,
                    env=subprocess_env(),
                )
                self.assertEqual(proc.returncode, 0, proc)
                actual_stdout = [x.strip() for x in proc.stdout.split("\n")]
                asserts_func(actual_stdout)

            def zero_asserts(actual_stdout: list[str]) -> None:
                expected_stdout = "max_size: 0"
                self.assertEqual(actual_stdout[0], expected_stdout)
                self.assertIn("used_size", actual_stdout[1])
                used_size = int(actual_stdout[1].split(" ")[1])
                self.assertGreater(used_size, 0)

            def onek_asserts(actual_stdout: list[str]) -> None:
                expected_stdout = "max_size: 1024"
                self.assertEqual(actual_stdout[0], expected_stdout)
                self.assertIn("used_size", actual_stdout[1])
                used_size = int(actual_stdout[1].split(" ")[1])
                self.assertGreater(used_size, 1024)
                # This is a bit fragile because it depends on what the initial 'zeroth'
                # allocation is; we assume < 600K.
                self.assertLess(used_size, 1024 * 600)

            # Run the zero-assert tests with JitAuto=1000 to test "normal" behavior
            # where we compile some code but don't have any limits to trip.
            run_test(zero_asserts, ["-X", "jit-auto=1000", "-X", "jit-max-code-size=0"])
            run_test(
                zero_asserts,
                [
                    "-X",
                    "jit-auto=1000",
                    "-X",
                    "jit-max-code-size=0",
                    "-X",
                    "jit-huge-pages=0",
                ],
            )

            # Run the onek-assert tests with JitAll so that we quickly trip the limit
            # and stop compiling.
            run_test(onek_asserts, ["-X", "jit-all", "-X", "jit-max-code-size=1024"])
            run_test(
                onek_asserts,
                [
                    "-X",
                    "jit-all",
                    "-X",
                    "jit-max-code-size=1024",
                    "-X",
                    "jit-huge-pages=0",
                ],
            )

    def test_max_code_size_fast(self) -> None:
        code = textwrap.dedent(
            """
            import cinderx.jit
            max_bytes = cinderx.jit.get_allocator_stats()["max_bytes"]
            print(f'max_size: {max_bytes}')
        """
        )
        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            codepath.write_text(code)

            def run_proc(size: str | None = None) -> str:
                args = [sys.executable]
                if size:
                    args.extend(["-X", f"jit-max-code-size={size}"])
                args.append("mod.py")

                proc = subprocess.run(
                    args,
                    cwd=tmp,
                    stdout=subprocess.PIPE,
                    encoding=ENCODING,
                    env=subprocess_env(),
                )
                self.assertEqual(proc.returncode, 0, proc)
                actual_stdout = [x.strip() for x in proc.stdout.split("\n")]
                return actual_stdout[0]

            self.assertEqual(run_proc(), "max_size: 0")
            self.assertEqual(run_proc("1234567"), "max_size: 1234567")
            self.assertEqual(run_proc("1k"), "max_size: 1024")
            self.assertEqual(run_proc("1K"), "max_size: 1024")
            self.assertEqual(run_proc("1m"), "max_size: 1048576")
            self.assertEqual(run_proc("1M"), "max_size: 1048576")
            self.assertEqual(run_proc("1g"), "max_size: 1073741824")
            self.assertEqual(run_proc("1G"), "max_size: 1073741824")

            def run_proc(size: str) -> str:
                args = [
                    sys.executable,
                    "-X",
                    f"jit-max-code-size={size}",
                    "mod.py",
                ]
                proc = subprocess.run(
                    args,
                    cwd=tmp,
                    stderr=subprocess.PIPE,
                    encoding=ENCODING,
                    env=subprocess_env(),
                )
                expected_returncode = (
                    _WINDOWS_STATUS_STACK_BUFFER_OVERRUN
                    if sys.platform == "win32"
                    else -signal.SIGABRT
                )
                self.assertEqual(proc.returncode, expected_returncode, proc)
                return proc.stderr

            self.assertIn(
                "Invalid unsigned integer in input string: '-1'", run_proc("-1")
            )
            self.assertIn(
                "Invalid unsigned integer in input string: '1.1'", run_proc("1.1")
            )
            self.assertIn("Invalid character in input string", run_proc("dogs"))
            self.assertIn(
                "Unsigned Integer overflow in input string: '1152921504606846976g'",
                run_proc("1152921504606846976g"),
            )


class MaxCodeSizeApiTest(unittest.TestCase):
    """In-process tests for the runtime max-code-size setter.

    These mutate global JIT config, so they require the JIT to be enabled and
    restore the original limit on exit.
    """

    def test_set_max_code_size(self) -> None:
        # Test with valid positive value
        cinderx.jit.set_max_code_size(100_000_000)

        # Test with zero (unlimited)
        cinderx.jit.set_max_code_size(0)

        # Test invalid types
        with self.assertRaises(TypeError):
            # pyre-ignore[6]: Intentional type error.
            cinderx.jit.set_max_code_size(None)
        with self.assertRaises(TypeError):
            # pyre-ignore[6]: Intentional type error.
            cinderx.jit.set_max_code_size("100M")
        with self.assertRaises(TypeError):
            # pyre-ignore[6]: Intentional type error.
            cinderx.jit.set_max_code_size(100.5)

        # Test negative value
        with self.assertRaises(ValueError):
            cinderx.jit.set_max_code_size(-1)
        with self.assertRaises(ValueError):
            cinderx.jit.set_max_code_size(-100)

    def test_max_code_size_prevents_compilation(self) -> None:
        # Setup: Get current allocator stats and set a very small limit
        stats = cinderx.jit.get_allocator_stats()
        if stats is None:
            self.skipTest("Allocator stats not available")

        # Save original limit to restore later
        original_limit = cinderx.jit.get_allocator_stats()["max_bytes"]

        try:
            # Make the limit infinite
            cinderx.jit.set_max_code_size(0)

            # Create a function that should compile successfully
            def small_func1():
                return 42

            force_compile(small_func1)
            self.assertTrue(is_jit_compiled(small_func1))

            # Set a very small limit to prevent further compilations
            cinderx.jit.set_max_code_size(5)

            # Create another function that should NOT compile due to limit
            def small_func2():
                return 43

            with self.assertRaisesRegex(RuntimeError, "PYJIT_OVER_MAX_CODE_SIZE"):
                force_compile(small_func2)

            self.assertFalse(is_jit_compiled(small_func2))

        finally:
            cinderx.jit.set_max_code_size(original_limit)

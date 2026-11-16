# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from cinderx.test_support import (
    ENCODING,
    skip_if_prefork,
    skip_unless_jit,
    subprocess_env,
)


# Enable the native ABI calling convention for JIT-to-JIT calls.  Huge pages are
# disabled so the stale-code reclamation path actually unmaps freed code
# (CodeAllocatorCinder::releaseCode is a no-op).
_JIT_TO_JIT_ENV: dict[str, str] = {
    **subprocess_env(),
    "CINDERX_JIT_AUTO": "1",
    "CINDERX_JIT_TO_JIT_CALLS": "1",
    "CINDERX_JIT_HUGE_PAGES": "0",
    "CINDERX_JIT_ENABLE_HIR_INLINER": "0",
}


@skip_unless_jit("Exercises the JIT-to-JIT native calling convention")
@skip_if_prefork(
    "JIT-to-JIT calls are disabled under the prefork model: runtime call-site "
    "patching would un-share copy-on-write code pages across forked workers"
)
class JitToJitCallsTest(unittest.TestCase):
    """
    Tests for direct register-ABI calls between JIT-compiled non-Static-Python
    functions.  A compiled caller emits a relative call to the link trampoline;
    on first execution the link shim patches the call site to the callee's
    register entry.  When the callee becomes stale (recompiled/deallocated) its
    register entry is stamped to the relink trampoline, which reverts the call
    site, and the deferred-cleanup GC eventually frees the code.
    """

    def _run(self, code: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp:
            mod = Path(tmp) / "mod.py"
            mod.write_text(textwrap.dedent(code))
            return subprocess.run(
                [sys.executable, str(mod)],
                cwd=tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding=ENCODING,
                env=_JIT_TO_JIT_ENV,
            )

    def test_direct_call_correctness(self) -> None:
        """
        A compiled caller repeatedly calls a compiled callee with a fixed
        positional argument count.  The first call binds through the link
        trampoline and patches the call site to a direct register-ABI call; all
        subsequent calls take the patched fast path.  The accumulated result
        proves every call returned correctly across that transition.
        """
        proc = self._run(
            """
            import cinderx.jit

            def callee(a, b, c):
                return a * 100 + b * 10 + c

            def caller(n):
                total = 0
                for i in range(n):
                    total += callee(i, i + 1, i + 2)
                return total

            cinderx.jit.force_compile(callee)
            cinderx.jit.force_compile(caller)
            assert cinderx.jit.is_jit_compiled(callee)
            assert cinderx.jit.is_jit_compiled(caller)

            N = 5000
            expected = sum(i * 100 + (i + 1) * 10 + (i + 2) for i in range(N))
            result = caller(N)
            assert result == expected, (result, expected)

            print("OK")
        """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_relink_after_callee_recompile(self) -> None:
        """
        After a call site is patched to the callee's register entry, de-opting
        the callee stamps that entry to the relink trampoline.  Continuing to
        call the caller must transparently divert through the relink trampoline
        (reverting the call site) and still produce correct results -- never
        jumping into stale code.
        """
        proc = self._run(
            """
            import gc

            import cinderx.jit

            def callee(x):
                return x + 1

            def caller(n):
                total = 0
                for i in range(n):
                    total += callee(i)
                return total

            cinderx.jit.force_compile(callee)
            cinderx.jit.force_compile(caller)

            # Warm up: patch the call site to callee's register entry.
            assert caller(1000) == sum(i + 1 for i in range(1000))

            # De-opt the callee: drops the last CompiledFunction reference, which
            # stamps its register entry to the relink trampoline and defers the
            # code.  Patched callers must relink on their next call.
            assert cinderx.jit.force_uncompile(callee)
            assert not cinderx.jit.is_jit_compiled(callee)

            # Force generation-2 GCs to drive deferred-code reclamation while the
            # caller keeps invoking the (now interpreted) callee.
            for _ in range(3):
                assert caller(1000) == sum(i + 1 for i in range(1000))
                gc.collect()

            print("OK")
        """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_many_callers_one_callee_relink(self) -> None:
        """
        Many distinct compiled callers all patch direct calls into one shared
        callee.  De-opting the callee must revert every caller (via the callee's
        reverse index of callers) and keep producing correct results.
        """
        proc = self._run(
            """
            import gc

            import cinderx.jit

            def callee(x):
                return x + 1

            # Build many distinct caller functions sharing the one callee.
            callers = []
            for _ in range(50):
                ns = {"callee": callee}
                exec(
                    "def caller(n):\\n"
                    "    total = 0\\n"
                    "    for i in range(n):\\n"
                    "        total += callee(i)\\n"
                    "    return total\\n",
                    ns,
                )
                callers.append(ns["caller"])

            cinderx.jit.force_compile(callee)
            for c in callers:
                cinderx.jit.force_compile(c)

            expected = sum(i + 1 for i in range(100))
            # Warm up: patch every caller's call site to callee's register entry.
            for c in callers:
                assert c(100) == expected

            # De-opt the callee: the deferred GC must revert every caller.
            assert cinderx.jit.force_uncompile(callee)
            assert not cinderx.jit.is_jit_compiled(callee)

            for _ in range(3):
                for c in callers:
                    assert c(100) == expected
                gc.collect()

            print("OK")
        """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_caller_freed_before_callee(self) -> None:
        """
        Free a caller's compiled code *before* the callee it links to.  The
        caller must be detached from the callee's reverse index, so the later
        callee reclaim never dereferences the freed caller's call sites.
        """
        proc = self._run(
            """
            import gc

            import cinderx.jit

            def callee(x):
                return x + 1

            def make_caller():
                ns = {"callee": callee}
                exec(
                    "def caller(n):\\n"
                    "    total = 0\\n"
                    "    for i in range(n):\\n"
                    "        total += callee(i)\\n"
                    "    return total\\n",
                    ns,
                )
                return ns["caller"]

            caller = make_caller()
            cinderx.jit.force_compile(callee)
            cinderx.jit.force_compile(caller)

            expected = sum(i + 1 for i in range(100))
            assert caller(100) == expected  # links caller -> callee

            # Free the CALLER's compiled code first, while it is a tracked caller
            # of the callee.  Its reverse-index edge must be detached.
            assert cinderx.jit.force_uncompile(caller)
            del caller
            gc.collect()

            # Now free the callee too; must not crash on a dangling caller.
            assert cinderx.jit.force_uncompile(callee)
            gc.collect()
            assert callee(5) == 6

            print("OK")
        """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_cyclic_callers_relink(self) -> None:
        """
        Mutually-recursive compiled functions link into each other (A<->B).
        De-opting both must tear down the reverse-index edges in both directions
        without crashing, and both must keep computing correctly afterward.
        """
        proc = self._run(
            """
            import gc

            import cinderx.jit

            def a(n):
                if n <= 0:
                    return 0
                return 1 + b(n - 1)

            def b(n):
                if n <= 0:
                    return 0
                return 1 + a(n - 1)

            cinderx.jit.force_compile(a)
            cinderx.jit.force_compile(b)
            assert a(50) == 50
            assert b(50) == 50

            assert cinderx.jit.force_uncompile(a)
            assert cinderx.jit.force_uncompile(b)
            for _ in range(3):
                assert a(50) == 50
                assert b(50) == 50
                gc.collect()

            print("OK")
        """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_callee_code_change(self) -> None:
        proc = self._run(
            """
            import cinderx.jit

            def callee(x):
                return x + 1

            def caller(x):
                return callee(x)

            def zero():
                return 42

            def same(x):
                return x + 100

            assert cinderx.jit.force_compile(callee)
            assert cinderx.jit.force_compile(caller)
            assert caller(1) == 2
            assert caller(1) == 2
            callee.__code__ = zero.__code__
            for _ in range(2):
                try:
                    caller(1)
                except TypeError:
                    pass
                else:
                    raise AssertionError("argument count was lost")
            callee.__code__ = same.__code__
            assert cinderx.jit.force_compile(callee)
            assert caller(1) == 101
            assert caller(2) == 102
            print("OK")
            """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_shared_closure_code_change(self) -> None:
        proc = self._run(
            """
            import cinderx.jit

            def make(offset):
                def inner(x):
                    return offset + x
                return inner

            def make_replacement(offset):
                def inner(x):
                    return offset - x
                return inner

            f, g = make(10), make(20)

            def caller_f(x):
                return f(x)

            def caller_g(x):
                return g(x)

            for func in (f, g, caller_f, caller_g):
                assert cinderx.jit.force_compile(func)
            held = cinderx.jit.get_compiled_function(f)
            assert held is cinderx.jit.get_compiled_function(g)
            assert caller_f(1) == 11
            assert caller_g(1) == 21
            f.__code__ = make_replacement(0).__code__
            for _ in range(2):
                assert caller_f(1) == 9
                assert caller_g(1) == 21
            print("OK")
            """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_suspended_caller_survives_callee_cleanup(self) -> None:
        proc = self._run(
            """
            import gc

            import cinderx.jit

            def callee(x):
                return x + 1

            def caller(n):
                for i in range(n):
                    yield callee(i)

            assert cinderx.jit.force_compile(callee)
            assert cinderx.jit.force_compile(caller)
            gen = caller(3)
            assert next(gen) == 1
            assert cinderx.jit.force_uncompile(caller)
            gc.collect()
            assert cinderx.jit.force_uncompile(callee)
            gc.collect()
            assert list(gen) == [2, 3]
            del gen
            gc.collect()
            print("OK")
            """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_argument_count_boundaries(self) -> None:
        proc = self._run(
            """
            import gc

            import cinderx.jit

            for count in (0, 1, 3, 4, 5, 6, 7, 8):
                names = ", ".join(f"a{i}" for i in range(count))
                values = ", ".join(str(i + 10) for i in range(count))
                ns = {}
                exec(
                    f"def callee({names}):\\n"
                    f"    return ({names}{',' if count else ''})\\n"
                    f"def caller():\\n"
                    f"    return callee({values})\\n",
                    ns,
                )
                callee, caller = ns["callee"], ns["caller"]
                assert cinderx.jit.force_compile(callee)
                assert cinderx.jit.force_compile(caller)
                expected = tuple(range(10, 10 + count))
                assert caller() == expected
                assert caller() == expected
                assert cinderx.jit.force_uncompile(callee)
                gc.collect()
                assert caller() == expected
                assert cinderx.jit.force_compile(callee)
                assert caller() == expected
                assert caller() == expected

            print("OK")
            """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_exception_through_link_and_relink(self) -> None:
        proc = self._run(
            """
            import cinderx.jit

            def callee(x):
                if x < 0:
                    raise ValueError("negative input")
                return x + 1

            def caller(x):
                return callee(x)

            assert cinderx.jit.force_compile(callee)
            assert cinderx.jit.force_compile(caller)
            for generation in range(3):
                for _ in range(2):
                    try:
                        caller(-1)
                    except ValueError as exc:
                        assert str(exc) == "negative input"
                    else:
                        raise AssertionError("exception was lost")
                    assert caller(1) == 2
                if generation == 0:
                    assert cinderx.jit.force_uncompile(callee)
                elif generation == 1:
                    assert cinderx.jit.force_compile(callee)
            print("OK")
            """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_kwargs_call_falls_back(self) -> None:
        """
        Calls that do not meet the register-ABI conditions (here, keyword
        arguments) must fall back to the vectorcall path and still work.
        """
        proc = self._run(
            """
            import cinderx.jit

            def callee(a, b):
                return a - b

            def caller(n):
                total = 0
                for i in range(n):
                    total += callee(a=i, b=1)
                return total

            cinderx.jit.force_compile(callee)
            cinderx.jit.force_compile(caller)

            N = 2000
            assert caller(N) == sum(i - 1 for i in range(N))
            print("OK")
        """
        )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

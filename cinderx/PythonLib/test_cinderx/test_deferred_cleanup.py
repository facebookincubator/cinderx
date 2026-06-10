# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from cinderx.test_support import ENCODING, skip_unless_jit, subprocess_env


# Each subprocess must use the allocator that actually unmaps freed code
# (CodeAllocator) rather than the huge-pages allocator (CodeAllocatorCinder)
# whose releaseCode() is a no-op.  Without this, freeing deferred code would
# never reclaim memory and a use-after-free would not crash, defeating the
# point of these tests.
_HUGE_PAGES_OFF: dict[str, str] = {
    **subprocess_env(),
    "CINDERX_JIT_HUGE_PAGES": "0",
}


@skip_unless_jit("Exercises JIT-compiled code lifetime")
class DeferredCleanupTest(unittest.TestCase):
    """
    Tests for the deferred freeing of JIT-compiled code (the
    CompiledFunctionData held by a CompiledFunction).

    When a function is de-opted while frames executing its compiled code are
    still on the stack, the machine code must NOT be freed immediately --
    returning through those frames would execute freed (and, with huge pages
    disabled, unmapped) memory.  Instead the code is deferred and only reclaimed
    later, during a generation-2 GC, once no live frame references it.  See
    Context::processDeferredCleanup() and Jit/pyjit.cpp's gc_callback().
    """

    def _run_subprocess(self, code: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as tmp:
            mod = Path(tmp) / "mod.py"
            mod.write_text(textwrap.dedent(code))
            return subprocess.run(
                [sys.executable, str(mod)],
                cwd=tmp,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding=ENCODING,
                env=_HUGE_PAGES_OFF,
            )

    def test_deopt_during_recursion_defers_code(self) -> None:
        """
        De-opt a JIT-compiled function from the bottom of a deep recursion, so
        every outer frame is still running its compiled code, then return all
        the way out.  The deferred-cleanup mechanism must keep the code alive
        until the frames unwind, otherwise we crash on the return path.
        """
        proc = self._run_subprocess("""
            import cinderx.jit

            DEPTH = 100
            COMPILED_KEY = "__cinderx_compiled_func__"

            def recursive(n):
                if n > 0:
                    return recursive(n - 1) + n
                # Bottom of the recursion: every outer frame (and this one) is
                # executing JIT-compiled code for `recursive`.  Dropping the
                # last reference to the CompiledFunction runs its destructor,
                # which must DEFER freeing the machine code because it is still
                # on the stack above us.
                del recursive.__dict__[COMPILED_KEY]
                assert not cinderx.jit.is_jit_compiled(recursive)
                return 0

            cinderx.jit.force_compile(recursive)
            assert cinderx.jit.is_jit_compiled(recursive)
            assert COMPILED_KEY in recursive.__dict__

            # Returning through the still-live JIT frames must not crash even
            # though the code was logically freed at the bottom of the
            # recursion.  The correct sum proves the frames ran to completion.
            result = recursive(DEPTH)
            assert result == DEPTH * (DEPTH + 1) // 2, result

            # The function stays de-opted after unwinding.
            assert not cinderx.jit.is_jit_compiled(recursive)
            assert COMPILED_KEY not in recursive.__dict__

            print("OK")
        """)
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_deferred_code_freed_by_gen2_gc(self) -> None:
        """
        After the de-opting recursion unwinds, the deferred code should be
        reclaimed by a generation-2 GC (which fires the cinderx GC callback ->
        Context::processDeferredCleanup()).  Running the whole cycle repeatedly
        and forcing full collections must reclaim the code without crashing.
        """
        proc = self._run_subprocess("""
            import gc

            import cinderx.jit

            DEPTH = 50
            COMPILED_KEY = "__cinderx_compiled_func__"

            def make_and_deopt():
                # A fresh function each round so its compiled code is distinct
                # and becomes eligible for freeing once we de-opt it.
                ns = {}
                exec(
                    "def recursive(n, deopt):\\n"
                    "    if n > 0:\\n"
                    "        return recursive(n - 1, deopt) + n\\n"
                    "    deopt()\\n"
                    "    return 0\\n",
                    ns,
                )
                recursive = ns["recursive"]

                cinderx.jit.force_compile(recursive)
                assert cinderx.jit.is_jit_compiled(recursive)

                def deopt():
                    # De-opt while DEPTH compiled frames are still live.
                    del recursive.__dict__[COMPILED_KEY]

                result = recursive(DEPTH, deopt)
                assert result == DEPTH * (DEPTH + 1) // 2, result
                assert not cinderx.jit.is_jit_compiled(recursive)

            for _ in range(20):
                make_and_deopt()
                # Generation-2 collection triggers deferred cleanup of any code
                # whose frames are no longer on the stack.
                gc.collect()

            print("OK")
        """)
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)


if __name__ == "__main__":
    unittest.main()

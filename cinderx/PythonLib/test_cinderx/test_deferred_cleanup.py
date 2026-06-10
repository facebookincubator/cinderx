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

    def test_deopt_generator_keeps_code_alive_while_suspended(self) -> None:
        """
        A suspended generator keeps its JIT-compiled code alive.

        Unlike a plain function, a generator can be suspended at a yield point
        with a JIT frame that is NOT on any thread's stack -- so the deferred
        cleanup stack-walk cannot protect it.  Instead the suspended generator
        holds a strong reference to the CompiledFunction, so dropping the
        function dict's reference does NOT free the code.  Resuming the
        generator afterwards must run the still-live code and produce correct
        results without crashing.
        """
        proc = self._run_subprocess("""
            import gc

            import cinderx.jit

            COMPILED_KEY = "__cinderx_compiled_func__"

            def gen(n):
                acc = 0
                for i in range(n):
                    acc += i
                    yield acc

            cinderx.jit.force_compile(gen)
            assert cinderx.jit.is_jit_compiled(gen)
            assert COMPILED_KEY in gen.__dict__

            # Suspend the generator mid-execution: it now has a live JIT frame
            # holding the loop state across the yield.
            g = gen(5)
            assert next(g) == 0

            # Drop the function dict's reference to the CompiledFunction.  The
            # suspended generator keeps it (and its machine code) alive, so the
            # function is NOT deopted while the generator is live -- this is the
            # guarantee that makes resuming safe.
            del gen.__dict__[COMPILED_KEY]
            assert COMPILED_KEY not in gen.__dict__
            assert cinderx.jit.is_jit_compiled(gen)

            # A generation-2 GC reclaims deferred code whose frames are gone.
            # The suspended frame is not on any stack, so only the generator's
            # strong reference keeps the code from being freed here.
            gc.collect()
            assert cinderx.jit.is_jit_compiled(gen)

            # Resuming the suspended JIT frame must not crash and must produce
            # the right values, proving the code stayed alive.
            assert list(g) == [1, 3, 6, 10]

            # Once the generator is gone the CompiledFunction is released and
            # the function finally deopts -- still no crash.
            del g
            gc.collect()
            assert not cinderx.jit.is_jit_compiled(gen)

            print("OK")
        """)
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)

    def test_many_suspended_generators_survive_gc(self) -> None:
        """
        Many generators suspended at once all share a single CompiledFunction.
        After dropping the function dict's reference, repeated full collections
        must not free the code out from under any of the live suspended frames,
        and every generator must resume to completion without crashing.
        """
        proc = self._run_subprocess("""
            import gc

            import cinderx.jit

            COMPILED_KEY = "__cinderx_compiled_func__"

            def gen(n):
                acc = 0
                for i in range(n):
                    acc += i
                    yield acc

            cinderx.jit.force_compile(gen)
            assert cinderx.jit.is_jit_compiled(gen)

            gens = [gen(5) for _ in range(10)]
            for g in gens:
                assert next(g) == 0

            # The suspended generators are now the only thing keeping the
            # compiled code alive.
            del gen.__dict__[COMPILED_KEY]

            for _ in range(3):
                gc.collect()
                assert cinderx.jit.is_jit_compiled(gen)

            # Resume every generator to completion, interleaving collections.
            for g in gens:
                assert list(g) == [1, 3, 6, 10]
                gc.collect()

            del gens, g
            gc.collect()
            assert not cinderx.jit.is_jit_compiled(gen)

            print("OK")
        """)
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("OK", proc.stdout, proc)


if __name__ == "__main__":
    unittest.main()

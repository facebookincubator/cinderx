# Copyright (c) Meta Platforms, Inc. and affiliates.

import ctypes
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from cinderx.test_support import skip_unless_jit, subprocess_env

# Standalone check run in a subprocess: flag values are read once at JIT
# init, so the check cannot enable the flag in-process. The subprocess
# runs with libgcc frame registration enabled (it is off by default), so
# glibc backtrace() can unwind through JIT frames.
_CHECK_SCRIPT = textwrap.dedent(
    """
    import ctypes
    import gc
    import sys

    import cinderx.jit

    _backtrace_fn = None
    _backtrace_loaded = False


    def _load_backtrace():
        global _backtrace_fn, _backtrace_loaded
        if _backtrace_loaded:
            return _backtrace_fn
        _backtrace_loaded = True
        try:
            # Look `backtrace` up in libc itself: the executable can define
            # its own (a statically linked libunwind exports one), and the
            # global lookup would then bypass the libgcc unwinder this
            # check exercises.
            backtrace = ctypes.CDLL("libc.so.6", use_errno=True).backtrace
            backtrace.restype = ctypes.c_int
            backtrace.argtypes = [
                ctypes.POINTER(ctypes.c_void_p),
                ctypes.c_int,
            ]
            _backtrace_fn = backtrace
        except (OSError, AttributeError):
            _backtrace_fn = None
        return _backtrace_fn


    def _capture_backtrace():
        backtrace = _load_backtrace()
        buf = (ctypes.c_void_p * 64)()
        count = backtrace(buf, len(buf))
        return [buf[i] for i in range(count)]


    def _inner():
        return _capture_backtrace()


    # Called through `globals()` so the HIR inliner cannot fold the callee
    # into the caller: each function must keep its own JIT frame on the
    # stack.
    def _middle():
        return globals()["_inner"]()


    def _outer():
        return globals()["_middle"]()


    def _gen_inner():
        yield _capture_backtrace()


    def _gen_outer():
        return next(_gen_inner())


    def _executable_anon_ranges():
        ranges = []
        with open("/proc/self/maps") as maps:
            for line in maps:
                parts = line.split()
                # Anonymous executable mappings are JIT code
                # (write-protected to RX after emission).  File-backed
                # mappings carry a path.
                if len(parts) == 5 and "x" in parts[1]:
                    lo, hi = parts[0].split("-")
                    ranges.append((int(lo, 16), int(hi, 16)))
        return ranges


    def _jit_frames(frames):
        ranges = _executable_anon_ranges()
        return [
            addr for addr in frames if any(lo <= addr < hi for lo, hi in ranges)
        ]


    def _assert_compiled(func):
        cinderx.jit.force_compile(func)
        assert cinderx.jit.is_jit_compiled(func)


    def _check_plain():
        for func in (_inner, _middle, _outer):
            _assert_compiled(func)
        frames = _outer()
        assert len(frames) >= 8
        # One return address per JIT function, in anonymous executable
        # memory, with the walk continuing past them into the interpreter.
        assert len(_jit_frames(frames)) >= 3


    def _check_generator():
        _assert_compiled(_gen_inner)
        _assert_compiled(_gen_outer)
        frames = _gen_outer()
        assert len(frames) >= 8
        # The generator body runs with the frame pointer swapped to its
        # heap footer; the walk must still pass through its code.
        assert len(_jit_frames(frames)) >= 1


    def _check_uncompile():
        for func in (_inner, _middle, _outer):
            _assert_compiled(func)
        # Freeing a compile deregisters its unwind info.  A later walk
        # through freshly compiled code must not trip over the old frame.
        cinderx.jit.force_uncompile(_outer)
        gc.collect()
        _assert_compiled(_outer)
        frames = _outer()
        assert len(frames) >= 8
        assert len(_jit_frames(frames)) >= 3


    _CASES = {
        "plain": _check_plain,
        "generator": _check_generator,
        "uncompile": _check_uncompile,
    }

    if __name__ == "__main__":
        _CASES[sys.argv[1]]()
        print("OK")
    """
)


@skip_unless_jit("JIT is not enabled")
class TestJitUnwind(unittest.TestCase):
    def setUp(self) -> None:
        super().setUp()
        # JIT unwind registration and glibc backtrace() are Linux-only.
        if sys.platform != "linux":
            self.skipTest("requires Linux and glibc backtrace()")
        try:
            ctypes.CDLL("libc.so.6", use_errno=True).backtrace
        except (OSError, AttributeError):
            self.skipTest("requires Linux and glibc backtrace()")

    def _run_case(self, case: str) -> None:
        # Libgcc frame registration is off by default; run the check in a
        # subprocess with it enabled, so glibc backtrace() can unwind
        # through JIT frames.
        with tempfile.TemporaryDirectory() as tmp_dir:
            script = Path(tmp_dir) / "check_jit_unwind.py"
            script.write_text(_CHECK_SCRIPT)
            proc = subprocess.run(
                [sys.executable, str(script), case],
                check=True,
                capture_output=True,
                encoding="utf-8",
                env={
                    **subprocess_env(),
                    "CINDERX_JIT_UNWIND_LIBGCC_FRAME_REGISTRATION": "1",
                },
            )
            self.assertIn("OK", proc.stdout, proc)

    def test_backtrace_passes_through_jit_frames(self) -> None:
        self._run_case("plain")

    def test_backtrace_through_generator_frame(self) -> None:
        self._run_case("generator")

    def test_backtrace_after_uncompile_and_gc(self) -> None:
        self._run_case("uncompile")

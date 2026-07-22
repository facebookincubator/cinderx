# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Stress test for the JIT compilation pipeline.

Generates control-flow-heavy functions and force-compiles them so that the
expensive HIR passes and the backend all get a real workout on a single
function:

  * SSAify           -- many accumulators redefined across many merge points,
                        which produces large iterated dominance frontiers and a
                        lot of Phis.
  * Simplify         -- a long chain of ``isinstance`` branches (the case that
                        historically drove Simplify's cost).
  * CleanCFG / DCE   -- a large basic-block count, so every dominator-tree
                        rebuild in ``removeUnreachableInstructions`` is
                        expensive (this is what the cached ``Function::domTree``
                        is meant to avoid recomputing across passes).
  * Linear-scan RA   -- many accumulators kept live across the whole body,
                        creating wide, overlapping live ranges.

The test asserts the functions compile, and prints the JIT compile time so the
numbers can be compared across builds (e.g. with and without the dominator-tree
cache).  Sizes are overridable via environment variables for A/B measurement.
"""

from __future__ import annotations

import os
import time
import unittest
from collections.abc import Callable
from typing import cast

import cinderx
import cinderx.jit
from cinderx.test_support import FREE_THREADING_BUILD, passIf


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.environ[name])
    except (KeyError, ValueError):
        return default


def _env_float(name: str, default: float) -> float:
    try:
        return float(os.environ[name])
    except (KeyError, ValueError):
        return default


# Tunable knobs.  Defaults make each individual compile heavy (so most of the
# time is spent inside the compiler), and the test then compiles fresh copies
# back-to-back until it has burned ~_TARGET_SECONDS of wall time.  All are
# overridable via the environment for A/B compile-time measurement.
#
# _SEGMENTS is kept just under the JIT's internal size ceiling (functions much
# larger than this are refused with PYJIT_RESULT_UNKNOWN_ERROR), so each compile
# is as heavy as the compiler will accept.
_SEGMENTS: int = _env_int("CINDERX_JIT_STRESS_SEGMENTS", 120)
_NUM_VARS: int = _env_int("CINDERX_JIT_STRESS_VARS", 8)
_NEST_DEPTH: int = _env_int("CINDERX_JIT_STRESS_DEPTH", 4)
_TARGET_SECONDS: float = _env_float("CINDERX_JIT_STRESS_SECONDS", 5.0)

_TYPES: tuple[str, ...] = (
    "int",
    "float",
    "str",
    "bytes",
    "list",
    "tuple",
    "dict",
    "set",
)


def _gen_source(name: str, segments: int, num_vars: int, nest_depth: int) -> str:
    """Emit source for one control-flow-heavy function.

    Each segment is an ``isinstance`` diamond that rewrites every accumulator
    (producing Phis at the merge), and every fourth segment additionally nests
    ``nest_depth`` conditionals to deepen the dominator tree.
    """
    params = ", ".join(f"a{i}" for i in range(num_vars))
    lines: list[str] = [f"def {name}(seq, {params}):"]
    for i in range(num_vars):
        lines.append(f"    v{i} = a{i}")
    lines.append("    total = 0")
    # A loop gives us a back edge (loop-header dominance) around the whole body.
    lines.append("    for x in seq:")
    for s in range(segments):
        t = _TYPES[s % len(_TYPES)]
        # isinstance diamond: reassign every accumulator -> a Phi per var.
        lines.append(f"        if isinstance(x, {t}):")
        for i in range(num_vars):
            lines.append(f"            v{i} = v{(i + 1) % num_vars} + {s + 1}")
        # Occasionally nest conditionals to grow dominator-tree depth.
        if s % 4 == 0:
            for d in range(nest_depth):
                indent = " " * (12 + d * 4)
                lines.append(f"{indent}if v{(s + d) % num_vars} > {d}:")
            indent = " " * (12 + nest_depth * 4)
            lines.append(f"{indent}v{s % num_vars} = v{(s + 1) % num_vars} - {s + 1}")
        lines.append(f"        total = total + v{s % num_vars}")
    lines.append("    return total")
    return "\n".join(lines)


def _make_function(
    name: str, segments: int, num_vars: int, nest_depth: int
) -> Callable[..., int]:
    source = _gen_source(name, segments, num_vars, nest_depth)
    namespace: dict[str, object] = {}
    exec(compile(source, f"<stress:{name}>", "exec"), namespace)
    return cast(Callable[..., int], namespace[name])


@passIf(FREE_THREADING_BUILD, "Test too slow to run with free-threading")
class JitPipelineStressTest(unittest.TestCase):
    def _compile_and_time(self, func: Callable[..., int]) -> int:
        """Force-compile ``func`` and return its compile time in milliseconds."""
        before = cinderx.jit.get_compilation_time()
        cinderx.jit.force_compile(func)
        after = cinderx.jit.get_compilation_time()
        self.assertTrue(
            cinderx.jit.is_jit_compiled(func),
            f"{func.__name__} was not JIT-compiled",
        )
        # The per-function timer is whole-millisecond, so fall back to the total
        # delta (which brackets exactly this compile) when it rounds to zero.
        per_func = cinderx.jit.get_function_compilation_time(func) or 0
        return max(per_func, after - before)

    def test_pipeline_stress(self) -> None:
        # First, a small copy for a correctness check (cheap to compile/run).
        checker = _make_function("stress_check", 8, _NUM_VARS, _NEST_DEPTH)
        args = [0] * _NUM_VARS
        seq = [1, 2.0, "s", b"b", [1], (1,), {1: 1}, {1}] * 4
        self.assertIsInstance(checker(seq, *args), int)

        # Then compile fresh copies of a large control-flow-heavy function
        # back-to-back until we've burned ~_TARGET_SECONDS, so the run spends
        # essentially all its time inside the JIT compiler.  min/median are
        # noise-robust metrics for A/B'ing compile time across builds.
        times: list[int] = []
        num_instrs = 0
        deadline = time.perf_counter() + _TARGET_SECONDS
        reps = 0
        while time.perf_counter() < deadline:
            func = _make_function(f"stress_{reps}", _SEGMENTS, _NUM_VARS, _NEST_DEPTH)
            try:
                elapsed_ms = self._compile_and_time(func)
            except RuntimeError:
                # The JIT refuses functions above an internal size ceiling; if
                # _SEGMENTS is set too high, stop rather than spin on failures.
                break
            times.append(elapsed_ms)
            counts = cinderx.jit.get_function_hir_opcode_counts(func) or {}
            num_instrs = sum(counts.values())
            reps += 1

        self.assertGreater(reps, 0, "no stress function compiled; lower _SEGMENTS")
        times.sort()

        total_time_s = sum(times) / 1000
        compiles_per_s = reps / total_time_s if total_time_s != 0.0 else 0.0

        print(
            f"\n[jit-stress] segments={_SEGMENTS} vars={_NUM_VARS} "
            f"depth={_NEST_DEPTH} -> {num_instrs} HIR instrs"
        )
        print(
            f"[jit-stress] Compiled {reps} copies in {total_time_s:.2f}s ({compiles_per_s:.2f} compile/sec): per-compile min={times[0]}ms median={times[len(times) // 2]}ms max={times[-1]}ms\n"
        )

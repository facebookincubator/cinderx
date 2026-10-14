# Copyright (c) Meta Platforms, Inc. and affiliates.

import gc
import os
import re
import sys
import tempfile
import unittest

import cinderx
from cinderx.test_support import passUnless, run_in_fork

_PY312: bool = sys.version_info[:2] == (3, 12)
# getallocatedblocks() counts pymalloc blocks, so it is 0 without pymalloc.
_PYMALLOC: bool = sys.getallocatedblocks() > 0
# Debug allocator hooks change block sizes, so fill does nothing under them.
_PYTHONMALLOC: str = os.environ.get("PYTHONMALLOC", "")
_DEBUG_HOOKS: bool = (
    _PYTHONMALLOC.endswith("debug")
    or sys.flags.dev_mode
    or (hasattr(sys, "gettotalrefcount") and _PYTHONMALLOC in ("", "default"))
)

# One row per size class: index, size, pools, blocks in use, available blocks.
_CLASS_ROW: re.Pattern[str] = re.compile(r"^\s*\d+\s+\d+\s+\d+\s+\d+\s+(\d+)\s*$")
_UNUSED_POOLS: re.Pattern[str] = re.compile(r"([\d,]+) unused pools \* (\d+) bytes")
_ARENAS: re.Pattern[str] = re.compile(r"([\d,]+) arenas \* (\d+) bytes/arena")


def _fragment_heap() -> list[object]:
    objs: list[object] = [
        [str(i) * 3, (i, str(i)), {"k": i}, float(i)] for i in range(30000)
    ]
    del objs[::3]
    gc.collect()
    return objs


def _arenas() -> int:
    with tempfile.TemporaryFile() as tmp:
        sys.stderr.flush()
        saved = os.dup(2)
        os.dup2(tmp.fileno(), 2)
        try:
            sys._debugmallocstats()
        finally:
            os.dup2(saved, 2)
            os.close(saved)
        tmp.seek(0)
        stats = tmp.read().decode()
    arenas = _ARENAS.search(stats)
    assert arenas is not None, stats
    return int(arenas[1].replace(",", ""))


@passUnless(
    cinderx.is_initialized() and _PY312 and _PYMALLOC and not _DEBUG_HOOKS,
    "Needs CinderX on 3.12 with plain pymalloc",
)
class PymallocFillTests(unittest.TestCase):
    @run_in_fork
    def test_fill_leaves_no_free_space(self) -> None:
        objs = _fragment_heap()
        gc.disable()
        with tempfile.TemporaryFile() as tmp:
            sys.stderr.flush()
            saved = os.dup(2)
            os.dup2(tmp.fileno(), 2)
            try:
                first = cinderx.fill_pymalloc_pools()
                # Back to back: _debugmallocstats() can free a block.
                second = cinderx.fill_pymalloc_pools()
                sys._debugmallocstats()
            finally:
                os.dup2(saved, 2)
                os.close(saved)
            tmp.seek(0)
            stats = tmp.read().decode()
        gc.enable()
        del objs

        self.assertGreater(first[0], 0)
        self.assertEqual(second, (0, 0))
        avail = [int(m[1]) for m in map(_CLASS_ROW.match, stats.splitlines()) if m]
        self.assertGreater(len(avail), 0, stats)
        self.assertEqual(sum(avail), 0, stats)
        # Only the newest arena's never-used pools are left.
        unused = _UNUSED_POOLS.search(stats)
        arenas = _ARENAS.search(stats)
        self.assertIsNotNone(unused, stats)
        self.assertIsNotNone(arenas, stats)
        pools_per_arena = int(arenas[2]) // int(unused[2])
        self.assertLess(int(unused[1].replace(",", "")), pools_per_arena, stats)

    @run_in_fork
    def test_repeated_fill_does_not_take_new_arenas(self) -> None:
        objs = _fragment_heap()
        cinderx.fill_pymalloc_pools()
        before = _arenas()
        calls = 50
        for _ in range(calls):
            tmp = [str(i) * 2 for i in range(200)]
            del tmp
            cinderx.fill_pymalloc_pools()
        grown = _arenas() - before
        del objs
        # Each call pins what was freed since the previous one (about a pool
        # here), not the rest of an arena.
        self.assertLess(grown, calls // 5)


@passUnless(
    cinderx.is_initialized() and not (_PY312 and _PYMALLOC),
    "Needs a build without pymalloc fill support",
)
class PymallocUnsupportedTests(unittest.TestCase):
    @run_in_fork
    def test_noop(self) -> None:
        self.assertEqual(cinderx.fill_pymalloc_pools(), (0, 0))
        objs = [[i] for i in range(1000)]
        del objs[::2]
        self.assertEqual(objs[0], [1])

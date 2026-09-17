# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Shared discovery helper for the generated CPython test shims."""

from __future__ import annotations

import atexit
import fnmatch
import functools
import os
import shutil
import tempfile
import unittest
from pathlib import Path

from cinderx.TestScripts.skip_list_support import (
    get_required_skip_list_files,
    get_skip_list_files,
    is_test_module_skipped,
    iter_tests,
    parse_skip_lists,
)
from cpython_tests.skipped_tests import SKIPPED_TESTS

_temp_cwd: str | None = None


def _use_temp_cwd() -> None:
    """Run from a scratch directory, as regrtest does.

    A good number of CPython tests write into the working directory or assert on
    paths relative to it, and fail if it is the repo root. `cinder_test_runner.py`
    wraps each worker in `os_helper.temp_cwd()` for the same reason; the TPX
    adapter has no equivalent, so do it here on behalf of every shim.
    """
    global _temp_cwd
    if _temp_cwd is not None:
        return
    original = os.getcwd()
    _temp_cwd = tempfile.mkdtemp(prefix=f"cpython-tests-{os.getpid()}-")
    # atexit runs LIFO, so this pair leaves the directory before removing it.
    # Finalising the interpreter from inside a deleted directory can fail the
    # process after every test has already passed.
    atexit.register(shutil.rmtree, _temp_cwd, ignore_errors=True)
    atexit.register(os.chdir, original)
    os.chdir(_temp_cwd)


_use_temp_cwd()


@functools.lru_cache(maxsize=1)
def _skip_list_rules() -> tuple[frozenset[str], tuple[str, ...]]:
    skip_list_files = get_skip_list_files(
        include_jit=False,
        include_platform=False,
    )
    modules, patterns = parse_skip_lists(
        Path(__file__).parent / "skip_lists",
        skip_list_files,
        required=get_required_skip_list_files(skip_list_files),
    )
    return frozenset(modules), tuple(patterns)


def _is_skip_listed(test_id: str) -> bool:
    modules, patterns = _skip_list_rules()
    if is_test_module_skipped(test_id, modules):
        return True
    parts = test_id.split(".")
    return any(
        fnmatch.fnmatchcase(test_id, pattern)
        or any(fnmatch.fnmatchcase(part, pattern) for part in parts)
        for pattern in patterns
    )


def _is_skipped(test_id: str) -> bool:
    """Match an exact test id, or any prefix of it at a dotted boundary.

    Prefixes let a whole class or module be excluded with one entry, which
    matters for suites whose members fail interchangeably from run to run --
    listing them individually never converges.
    """
    if test_id in SKIPPED_TESTS:
        return True
    parts = test_id.split(".")
    return any(".".join(parts[:i]) in SKIPPED_TESTS for i in range(1, len(parts)))


def load_module_tests(loader: unittest.TestLoader, module) -> unittest.TestSuite:
    """Load `module`'s tests, dropping anything matched by SKIPPED_TESTS.

    Filtering happens at discovery rather than via `skipTest` so the skipped
    tests never reach TPX at all; a test TPX has listed but cannot re-run by
    dotted name takes the whole batch down with it.
    """
    suite = unittest.TestSuite()
    for test in iter_tests(loader.loadTestsFromModule(module)):
        test_id = test.id()
        if not _is_skipped(test_id) and not _is_skip_listed(test_id):
            suite.addTest(test)
    return suite

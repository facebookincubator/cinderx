# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Shared discovery helper for the generated CPython test shims."""

from __future__ import annotations

import atexit
import contextlib
import fnmatch
import functools
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
from test.support import os_helper

_temp_cwd: contextlib.ExitStack | None = None


def use_temp_cwd() -> None:
    """Run from a scratch directory, as regrtest does."""
    global _temp_cwd
    if _temp_cwd is not None:
        return
    _temp_cwd = contextlib.ExitStack()
    cwd = _temp_cwd.enter_context(os_helper.temp_cwd(name=None))
    env = _temp_cwd.enter_context(os_helper.EnvironmentVarGuard())
    for name in ("TMPDIR", "TEMP", "TMP"):
        env[name] = cwd
    original_tempdir = tempfile.tempdir
    _temp_cwd.callback(setattr, tempfile, "tempdir", original_tempdir)
    tempfile.tempdir = cwd
    atexit.register(_temp_cwd.close)


@functools.lru_cache(maxsize=1)
def _skip_list_rules() -> tuple[frozenset[str], tuple[str, ...]]:
    skip_list_files = get_skip_list_files(
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

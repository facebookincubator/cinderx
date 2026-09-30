# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Shared discovery helper for the generated CPython test shims."""

from __future__ import annotations

import atexit
import contextlib
import functools
import importlib
import tempfile
import unittest
from pathlib import Path

from cinderx.TestScripts.skip_list_support import (
    expand_test_patterns,
    get_required_skip_list_files,
    get_skip_list_files,
    is_test_module_skipped,
    iter_tests,
    matches_test_patterns,
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
def _exclusions() -> tuple[frozenset[str], tuple[str, ...]]:
    """Excluded top-level modules, and test-id patterns, from both sources.

    The runner's shared skip lists and Tier 1's own SKIPPED_TESTS are applied
    identically once expanded, so they share one cache and one matching pass.
    Expanding SKIPPED_TESTS lets a class or module entry cover its members,
    which matters for suites whose failing member varies between runs.
    """
    skip_list_files = get_skip_list_files(include_platform=False)
    modules, patterns = parse_skip_lists(
        Path(__file__).parent / "skip_lists",
        skip_list_files,
        required=get_required_skip_list_files(skip_list_files),
    )
    return frozenset(modules), tuple(patterns) + tuple(
        expand_test_patterns(SKIPPED_TESTS)
    )


def _is_excluded(test_id: str) -> bool:
    modules, patterns = _exclusions()
    return is_test_module_skipped(test_id, modules) or matches_test_patterns(
        test_id, patterns
    )


def load_module_tests(
    loader: unittest.TestLoader, module_name: str
) -> unittest.TestSuite:
    """Load `module_name`'s tests, dropping anything the runner would skip.

    Filtering happens at discovery rather than via `skipTest` so the skipped
    tests never reach TPX at all; a test TPX has listed but cannot re-run by
    dotted name takes the whole batch down with it.

    The import is here rather than at shim import time because whether a module
    is available depends on the build: CPython raises SkipTest at import for
    `test_argparse` and a dozen others under ASAN. The partition is classified
    once, in opt, so it has to hold for every mode. Only SkipTest is caught -- a
    module that has genuinely gone away on a Python upgrade should still take
    the target down rather than quietly dropping out.
    """
    suite = unittest.TestSuite()
    try:
        module = importlib.import_module(module_name)
    except unittest.SkipTest:
        return suite
    for test in iter_tests(loader.loadTestsFromModule(module)):
        test_id = test.id()
        if not _is_excluded(test_id):
            suite.addTest(test)
    return suite

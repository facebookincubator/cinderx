# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Shared skip-list handling for CinderX's CPython test runners."""

from __future__ import annotations

import importlib
import platform
import sys
import sysconfig
import unittest
from collections.abc import Iterable, Iterator, Set
from pathlib import Path


def iter_tests(suite: unittest.TestSuite) -> Iterator[unittest.TestCase]:
    """Yield the individual test cases nested anywhere inside `suite`."""
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            yield from iter_tests(test)
        else:
            yield test


_BASE_SKIP_LIST_FILES = ("devserver_skip_tests.txt", "cinder_skip_test.txt")
_REQUIRED_WHEN_SELECTED = frozenset(
    {"asan_skip_tests.txt", "cinder_jit_ignore_tests.txt"}
)


def is_prefork_build() -> bool:
    try:
        cinderx = importlib.import_module("cinderx")
    except ImportError:
        return False
    return bool(vars(cinderx)["is_prefork_build"]())


def _is_asan_build() -> bool:
    # regrtest's own notion, deliberately not cinderx.test_support's
    # is_sanitizer_build(): these lists were written against what regrtest
    # skips, and the two can disagree. Both are in scope in
    # cinder_test_runner.py, so unifying them would silently change which tests
    # run.
    support = importlib.import_module("test.support")
    return bool(vars(support)["check_sanitizer"](address=True))


def _jit_skip_lists(version: str) -> list[str]:
    try:
        importlib.import_module("cinderjit")
    except ImportError:
        return []

    skip_list_files = [
        "cinder_jit_ignore_tests.txt",
        f"cinder_jit_ignore_tests_{version}.txt",
    ]
    if sysconfig.get_config_var("Py_GIL_DISABLED"):
        skip_list_files.append(f"cinder_jit_ignore_tests_{version}t.txt")
    return skip_list_files


def _platform_skip_lists() -> list[str]:
    skip_list_files = []
    if platform.processor() not in ("", platform.machine()):
        skip_list_files.append("cross_platform_skip_tests.txt")
    if platform.machine() in ("aarch64", "arm64"):
        skip_list_files.append("arm64_skip_tests.txt")
    return skip_list_files


def get_skip_list_files(
    huntrleaks: bool = False,
    use_rr: bool = False,
    extra_skip_files: Iterable[str] | None = None,
    *,
    include_jit: bool = True,
    include_platform: bool = True,
) -> list[str]:
    """Return the skip lists active for the current interpreter and options."""
    skip_list_files = list(_BASE_SKIP_LIST_FILES)

    version = "".join(str(v) for v in sys.version_info[:2])
    skip_list_files.append(f"cinder_skip_test_{version}.txt")

    if _is_asan_build():
        skip_list_files.append("asan_skip_tests.txt")

    if use_rr:
        skip_list_files.append("rr_skip_tests.txt")

    if extra_skip_files:
        skip_list_files.extend(extra_skip_files)

    if include_jit:
        skip_list_files.extend(_jit_skip_lists(version))

    if huntrleaks:
        skip_list_files.append("refleak_skip_tests.txt")
        if is_prefork_build():
            skip_list_files.append("refleak_prefork_skip_tests.txt")

    if include_platform:
        skip_list_files.extend(_platform_skip_lists())

    return skip_list_files


def get_required_skip_list_files(skip_list_files: Iterable[str]) -> set[str]:
    """Return files that a packaged Tier 1 runner must provide."""
    selected = set(skip_list_files)
    return set(_BASE_SKIP_LIST_FILES) | (selected & _REQUIRED_WHEN_SELECTED)


def is_test_module_skipped(test_id: str, skip_modules: Set[str]) -> bool:
    """Whether a CPython test id belongs to an excluded top-level module."""
    parts = test_id.split(".")
    return len(parts) >= 2 and parts[0] == "test" and parts[1] in skip_modules


def parse_skip_lists(
    directory: Path,
    skip_list_files: Iterable[str],
    *,
    required: Iterable[str] = (),
) -> tuple[set[str], set[str]]:
    """Parse skip-list files into module exclusions and test-id patterns."""
    required_files = set(required)
    missing = [name for name in required_files if not (directory / name).exists()]
    if missing:
        raise RuntimeError(f"skip lists missing from resources: {sorted(missing)}")

    skip_modules: set[str] = set()
    skip_patterns: set[str] = set()
    for skip_file in skip_list_files:
        skip_file_path = directory / skip_file
        if not skip_file_path.exists():
            continue
        for raw in skip_file_path.read_text().splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if "." in line or "*" in line:
                skip_patterns.add(line)
            else:
                skip_modules.add(line)

    return skip_modules, skip_patterns

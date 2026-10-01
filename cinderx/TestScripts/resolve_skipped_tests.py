#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Resolve CPythonTests/skipped_tests.py into regrtest module and match args.

Driven by run_skipped_tier1_tests.sh, which has to run the tests Tier 1 filters
out through cinder_test_runner.py. regrtest needs to be told which module files
to open as well as which ids to match, so each entry is resolved to the module
that owns it.

Emits one directive per line: `M <module>` for `-t`, `P <pattern>` for `-m`.
"""

from __future__ import annotations

import argparse
import runpy
import sys
from pathlib import Path

from cinderx.TestScripts.skip_list_support import expand_test_patterns


def read_skipped_tests(path: Path) -> list[str]:
    """Load the Tier 1 exclusions from their canonical Python module."""
    tests = runpy.run_path(str(path)).get("SKIPPED_TESTS")
    if not isinstance(tests, (frozenset, set, tuple, list)) or not tests:
        raise ValueError(f"SKIPPED_TESTS is missing or empty in {path}")
    if not all(isinstance(test, str) for test in tests):
        raise ValueError(f"SKIPPED_TESTS contains a non-string entry in {path}")
    return sorted(tests)


def _owning_module(test: str, tier1_modules: set[str]) -> str | None:
    """Longest Tier 1 module prefix of `test`.

    Longest-first is what makes the split test directories come out right:
    `test.test_asyncio.test_events.Foo.test_bar` is owned by the
    `test.test_asyncio.test_events` module rather than the `test.test_asyncio`
    package. The generated partition is authoritative, so this does not import
    test modules while constructing the plan; some imports raise `SkipTest`
    under ASAN before the runner can apply its sanitizer skip list.
    """
    parts = test.split(".")
    for i in range(len(parts), 1, -1):
        name = ".".join(parts[:i])
        if name in tier1_modules:
            return name
    return None


def resolve(tests: list[str], tier1_modules: set[str]) -> tuple[list[str], list[str]]:
    """Split each entry into its owning module and the ids to match.

    The list is shared across versions, so an entry naming something this
    interpreter does not have is dropped rather than being an error. Entries
    owned by Tier 2 are also dropped because that tier already runs them through
    regrtest.
    """
    modules: set[str] = set()
    selected: list[str] = []
    dropped: list[str] = []
    for test in tests:
        name = _owning_module(test, tier1_modules)
        if name is None:
            dropped.append(test)
            continue
        modules.add(name)
        selected.append(test)
    if dropped:
        print(
            f"note: {len(dropped)} entr{'y' if len(dropped) == 1 else 'ies'} did "
            f"not resolve to a Tier 1 module here: {', '.join(dropped[:5])}",
            file=sys.stderr,
        )
    return sorted(modules), selected


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--tier1-module",
        action="append",
        dest="tier1_modules",
        required=True,
        help="Module owned by Tier 1; may be supplied multiple times",
    )
    parser.add_argument("skipped_tests_py", type=Path)
    args = parser.parse_args()

    path = args.skipped_tests_py
    tests = read_skipped_tests(path)
    modules, selected = resolve(tests, set(args.tier1_modules))
    if not modules:
        raise SystemExit(
            f"none of the {len(tests)} entries in {path} resolved to a Tier 1 "
            "module; the list is dominated by stdlib test modules, so this is "
            "a bug rather than a version difference"
        )

    for module in modules:
        print("M", module)
    for pattern in expand_test_patterns(selected):
        print("P", pattern)


if __name__ == "__main__":
    main()
    sys.exit(0)

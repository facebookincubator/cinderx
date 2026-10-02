# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Guard against Tier 1 and skip_list_support.py disagreeing on skip lists.

Tier 1 runs as a `python_unittest`, so it never calls `get_skip_list_files()` and
has to re-apply the same lists itself -- which means the set of lists is written
down twice, once in the runner and once in `skip_lists.bzl`. Adding a list to
the runner and forgetting the other half does not fail anything: Tier 1 just
stops honouring it, and the result is a wall of failures that look unrelated to
the skip list nobody shipped.

`skip_list_support.py` publishes the patterns for every built-in list it can
select. Every matching file must be either shipped to Tier 1 or named as a
deliberate exclusion, with a reason.
"""

from __future__ import annotations

import ast
import fnmatch
import unittest
from pathlib import Path

from cinderx.TestScripts.skip_list_support import SKIP_LIST_FILE_PATTERNS

_SOURCES = Path(__file__).parent / "skip_list_sources"


def _assignment(tree: ast.Module, name: str) -> ast.expr:
    for node in ast.walk(tree):
        if isinstance(node, ast.AnnAssign):
            target = node.target
        elif isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
        else:
            continue
        if isinstance(target, ast.Name) and target.id == name:
            return node.value
    raise AssertionError(f"no {name} assignment found")


def _bzl_value(name: str) -> object:
    # skip_lists.bzl is Starlark, but a list/dict of literals parses as Python.
    tree = ast.parse((_SOURCES / "skip_lists.bzl").read_text())
    return ast.literal_eval(_assignment(tree, name))


def _on_disk() -> set[str]:
    return {path.name for path in _SOURCES.glob("*.txt")}


def _referenced() -> set[str]:
    """Runner patterns resolved against the skip lists that actually exist."""
    on_disk = _on_disk()
    referenced = set()
    for pattern in SKIP_LIST_FILE_PATTERNS:
        referenced |= {name for name in on_disk if fnmatch.fnmatchcase(name, pattern)}
    return referenced


class SkipListCoverageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.shipped = set(_bzl_value("CPYTHON_TIER1_SKIP_LISTS"))
        self.excluded = dict(_bzl_value("CPYTHON_TIER1_SKIP_LISTS_EXCLUDED"))

    def test_every_runner_skip_list_is_shipped_or_excluded(self) -> None:
        unaccounted = sorted(_referenced() - self.shipped - set(self.excluded))
        self.assertEqual(
            unaccounted,
            [],
            "skip_list_support.py can load these skip lists but "
            "TestScripts/skip_lists.bzl says nothing about them. Add each to "
            "CPYTHON_TIER1_SKIP_LISTS so Tier 1 applies it, or to "
            "CPYTHON_TIER1_SKIP_LISTS_EXCLUDED with the reason it cannot.",
        )

    def test_no_list_is_both_shipped_and_excluded(self) -> None:
        self.assertEqual(sorted(self.shipped & set(self.excluded)), [])

    def test_every_named_list_exists_on_disk(self) -> None:
        on_disk = _on_disk()
        named = self.shipped | set(self.excluded)
        self.assertEqual(
            sorted(named - on_disk),
            [],
            "skip_lists.bzl names files that are not in cinderx/TestScripts; "
            "a shipped name that does not exist makes the Tier 1 target fail "
            "to build, and a stale exclusion hides nothing.",
        )

    def test_shipped_lists_are_reachable_by_the_runner(self) -> None:
        # The other direction: shipping something the runner never loads means
        # Tier 1 skips tests the dispatcher runs.
        self.assertEqual(sorted(self.shipped - _referenced()), [])

    def test_every_exclusion_has_a_reason(self) -> None:
        self.assertEqual(
            sorted(
                name for name, reason in self.excluded.items() if not reason.strip()
            ),
            [],
        )

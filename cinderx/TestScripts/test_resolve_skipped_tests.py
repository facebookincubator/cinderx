# Copyright (c) Meta Platforms, Inc. and affiliates.

import tempfile
import unittest
from pathlib import Path

from cinderx.TestScripts.resolve_skipped_tests import read_skipped_tests, resolve


class SkippedTestsParsingTest(unittest.TestCase):
    def _read(self, source: str) -> list[str]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "skipped_tests.py"
            path.write_text(source)
            return read_skipped_tests(path)

    def test_loads_canonical_module(self) -> None:
        self.assertEqual(
            self._read(
                'SKIPPED_TESTS: frozenset[str] = frozenset({"test.test_b", "test.test_a"})'
            ),
            ["test.test_a", "test.test_b"],
        )

    def test_rejects_missing_or_empty_list(self) -> None:
        for source in ("OTHER = {'test.test_a'}", "SKIPPED_TESTS = frozenset()"):
            with self.subTest(source=source):
                with self.assertRaisesRegex(ValueError, "missing or empty"):
                    self._read(source)

    def test_rejects_non_string_entries(self) -> None:
        with self.assertRaisesRegex(ValueError, "non-string"):
            self._read("SKIPPED_TESTS = {'test.test_a', 1}")


class ResolveSkippedTestsTest(unittest.TestCase):
    def test_resolves_owning_module(self) -> None:
        modules, selected = resolve(
            ["test.test_bool.BoolTest.test_true"], {"test.test_bool"}
        )
        self.assertEqual(modules, ["test.test_bool"])
        self.assertEqual(selected, ["test.test_bool.BoolTest.test_true"])

    def test_resolves_module_inside_a_split_package(self) -> None:
        test = "test.test_asyncio.test_events.Foo.test_bar"
        modules, selected = resolve([test], {"test.test_asyncio.test_events"})
        # Longest prefix wins: the leaf module, not the test.test_asyncio package.
        self.assertEqual(modules, ["test.test_asyncio.test_events"])
        self.assertEqual(selected, [test])

    def test_resolves_a_package_shaped_module(self) -> None:
        # A directory rather than a file -- test_pathlib is one in 3.14 -- still
        # owns its entries. (It is Tier 2 in practice, so the ownership filter
        # drops it; this covers the resolution step in isolation.)
        test = "test.test_pathlib.PathSubclassTest.test_touch_common"
        modules, selected = resolve([test], {"test.test_pathlib"})
        self.assertEqual(modules, ["test.test_pathlib"])
        self.assertEqual(selected, [test])

    def test_drops_entries_this_version_does_not_have(self) -> None:
        modules, selected = resolve(
            ["test.test_does_not_exist_at_all.Foo.test_x"], {"test.test_bool"}
        )
        self.assertEqual(modules, [])
        self.assertEqual(selected, [])

    def test_does_not_import_a_partitioned_module(self) -> None:
        test = "test.test_does_not_exist_at_all.Foo.test_x"
        modules, selected = resolve([test], {"test.test_does_not_exist_at_all"})
        self.assertEqual(modules, ["test.test_does_not_exist_at_all"])
        self.assertEqual(selected, [test])

    def test_drops_test_owned_by_tier2(self) -> None:
        modules, selected = resolve(
            ["test.test_bool.BoolTest.test_true"], {"test.test_iter"}
        )
        self.assertEqual(modules, [])
        self.assertEqual(selected, [])

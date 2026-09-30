# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from cinderx.TestScripts import skip_list_support
from cpython_tests import _support as cpython_test_support


class SkipListFilesTests(unittest.TestCase):
    @mock.patch.object(skip_list_support, "_is_asan_build", return_value=True)
    @mock.patch.object(skip_list_support, "sys")
    def test_tier1_selection(self, sys, is_asan_build) -> None:
        sys.version_info = (3, 14)

        self.assertEqual(
            skip_list_support.get_skip_list_files(
                include_jit=False,
                include_platform=False,
            ),
            [
                "devserver_skip_tests.txt",
                "cinder_skip_test.txt",
                "cinder_skip_test_314.txt",
                "asan_skip_tests.txt",
            ],
        )
        is_asan_build.assert_called_once_with()

    def test_required_files_exclude_optional_variants(self) -> None:
        self.assertEqual(
            skip_list_support.get_required_skip_list_files(
                [
                    "devserver_skip_tests.txt",
                    "cinder_skip_test.txt",
                    "cinder_skip_test_314.txt",
                    "asan_skip_tests.txt",
                    "cinder_jit_ignore_tests.txt",
                    "cinder_jit_ignore_tests_314.txt",
                ]
            ),
            {
                "devserver_skip_tests.txt",
                "cinder_skip_test.txt",
                "asan_skip_tests.txt",
                "cinder_jit_ignore_tests.txt",
            },
        )


class SkipModuleTests(unittest.TestCase):
    def test_matches_top_level_and_split_package_modules(self) -> None:
        self.assertTrue(
            skip_list_support.is_test_module_skipped(
                "test.test_signal", {"test_signal"}
            )
        )
        self.assertTrue(
            skip_list_support.is_test_module_skipped(
                "test.test_asyncio.test_events.EventTests.test_call",
                {"test_asyncio"},
            )
        )

    def test_rejects_prefixes_and_non_cpython_namespaces(self) -> None:
        self.assertFalse(
            skip_list_support.is_test_module_skipped("test.test_osx_env", {"test_os"})
        )
        self.assertFalse(
            skip_list_support.is_test_module_skipped(
                "test_cinderx.test_compiler.test_static.test_compile",
                {"test_compile"},
            )
        )


class SkipPatternTests(unittest.TestCase):
    def test_matches_whole_ids_and_dotted_parts(self) -> None:
        test_id = "test.test_bool.BoolTest.test_true"
        self.assertTrue(skip_list_support.matches_test_patterns(test_id, [test_id]))
        self.assertTrue(skip_list_support.matches_test_patterns(test_id, ["test_true"]))
        self.assertFalse(
            skip_list_support.matches_test_patterns(test_id, ["test_false"])
        )

    def test_expands_class_and_module_prefixes(self) -> None:
        patterns = skip_list_support.expand_test_patterns(["test.test_bool.BoolTest"])
        self.assertTrue(
            skip_list_support.matches_test_patterns(
                "test.test_bool.BoolTest.test_true", patterns
            )
        )


class LoadModuleTests(unittest.TestCase):
    @mock.patch.object(
        cpython_test_support.importlib,
        "import_module",
        side_effect=unittest.SkipTest("unavailable in this build mode"),
    )
    def test_skip_at_import_returns_an_empty_suite(self, import_module) -> None:
        suite = cpython_test_support.load_module_tests(
            unittest.TestLoader(), "test.test_mode_gated"
        )

        self.assertEqual(suite.countTestCases(), 0)
        import_module.assert_called_once_with("test.test_mode_gated")


class ParseSkipListsTests(unittest.TestCase):
    def test_parse_modules_patterns_and_comments(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "skip.txt").write_text(
                "# comment\n\ntest_module\ntest.mod.Case.test_method\ntest_*\n"
            )

            modules, patterns = skip_list_support.parse_skip_lists(
                directory,
                ["skip.txt", "optional.txt"],
                required=["skip.txt"],
            )

        self.assertEqual(modules, {"test_module"})
        self.assertEqual(patterns, {"test.mod.Case.test_method", "test_*"})

    def test_missing_required_file_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError, "required.txt"):
                skip_list_support.parse_skip_lists(
                    Path(tmp),
                    ["required.txt"],
                    required=["required.txt"],
                )

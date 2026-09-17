# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from cinderx.TestScripts import skip_list_support


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

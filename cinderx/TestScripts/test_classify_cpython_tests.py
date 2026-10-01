# Copyright (c) Meta Platforms, Inc. and affiliates.

import sys
import types
import unittest
from unittest.mock import patch

from cinderx.TestScripts import common, skip_list_support

sys.modules.setdefault("skip_list_support", skip_list_support)
sys.modules.setdefault("common", common)

from cinderx.TestScripts import classify_cpython_tests  # noqa: E402


class ClassifierVersionTest(unittest.TestCase):
    def _validate(
        self,
        version: str,
        *,
        interpreter: tuple[int, int] = (3, 14),
        free_threaded: bool = False,
        prefork: bool = False,
    ) -> None:
        with (
            patch.object(
                classify_cpython_tests.sys,
                "version_info",
                types.SimpleNamespace(major=interpreter[0], minor=interpreter[1]),
            ),
            patch.object(
                classify_cpython_tests.sysconfig,
                "get_config_var",
                return_value=free_threaded,
            ),
            patch.object(
                classify_cpython_tests, "is_prefork_build", return_value=prefork
            ),
        ):
            classify_cpython_tests.validate_version_label(version)

    def test_accepts_matching_labels(self) -> None:
        self._validate("3.14")
        self._validate("3.14t", free_threaded=True)
        self._validate("3.12-prefork-model", interpreter=(3, 12), prefork=True)

    def test_rejects_mismatched_labels(self) -> None:
        for version, free_threaded, prefork in (
            ("3.12", False, False),
            ("3.14t", False, False),
            ("3.14", True, False),
            ("3.14-prefork-model", False, False),
        ):
            with self.subTest(version=version):
                with self.assertRaisesRegex(SystemExit, "does not describe"):
                    self._validate(
                        version,
                        free_threaded=free_threaded,
                        prefork=prefork,
                    )

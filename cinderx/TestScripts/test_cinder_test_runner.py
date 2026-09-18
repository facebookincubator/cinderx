# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

import io
import sys
import unittest
from unittest.mock import patch

from cinderx.TestScripts import common, skip_list_support

sys.modules.setdefault("skip_list_support", skip_list_support)
sys.modules.setdefault("common", common)

from cinderx.TestScripts import cinder_test_runner  # noqa: E402


class RunnerSkipConfigurationTests(unittest.TestCase):
    def test_constructor_applies_modules_and_preserves_patterns(self) -> None:
        pattern = "test.test_bool.BoolTest.test_true"
        with patch.object(
            cinder_test_runner,
            "_computeSkipTests",
            return_value=({"test_signal"}, {pattern}),
        ):
            runner = cinder_test_runner.MultiWorkerCinderRegrtest(
                logfile=io.StringIO(),
                tests=["test.test_signal", "test.test_bool"],
                worker_timeout=1,
                worker_respawn_interval=1,
                success_on_test_errors=False,
                use_rr=False,
                recording_metadata_path="",
                no_retry_on_test_errors=False,
                huntrleaks=False,
                num_workers=1,
                json_summary_file=None,
                failfast=False,
                expected_failures=[],
            )

        self.assertEqual(("test.test_bool",), runner._runtests_config.tests)
        self.assertEqual([(pattern, False)], runner._runtests_config.match_tests)

    def test_constructor_warns_for_fully_skipped_explicit_selection(self) -> None:
        with (
            patch.object(
                cinder_test_runner,
                "_computeSkipTests",
                return_value=({"test_signal"}, set()),
            ),
            patch.object(cinder_test_runner, "log_err") as log_err,
        ):
            runner = cinder_test_runner.MultiWorkerCinderRegrtest(
                logfile=io.StringIO(),
                tests=["test.test_signal"],
                worker_timeout=1,
                worker_respawn_interval=1,
                success_on_test_errors=False,
                use_rr=False,
                recording_metadata_path="",
                no_retry_on_test_errors=False,
                huntrleaks=False,
                num_workers=1,
                json_summary_file=None,
                failfast=False,
                expected_failures=[],
            )

        self.assertEqual((), runner._runtests_config.tests)
        log_err.assert_called_once_with(
            "WARNING: all explicitly selected tests are skip-listed\n"
        )

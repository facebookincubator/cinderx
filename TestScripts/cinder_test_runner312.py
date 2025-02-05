# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# A modified version of the regrtest system which runs multiple tests per
# instance of a parallel worker. This compensates for JIT compilation,
# particularly for debug builds, is a huge overhead. So, we amortize initial
# startup costs.
#
# We also have a few Cinder/Meta-specific tweaks. E.g. some logic for selecting
# tests to skip under certain test conditions like JIT, or hooking into our
# internal logging systems, etc.

import argparse
import dataclasses
import gc
import json
import multiprocessing
import os
import os.path
import pathlib
import queue
import resource
import shlex
import signal
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest

from pathlib import Path

from typing import Dict, IO, Iterable, List, Optional, Set, Tuple

import test.libregrtest.findtests as libregrtest_findtests

import test.libregrtest.logger as libregrtest_logger
import test.libregrtest.result as libregrtest_result

import test.libregrtest.results as libregrtest_results

import test.libregrtest.runtests as libregrtest_runtests

import test.libregrtest.setup as libregrtest_setup

import test.libregrtest.single as libregrtest_single
import test.libregrtest.utils as libregrtest_utils

from cinderx.test_support import get_cinderjit_xargs, is_asan_build

from common import (
    ActiveTest,
    ASANLogManipulator,
    CINDER_RUNNER_LOG_DIR,
    get_cinderx_dir,
    get_cinderx_static_tests,
    log_err,
    MAX_WORKERS,
    MessagePipe,
    print_running_tests,
    RR_RECORD_BASE_CMD,
    RunTest,
    ShutdownWorker,
    TestComplete,
    TestLog,
    TESTS_TO_SERIALIZE,
    TestStarted,
    WORKER_RESPAWN_INTERVAL,
    WorkerDone,
    WorkSender,
)

from test import support
from test.libregrtest.cmdline import _parse_args as libregrtest_parse_args

from test.libregrtest.main import main as libregrtest_main
from test.support import os_helper

WORKER_PATH = os.path.abspath(__file__)

# Directories in test_cinderx to recurse into to find test modules, rather than
# treating the whole directory as a single test module.
CINDERX_SPLIT_TEST_DIRS = {
    "test_cinderx.test_cpython_overrides",
    "test_cinderx.test_compiler",
    "test_cinderx.test_compiler.test_static",
}


class ReplayInfo:
    def __init__(
        self,
        pid: int,
        test_log: str,
        rr_trace_dir: Optional[str],
    ) -> None:
        self.pid = pid
        self.test_log = test_log
        self.rr_trace_dir = rr_trace_dir
        self.failed: List[str] = []
        self.crashed: Optional[str] = None

    def _build_replay_cmd(self) -> str:
        args = [
            sys.executable,
            *get_cinderjit_xargs(),
            sys.argv[0],
            "dispatcher",
            "--replay",
            self.test_log,
        ]
        skip_next_arg = False
        trailing_args = []
        for arg in sys.argv[1:]:
            if skip_next_arg:
                skip_next_arg = False
                continue
            if arg == "dispatcher":
                continue
            if arg == "--replay":
                skip_next_arg = True
                continue
            trailing_args.append(arg)

        if trailing_args:
            args.append("--")
            args.extend(trailing_args)
        return shlex.join(args)

    def __str__(self) -> str:
        if not (self.failed or self.crashed):
            return f"No failures in worker {self.pid}"
        msg = f"In worker {self.pid},"
        if self.failed:
            msg += " " + ", ".join(self.failed) + " failed"
        if self.crashed:
            if self.failed:
                msg += " and"
            msg += f" {self.crashed} crashed"
        cmd = self._build_replay_cmd()
        msg += f".\n Replay using '{cmd}'"
        if self.rr_trace_dir is not None:
            # TODO: Add link to fdb documentation
            msg += f"\n Replay recording with: fdb replay debug {self.rr_trace_dir}"
        return msg

    def should_share(self):
        return self.rr_trace_dir is not None and self.has_broken_tests()

    def has_broken_tests(self):
        return self.failed or self.crashed

    def broken_tests(self):
        tests = self.failed.copy()
        if self.crashed:
            tests.append(self.crashed)
        return tests


def get_test_cinderx_dir() -> Path:
    return get_cinderx_dir() / "PythonLib" / "test_cinderx"


def start_worker(
    runtests_config: libregrtest_runtests.RunTests,
    worker_timeout: int,
    use_rr: bool,
) -> WorkSender:
    """Start a worker process we can use to run tests"""
    d_r, w_w = os.pipe()
    os.set_inheritable(d_r, True)
    os.set_inheritable(w_w, True)

    w_r, d_w = os.pipe()
    os.set_inheritable(w_r, True)
    os.set_inheritable(d_w, True)

    pipe = MessagePipe(d_r, d_w)

    runtest_config_file = tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", delete=False
    )
    json.dump(dataclasses.asdict(runtests_config), runtest_config_file.file)
    cmd = [
        sys.executable,
        *get_cinderjit_xargs(),
        WORKER_PATH,
        "worker",
        str(w_r),
        str(w_w),
        str(runtest_config_file.name),
    ]
    env = dict(os.environ)
    # This causes fdb/rr to panic when its set to a unicode value. This is
    # unconditionally set for regression tests as of Python 3.10. The docs
    # recommend setting it to 0 when it causes issues with the test
    # environment. We set it unconditionally to eliminate a difference between
    # running tests with/without rr.
    env["PYTHONREGRTEST_UNICODE_GUARD"] = "0"
    rr_trace_dir = None
    if use_rr:
        # RR gets upset if it's not allowed to create a directory itself so we
        # make a temporary directory and then let it create another inside.
        rr_trace_dir = tempfile.mkdtemp(prefix="rr-", dir=CINDER_RUNNER_LOG_DIR)
        rr_trace_dir += "/d"
        cmd = RR_RECORD_BASE_CMD + [f"--recording-dir={rr_trace_dir}"] + cmd
    if worker_timeout != 0:
        cmd = ["timeout", "--foreground", f"{worker_timeout}s"] + cmd

    popen = subprocess.Popen(cmd, pass_fds=(w_r, w_w), cwd=os_helper.SAVEDCWD, env=env)
    os.close(w_r)
    os.close(w_w)

    return WorkSender(pipe, popen, rr_trace_dir)


def manage_worker(
    runtests_config: libregrtest_runtests.RunTests,
    testq: queue.Queue,
    resultq: queue.Queue,
    worker_timeout: int,
    worker_respawn_interval: int,
    use_rr: bool,
) -> None:
    """Spawn and manage a subprocess to execute tests.

    This handles spawning worker processes that crash and periodically restarting workers
    in order to avoid consuming too much memory.
    """
    worker = start_worker(runtests_config, worker_timeout, use_rr)
    result = None
    while not isinstance(result, WorkerDone):
        msg = testq.get()
        if isinstance(msg, RunTest):
            resultq.put(
                TestStarted(
                    worker.pid, msg.test_name, worker.test_log.path, worker.rr_trace_dir
                )
            )
        try:
            worker.send(msg)
            result = worker.recv()
        except (BrokenPipeError, EOFError):
            if isinstance(msg, ShutdownWorker):
                # Worker exited cleanly
                resultq.put(WorkerDone())
                break
            elif isinstance(msg, RunTest):
                # Worker crashed while running a test
                test_result = libregrtest_result.TestResult(
                    msg.test_name, state=libregrtest_result.State.WORKER_FAILED
                )
                result = TestComplete(msg.test_name, test_result)
                resultq.put(result)
                worker.wait()
                worker = start_worker(runtests_config, worker_timeout, use_rr)
        else:
            resultq.put(result)
            if worker.ncompleted == worker_respawn_interval:
                # Respawn workers periodically to avoid oom-ing the machine
                worker.shutdown()
                worker = start_worker(runtests_config, worker_timeout, use_rr)
    worker.wait()


def _computeSkipTests(huntrleaks, use_rr=False) -> Tuple[Set[str], Set[str]]:
    skip_list_files = ["devserver_skip_tests.txt", "cinder_skip_test.txt"]

    if support.check_sanitizer(address=True):
        skip_list_files.append("asan_skip_tests.txt")

    if use_rr:
        skip_list_files.append("rr_skip_tests.txt")

    try:
        import cinderjit  # noqa: F401

        skip_list_files.append("cinder_jit_ignore_tests.txt")
    except ImportError:
        pass

    if huntrleaks:
        skip_list_files.append("refleak_skip_tests.txt")

    skip_modules = set()
    skip_patterns = set()

    for skip_file in skip_list_files:
        with open(os.path.join(os.path.dirname(__file__), skip_file)) as fp:
            for line in fp:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if len({".", "*"} & set(line)):
                    skip_patterns.add(line)
                else:
                    skip_modules.add(line)

    return skip_modules, skip_patterns


class MultiWorkerCinderRegrtest:
    def __init__(
        self,
        logfile: IO,
        tests: Iterable[str],
        worker_timeout: int,
        worker_respawn_interval: int,
        success_on_test_errors: bool,
        use_rr: bool,
        recording_metadata_path: str,
        no_retry_on_test_errors: bool,
        huntrleaks: bool,
        num_workers: int,
        json_summary_file: str | None,
    ):
        self._cinder_regr_runner_logfile = logfile
        self._success_on_test_errors = success_on_test_errors
        self._worker_timeout = worker_timeout
        self._worker_respawn_interval = worker_respawn_interval
        self._use_rr = use_rr
        self._recording_metadata_path = recording_metadata_path
        self._no_retry_on_test_errors = no_retry_on_test_errors
        self._huntrleaks = huntrleaks
        self._num_workers = num_workers
        self._json_summary_file = json_summary_file

        self._ntests_done = 0
        self._interrupted = False

        self._results = libregrtest_results.TestResults()
        # False, False => quiet, pgo
        self._logger = libregrtest_logger.Logger(self._results, False, False)

        skip_modules, skip_patterns = _computeSkipTests(self._huntrleaks, self._use_rr)

        if tests is None:
            tests = self._selectTests(skip_modules)

        self._runtests_config = libregrtest_runtests.RunTests(
            tests=tuple(tests),
            fail_fast=False,
            fail_env_changed=True,
            match_tests=[(p, False) for p in skip_patterns],
            match_tests_dict={},
            rerun=False,
            forever=False,
            pgo=False,
            pgo_extended=False,
            output_on_failure=True,
            timeout=None,
            verbose=0,
            quiet=False,
            hunt_refleak=(
                libregrtest_runtests.HuntRefleak(
                    warmups=5, runs=4, filename="reflog.txt"
                )
                if huntrleaks
                else None
            ),
            test_dir=None,
            use_junit=False,
            memory_limit=None,
            gc_threshold=None,
            use_resources=(),
            python_cmd=None,
            randomize=False,
            random_seed=1,
        )

    def _run_tests_with_n_workers(  # noqa: C901
        self, tests: Iterable[str], n_workers: int, replay_infos: List[ReplayInfo]
    ) -> None:
        runtests_config = self._runtests_config.copy(tests=())
        resultq = queue.Queue()
        testq = queue.Queue()
        ntests_remaining = 0
        for test in tests:
            ntests_remaining += 1
            testq.put(RunTest(test))

        threads = []
        for _i in range(n_workers):
            testq.put(ShutdownWorker())
            t = threading.Thread(
                target=manage_worker,
                args=(
                    runtests_config,
                    testq,
                    resultq,
                    self._worker_timeout,
                    self._worker_respawn_interval,
                    self._use_rr,
                ),
            )
            t.start()
            threads.append(t)

        try:
            active_tests: Dict[str, ActiveTest] = {}
            worker_infos: Dict[int, ReplayInfo] = {}
            while ntests_remaining:
                if not any(t.is_alive() for t in threads):
                    raise Exception("No worker threads alive, but tests remain to run.")
                try:
                    msg = resultq.get(timeout=10)
                except queue.Empty:
                    print_running_tests(active_tests)
                    continue
                if isinstance(msg, TestStarted):
                    active_tests[msg.test_name] = ActiveTest(
                        msg.worker_pid, time.time(), msg.test_log, msg.rr_trace_dir
                    )
                    print(
                        f"Running test '{msg.test_name}' on worker "
                        f"{msg.worker_pid}",
                        file=self._cinder_regr_runner_logfile,
                    )
                    self._cinder_regr_runner_logfile.flush()
                elif isinstance(msg, TestComplete):
                    ntests_remaining -= 1
                    self._ntests_done += 1
                    result = msg.result
                    if result.state not in (
                        libregrtest_result.State.PASSED,
                        libregrtest_result.State.RESOURCE_DENIED,
                        libregrtest_result.State.SKIPPED,
                        libregrtest_result.State.DID_NOT_RUN,
                    ):
                        worker_pid = active_tests[msg.test_name].worker_pid
                        rr_trace_dir = active_tests[msg.test_name].rr_trace_dir
                        err = f"TEST ERROR: {msg.result} in pid {worker_pid}"
                        if rr_trace_dir:
                            # TODO: Add link to fdb documentation
                            err += f" Replay recording with: fdb replay debug {rr_trace_dir}"
                        log_err(f"{err}\n")
                        if worker_pid not in worker_infos:
                            log = active_tests[msg.test_name].worker_test_log
                            worker_infos[worker_pid] = ReplayInfo(
                                worker_pid, log, rr_trace_dir
                            )
                        replay_info = worker_infos[worker_pid]
                        if result.state == libregrtest_result.State.WORKER_FAILED:
                            replay_info.crashed = msg.test_name
                        else:
                            replay_info.failed.append(msg.test_name)
                    self._results.accumulate_result(msg.result, self._runtests_config)
                    self._logger.display_progress(self._ntests_done, msg.test_name)
                    del active_tests[msg.test_name]
        except KeyboardInterrupt:
            replay_infos.extend(worker_infos.values())
            self._show_replay_infos(replay_infos)
            self._save_recording_metadata(replay_infos)
            # Kill the whole process group, getting rid of this process and all
            # workers, including those which have gone a bit rogue.
            os.killpg(os.getpgid(os.getpid()), signal.SIGTERM)
            self._interrupted = True

        replay_infos.extend(worker_infos.values())
        for t in threads:
            t.join()

    def _show_replay_infos(self, replay_infos: List[ReplayInfo]) -> None:
        if not replay_infos:
            return
        info = ["", "You can replay failed tests using the commands below:"]
        seen = 0
        for ri in replay_infos:
            info.append("    " + str(ri))
            seen += 1
            if seen == 10:
                info.append("NOTE: Only showing 10 instances")
                break
        info.append("\n")
        log_err("\n".join(info))

    def _save_recording_metadata(self, replay_infos: List[ReplayInfo]) -> None:
        if not self._recording_metadata_path:
            return

        recordings = [
            {"tests": info.broken_tests(), "recording_path": info.rr_trace_dir}
            for info in replay_infos
            if info.should_share()
        ]

        metadata = {"recordings": recordings}

        os.makedirs(os.path.dirname(self._recording_metadata_path), exist_ok=True)
        with open(self._recording_metadata_path, "w") as f:
            json.dump(metadata, f)

    def run(self):
        if self._huntrleaks:
            unittest.BaseTestSuite._cleanup = False

        self._logger.set_tests(self._runtests_config)
        replay_infos: List[ReplayInfo] = []
        self._run_tests_with_n_workers(
            [t for t in self._runtests_config.tests if t not in TESTS_TO_SERIALIZE],
            self._num_workers,
            replay_infos,
        )
        if not self._interrupted:
            print("Running serial tests")
            self._run_tests_with_n_workers(
                [t for t in self._runtests_config.tests if t in TESTS_TO_SERIALIZE],
                1,
                replay_infos,
            )

        # False, False => quiet, print_slowest
        self._results.display_result(self._runtests_config.tests, False, False)

        self._show_replay_infos(replay_infos)

        self._save_recording_metadata(replay_infos)

        if (
            not self._interrupted
            and not self._no_retry_on_test_errors
            and self._results.need_rerun()
        ):
            rerun_tests, rereun_match_tests_dict = self._results.prepare_rerun()
            print()
            self._logger.log(f"Re-running {len(rerun_tests)} failed tests serially.")
            self._runtests_config = self._runtests_config.copy(
                tests=rerun_tests,
                rerun=True,
                verbose=True,
                forever=False,
                fail_fast=False,
                match_tests_dict=rereun_match_tests_dict,
                output_on_failure=False,
            )
            self._logger.set_tests(self._runtests_config)
            self._run_tests_with_n_workers(
                rerun_tests,
                1,
                replay_infos,
            )
            if self._results.bad:
                print(
                    f"{libregrtest_utils.count(len(self._results.bad), "test")} failed again:"
                )
                libregrtest_utils.printlist(self._results.bad)

            # False, False => quiet, print_slowest
            self._results.display_result(self._runtests_config.tests, False, False)

        if self._json_summary_file:
            print("Writing JSON summary to", self._json_summary_file)
            with open(self._json_summary_file, "w") as f:
                json.dump(
                    {
                        "bad": sorted(self._results.bad),
                        "good": sorted(self._results.good),
                        "skipped": sorted(self._results.skipped),
                        "resource_denied": sorted(self._results.resource_denied),
                        "run_no_tests": sorted(self._results.run_no_tests),
                    },
                    f,
                )

        if not self._success_on_test_errors:
            # True, True => fail_env_changed, fail_rerun
            sys.exit(self._results.get_exitcode(True, True))
        sys.exit(0)

    def _selectTests(self, exclude: Set[str]) -> List[str]:
        # Initial set of tests are the core Python ones.
        tests = libregrtest_findtests.findtests(
            exclude=exclude,
            base_mod="test",
            split_test_dirs={"test." + d for d in libregrtest_findtests.SPLITTESTDIRS},
        )

        # Add CinderX tests
        cinderx_tests = libregrtest_findtests.findtests(
            testdir=get_test_cinderx_dir(),
            exclude=exclude,
            split_test_dirs=CINDERX_SPLIT_TEST_DIRS,
            base_mod="test_cinderx",
        )
        tests.extend(cinderx_tests)

        # findtests won't discover the static tests that don't start with test_, so manually
        # add those (it would find just test_static if we didn't split on that, but we want
        # to parallelize all of the static tests)
        testdir = libregrtest_findtests.findtestdir(
            get_test_cinderx_dir() / Path("test_compiler/test_static")
        )
        tests.extend(get_cinderx_static_tests(testdir))

        return tests


# TODO(T184566736) Remove this work around for a bug in Buck2 which causes
# us to be the parent of fire-and-forget logging processes.
def fix_env_always_changed_issue():
    # Build a list of our already existing child-processes.
    current_pid = os.getpid()
    result = subprocess.run(
        ["pgrep", "-P", str(current_pid)], capture_output=True, text=True
    )
    pids = result.stdout.strip().split("\n")
    child_pids_ignore = {int(pid) for pid in pids if pid}

    # This is a copy of test.support.reap_children() altered to ignore our
    # initial children. We monkey-patch this version in below.
    def reap_children():
        """Use this function at the end of test_main() whenever sub-processes
        are started.  This will help ensure that no extra children (zombies)
        stick around to hog resources and create problems when looking
        for refleaks.
        """

        # Need os.waitpid(-1, os.WNOHANG): Windows is not supported
        if not (hasattr(os, "waitpid") and hasattr(os, "WNOHANG")):
            return

        # Reap all our dead child processes so we don't leave zombies around.
        # These hog resources and might be causing some of the buildbots to die.
        while True:
            try:
                # Read the exit status of any child process which already completed
                pid, status = os.waitpid(-1, os.WNOHANG)
            except OSError:
                break

            # *CINDER CHANGE HERE*
            if pid in child_pids_ignore:
                continue

            if pid == 0:
                break

            support.print_warning(f"reap_children() reaped child process {pid}")
            support.environment_altered = True

    assert type(support.reap_children) is types.FunctionType
    support.reap_children = reap_children


# This allows much finer grained control over what tests are run e.g.
# test.test_asyncgen.AsyncGenTests.test_await_for_iteration.
def patch_libregrtest_to_use_loadTestsFromName():
    # Mostly a copy of test.libregrtest.single.run_unittest
    def patched_run_unittest(test_name):
        loader = libregrtest_single.unittest.TestLoader()
        # Primary change for CinderX is this line
        tests = loader.loadTestsFromName(test_name, None)
        for error in loader.errors:
            print(error, file=sys.stderr)
        if loader.errors:
            raise Exception("errors while loading tests")
        libregrtest_single._filter_suite(tests, libregrtest_single.match_test)
        return libregrtest_single._run_suite(tests)

    def patched__load_run_test(
        result: libregrtest_single.TestResult, runtests: libregrtest_single.RunTests
    ) -> None:
        test_name = result.test_name

        def test_func():
            return patched_run_unittest(test_name)

        try:
            libregrtest_single.regrtest_runner(result, test_func, runtests)
        finally:
            # First kill any dangling references to open files etc.
            # This can also issue some ResourceWarnings which would otherwise get
            # triggered during the following test run, and possibly produce
            # failures.
            support.gc_collect()

            libregrtest_single.remove_testfn(test_name, runtests.verbose)

        if gc.garbage:
            support.environment_altered = True
            libregrtest_single.print_warning(
                f"{test_name} created {len(gc.garbage)} " f"uncollectable object(s)"
            )

            # move the uncollectable objects somewhere,
            # so we don't see them again
            libregrtest_single.GC_GARBAGE.extend(gc.garbage)
            gc.garbage.clear()

        support.reap_children()

    libregrtest_single._load_run_test = patched__load_run_test


def user_selected_main(args):
    sys.argv[1:] = args.rest[1:]

    sys.path.insert(0, str(get_cinderx_dir() / "PythonLib"))

    patch_libregrtest_to_use_loadTestsFromName()

    fix_env_always_changed_issue()

    if args.verbose:
        sys.argv.append("-v")

    # Get our own copy of parsed args so we can decide whether to monkey-patch.
    ns = libregrtest_parse_args(sys.argv[1:])
    if not ns.verbose and not ns.huntrleaks:
        # Test progress/status via dots etc. The maze of CPython test code
        # makes it hard to do this without monkey-patching or writing a ton
        # of new code.
        from unittest import TextTestResult

        old_init = TextTestResult.__init__

        def force_dots_output(self, *args, **kwargs):
            old_init(self, *args, **kwargs)
            self.dots = True

        TextTestResult.__init__ = force_dots_output

    skip_modules, skip_patterns = _computeSkipTests(ns.huntrleaks)
    sys.argv.extend(sum([["-x", m] for m in skip_modules], []))
    sys.argv.extend(sum([["-i", p] for p in skip_patterns], []))

    libregrtest_main(tests=args.test)


def worker_main(args):
    sys.path.insert(0, str(get_cinderx_dir() / "PythonLib"))
    patch_libregrtest_to_use_loadTestsFromName()
    libregrtest_setup.setup_process()
    with open(args.runtest_config_json_file, "r") as f:
        worker_runtests_dict = json.load(f)
    os.unlink(args.runtest_config_json_file)
    worker_runtests = libregrtest_runtests.RunTests(**worker_runtests_dict)
    with MessagePipe(args.cmd_fd, args.result_fd) as pipe:
        asan_log = ASANLogManipulator()
        # Create a temporary directory for test runs
        with os_helper.temp_cwd(name=f"{tempfile.gettempdir()}/worker-{os.getpid()}"):
            msg = pipe.recv()
            while not isinstance(msg, ShutdownWorker):
                if isinstance(msg, RunTest):
                    asan_log.log(f"Running module {msg.test_name}")
                    asan_log.put_env_module_log_path(msg.test_name)
                    result = libregrtest_single.run_single_test(
                        msg.test_name, worker_runtests
                    )
                    pipe.send(TestComplete(msg.test_name, result))
                msg = pipe.recv()
            pipe.send(WorkerDone())


def dispatcher_main(args):
    sys.path.insert(0, str(get_cinderx_dir() / "PythonLib"))
    libregrtest_setup.setup_process()
    pathlib.Path(CINDER_RUNNER_LOG_DIR).mkdir(parents=True, exist_ok=True)
    if args.replay:
        assert args.num_workers is None, "Cannot specify --num-workers with --replay"
        assert args.test is None, "Cannot specify specific tests with --replay"
        tests = TestLog(path=args.replay).test_order
        num_workers = 1
    else:
        tests = args.test
        num_workers = args.num_workers or min(multiprocessing.cpu_count(), MAX_WORKERS)
    try:
        with tempfile.NamedTemporaryFile(
            delete=False, mode="w+t", dir=CINDER_RUNNER_LOG_DIR
        ) as logfile:
            print(f"Using scheduling log file {logfile.name}")
            test_runner = MultiWorkerCinderRegrtest(
                logfile,
                tests,
                args.worker_timeout,
                args.worker_respawn_interval,
                args.success_on_test_errors,
                args.use_rr,
                args.recording_metadata_path,
                args.no_retry_on_test_errors,
                args.huntrleaks,
                num_workers,
                args.json_summary_file,
            )
            print(f"Spawning {num_workers} workers")
            test_runner.run()
    finally:
        if args.use_rr:
            print(
                "Consider cleaning out RR data with: "
                f"rm -rf {CINDER_RUNNER_LOG_DIR}/rr-*"
            )


def main():
    # Apparently some tests need this for consistency with other Python test
    # running environments. Notably test_embed.
    try:
        sys.executable = os.path.realpath(sys.executable)
    except OSError:
        pass

    parser = argparse.ArgumentParser()

    # Limit the amount of RAM per process by default to cause a quick OOM on
    # runaway loops. This 8GiB number is arbitrary but seems to be enough at
    # the time of writing.
    mem_limit_default = -1 if is_asan_build() else 8192 * 1024 * 1024

    # Increase default stack size for threads in ASAN builds as this can use
    # a lot more stack space.
    if is_asan_build():
        threading.stack_size(1024 * 1024 * 10)

    parser.add_argument(
        "--memory-limit",
        type=int,
        help="Memory limit in bytes per worker or -1",
        default=mem_limit_default,
    )

    subparsers = parser.add_subparsers()

    worker_parser = subparsers.add_parser("worker")
    worker_parser.add_argument(
        "cmd_fd", type=int, help="Readable fd to receive commands to execute"
    )
    worker_parser.add_argument(
        "result_fd", type=int, help="Writable fd to write test results"
    )
    worker_parser.add_argument(
        "runtest_config_json_file",
        type=str,
        help="JSON file to read runtest config from",
    )
    worker_parser.set_defaults(func=worker_main)

    dispatcher_parser = subparsers.add_parser("dispatcher")
    dispatcher_parser.add_argument(
        "--json-summary-file", type=str, help="Path to write JSON summary to"
    )
    dispatcher_parser.add_argument(
        "--num-workers",
        type=int,
        help="Number of parallel test runners to use",
    )
    dispatcher_parser.add_argument(
        "--worker-timeout",
        type=int,
        help="Timeout for worker jobs (in seconds)",
        default=20 * 60,
    )
    dispatcher_parser.add_argument(
        "--worker-respawn-interval",
        type=int,
        help="Number of jobs to run in a worker before respawning.",
        default=WORKER_RESPAWN_INTERVAL,
    )
    dispatcher_parser.add_argument(
        "--use-rr",
        action="store_true",
        help="Run worker processes in RR",
    )
    dispatcher_parser.add_argument(
        "--recording-metadata-path",
        type=str,
        help="Path of recording metadata output file",
    )
    dispatcher_parser.add_argument(
        "--success-on-test-errors",
        action="store_true",
        help="Return with exit code 0 even if tests fail",
    )
    dispatcher_parser.add_argument(
        "--no-retry-on-test-errors",
        action="store_true",
        help="Do not retry tests which fail with verbose output",
    )
    dispatcher_parser.add_argument(
        "-t",
        "--test",
        action="append",
        help="The name of a test to run (e.g. `test_math`). Can be supplied multiple times.",
    )
    dispatcher_parser.add_argument(
        "--replay",
        type=str,
        help="Replay from log file",
    )
    dispatcher_parser.add_argument(
        "-R",
        "--huntrleaks",
        action="store_true",
        help="Check for refleaks",
    )
    dispatcher_parser.add_argument("rest", nargs=argparse.REMAINDER)
    dispatcher_parser.set_defaults(func=dispatcher_main)

    user_selected_parser = subparsers.add_parser("test")
    user_selected_parser.add_argument(
        "-t",
        "--test",
        action="append",
        required=True,
        help="The name of a test to run (e.g. `test_math`). Can be supplied multiple times.",
    )
    user_selected_parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="List tests as they run etc.",
    )
    user_selected_parser.add_argument("rest", nargs=argparse.REMAINDER)
    user_selected_parser.set_defaults(func=user_selected_main)

    args = parser.parse_args()

    # Equivalent of 'ulimit -s unlimited'.
    resource.setrlimit(
        resource.RLIMIT_STACK, (resource.RLIM_INFINITY, resource.RLIM_INFINITY)
    )

    if args.memory_limit != -1:
        resource.setrlimit(resource.RLIMIT_AS, (args.memory_limit, args.memory_limit))

    if hasattr(args, "func"):
        args.func(args)
    else:
        parser.error("too few arguments")


if __name__ == "__main__":
    main()

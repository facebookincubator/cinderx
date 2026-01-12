# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Functionality shared between 3.10 and 3.12 versions of cinder_test_runner*.py

import ctypes
import io
import json
import os
import os.path
import pickle
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional

from cinderx.test_support import is_asan_build


MAX_WORKERS = 64

# Respawn workers after they have run this many tests
WORKER_RESPAWN_INTERVAL = 10

CINDER_RUNNER_LOG_DIR = "/tmp/cinder_test_runner_logs"

# Tests that don't play nicely when run in parallel with other tests
TESTS_TO_SERIALIZE = {
    "test_ftplib",
    "test_epoll",
    "test_urllib2_localnet",
    "test_docxmlrpc",
    "test_filecmp",
    "test_cinderx.test_jitlist",
    "test_cinderx.test_cinderjit",
}

# Use the fdb debugging tool to invoke rr
RR_RECORD_BASE_CMD = [
    "fdb",
    "--caller-to-log",
    "cinder-jit-test-runner",
    "replay",
    "record",
]


@dataclass
class ActiveTest:
    worker_pid: int
    start_time: float
    worker_test_log: str
    rr_trace_dir: Optional[str]


class Message:
    pass


@dataclass(frozen=True)
class RunTest(Message):
    test_name: str


@dataclass(frozen=True)
class TestStarted(Message):
    worker_pid: int
    test_name: str
    test_log: str
    rr_trace_dir: Optional[str]


@dataclass(frozen=True)
class TestComplete(Message):
    test_name: str
    result: Any


class ShutdownWorker(Message):
    pass


class WorkerDone(Message):
    pass


class MessagePipe:
    def __init__(self, read_fd: int, write_fd: int) -> None:
        self.infile = os.fdopen(read_fd, "rb")
        self.outfile = os.fdopen(write_fd, "wb")

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def close(self) -> None:
        self.infile.close()
        self.outfile.close()

    def recv(self) -> Message:
        # @lint-ignore PYTHONPICKLEISBAD
        return pickle.load(self.infile)

    def send(self, message) -> None:
        # @lint-ignore PYTHONPICKLEISBAD
        pickle.dump(message, self.outfile)
        self.outfile.flush()


class TestLog:
    def __init__(self, pid: int = -1, path: None | str = None) -> None:
        self.pid = pid
        self.test_order: List[str] = []
        if path is None:
            self.path = tempfile.NamedTemporaryFile(delete=False).name
        else:
            self.path = path
            self._deserialize()

    def add_test(self, test_name: str) -> None:
        self.test_order.append(test_name)
        self._serialize()

    def _serialize(self) -> None:
        data = {
            "pid": self.pid,
            "test_order": self.test_order,
        }
        with tempfile.NamedTemporaryFile(mode="w+", delete=False) as tf:
            json.dump(data, tf)
            shutil.move(tf.name, self.path)

    def _deserialize(self) -> None:
        with open(self.path) as f:
            data = json.load(f)
            self.pid = data["pid"]
            self.test_order = data["test_order"]


class WorkSender:
    def __init__(
        self,
        pipe: MessagePipe,
        popen: subprocess.Popen,
        rr_trace_dir: Optional[str],
    ) -> None:
        self.pipe = pipe
        self.popen = popen
        self.ncompleted = 0
        self.rr_trace_dir = rr_trace_dir
        self.test_log = TestLog(popen.pid)

    @property
    def pid(self) -> int:
        return self.popen.pid

    def send(self, msg: Message) -> None:
        if isinstance(msg, RunTest):
            self.test_log.add_test(msg.test_name)
        self.pipe.send(msg)

    def recv(self) -> Message:
        msg = self.pipe.recv()
        if isinstance(msg, TestComplete):
            self.ncompleted += 1
        return msg

    def shutdown(self) -> None:
        self.pipe.send(ShutdownWorker())
        self.wait()

    def wait(self) -> None:
        assert self.popen is not None
        r = self.popen.wait()
        if r != 0 and self.rr_trace_dir is not None:
            print(
                f"Worker with PID {self.pid} ended with exit code {r}.\n"
                # TODO: Add link to fdb documentation
                f"Replay recording with: fdb replay debug {self.rr_trace_dir}"
            )
        self.pipe.close()


class ASANLogManipulator:
    def __init__(self) -> None:
        self._io: io.TextIOWrapper | None = None
        self._log_path_base = None
        self._base_asan_options = None

        if not is_asan_build():
            return

        asan_options = os.environ.get("ASAN_OPTIONS", "")
        log_path_base = None
        for option in asan_options.split(","):
            if option.startswith("log_path="):
                log_path_base = option[len("log_path=") :]
                break

        if log_path_base is None:
            return

        log_path = f"{log_path_base}-{uuid.uuid4()}"
        fd = os.open(log_path, os.O_WRONLY | os.O_CREAT, mode=0o644)
        ctypes.pythonapi["__sanitizer_set_report_fd"](fd)

        # IO must not close the fd or ASAN will not be able to write final info.
        self._io = io.TextIOWrapper(io.FileIO(fd, "a", closefd=False), encoding="utf-8")
        self._log_path_base = log_path_base
        self._base_asan_options = [
            opt for opt in asan_options.split(",") if not opt.startswith("log_path=")
        ]

        # Monkey patch individual test logging support
        old_startTest = unittest.TextTestResult.startTest

        def patched_startTest(text_test_runner_self, test):
            self.log(f"Starting test {test}")
            old_startTest(text_test_runner_self, test)

        unittest.TextTestResult.startTest = patched_startTest

    def log(self, s: str) -> None:
        if self._io is None:
            return
        io = self._io
        io.write(f"### {s}\n")
        io.flush()

    # Change the log path base which will be used only by *new* sub-processes.
    def put_env_module_log_path(self, module_name: str) -> None:
        if self._base_asan_options is None:
            return
        new_asan_options = self._base_asan_options + [
            f"log_path={self._log_path_base}-sub_process_of-{module_name}"
        ]
        os.putenv("ASAN_OPTIONS", ",".join(new_asan_options))


def get_cinderx_dir() -> Path:
    return Path(__file__).parent.parent


def get_cinderx_static_tests(testdir: str) -> list[str]:
    tests = []
    names = os.listdir(testdir)
    for name in names:
        mod, ext = os.path.splitext(name)
        if ext not in (".py",) or mod in ("__init__", "__main__", "common"):
            continue
        tests.append("test_cinderx.test_compiler.test_static." + mod)
    return tests


def print_running_tests(tests: Dict[str, ActiveTest]) -> None:
    if len(tests) == 0:
        print("No tests running")
        return
    now = time.time()
    msg_parts = ["Running tests:"]
    for k in sorted(tests.keys()):
        elapsed = int(now - tests[k].start_time)
        msg_parts.append(f"{k} ({elapsed}s, pid {tests[k].worker_pid})")
    print(" ".join(msg_parts))


def log_err(msg: str) -> None:
    sys.stderr.write(msg)
    sys.stderr.flush()

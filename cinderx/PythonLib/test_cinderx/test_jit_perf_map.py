# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import os.path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

import cinderx

cinderx.init()

from cinderx.test_support import ENCODING, passUnless, skip_unless_jit, subprocess_env

# Magic word at the start of every jitdump FileHeader (see perf_jitdump.cpp),
# stored little-endian.
_JITDUMP_MAGIC: int = 0x4A695444
# Six u32s (magic, version, total_size, elf_mach, pad1, pid) plus two u64s
# (timestamp, flags).
_JITDUMP_HEADER_SIZE: int = 40


class PerfMapTests(unittest.TestCase):
    def _run_perf_helper(
        self, helper_name: str, extra_env: dict[str, str] | None = None
    ) -> str:
        helper_file = os.path.join(
            os.path.dirname(__file__),
            helper_name,
        )
        env = subprocess_env()
        env.update(extra_env or {})
        proc: subprocess.CompletedProcess[str] = subprocess.run(
            [
                sys.executable,
                "-X",
                "cinderx-jit-perf-map",
                "-X",
                # Disable the inliner as it screws up this test's expectations.
                "jit-enable-hir-inliner=0",
                helper_file,
            ],
            stdout=subprocess.PIPE,
            encoding=ENCODING,
            env=env,
        )
        self.assertEqual(proc.returncode, 0)
        return proc.stdout

    def _read_perf_map(self, pid: int) -> str:
        path = f"/tmp/perf-{pid}.map"
        self.assertTrue(
            os.path.exists(path), f"process (pid {pid}) did not generate a map"
        )
        with open(path) as f:
            return f.read()

    def _find_mapped_funcs(self, stdout: str, which: str) -> set[str]:
        pattern = rf"{which}\(([0-9]+)\) computed "
        m = re.search(pattern, stdout)
        self.assertIsNotNone(m, f"Couldn't find /{pattern}/ in stdout:\n\n{stdout}")
        pid = int(m[1])
        map_contents = self._read_perf_map(pid)
        return set(re.findall("__CINDER_JIT:__main__:(.+)", map_contents))

    @passUnless(hasattr(os, "fork"), "fork not available on Windows")
    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_forked_pid_map(self) -> None:
        stdout = self._run_perf_helper("perf_fork_helper.py")

        self.assertEqual(
            self._find_mapped_funcs(stdout, "parent"), {"main", "parent", "compute"}
        )
        self.assertEqual(
            self._find_mapped_funcs(stdout, "child1"), {"main", "child1", "compute"}
        )
        self.assertEqual(
            self._find_mapped_funcs(stdout, "child2"), {"main", "child2", "compute"}
        )

    @passUnless(hasattr(os, "fork"), "fork not available on Windows")
    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_child_inherits_pre_fork_entries(self) -> None:
        # The child never calls before_fork itself; if the post-fork copy
        # works, its map still contains the parent's pre-fork entry, while
        # the parent's map never gains the child's entries.
        stdout = self._run_perf_helper("perf_fork_copy_helper.py")

        self.assertEqual(
            self._find_mapped_funcs(stdout, "parent"),
            {"before_fork"},
        )
        self.assertEqual(
            self._find_mapped_funcs(stdout, "child"),
            {"before_fork", "child_only"},
        )

    @passUnless(hasattr(os, "fork"), "fork not available on Windows")
    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_child_jitdump_preserves_header(self) -> None:
        # Reopening the copied JIT dump must not truncate it: every forked
        # process's dump has to start with the FileHeader copied from its
        # parent.
        with tempfile.TemporaryDirectory() as dump_dir:
            stdout = self._run_perf_helper(
                "perf_fork_helper.py", {"JITDUMPDIR": dump_dir}
            )

            pids = {int(pid) for pid in re.findall(r"\((\d+)\) computed ", stdout)}
            # parent + two children.
            self.assertEqual(len(pids), 3)
            for pid in pids:
                path = os.path.join(dump_dir, f"jit-{pid}.dump")
                self.assertTrue(
                    os.path.exists(path),
                    f"process (pid {pid}) did not generate a JIT dump",
                )
                with open(path, "rb") as f:
                    header = f.read(_JITDUMP_HEADER_SIZE)
                self.assertGreaterEqual(
                    len(header),
                    _JITDUMP_HEADER_SIZE,
                    f"JIT dump for pid {pid} is truncated",
                )
                (magic,) = struct.unpack("<I", header[:4])
                self.assertEqual(
                    magic, _JITDUMP_MAGIC, f"JIT dump for pid {pid} lost its header"
                )

    @passUnless(hasattr(os, "fork"), "fork not available on Windows")
    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_after_fork_child_reentrant(self) -> None:
        # Invoking the post-fork reinitialization twice in a row (as nested
        # fork handling can) must not crash on already-moved file state, e.g.
        # via fclose(nullptr) or a repeated munmap.
        script = (
            "import cinderx.jit\n"
            "import cinderjit\n"
            "import os\n"
            "\n"
            "\n"
            "def compute() -> int:\n"
            "    n = 0\n"
            "    for i in range(10):\n"
            "        n += i\n"
            "    return n\n"
            "\n"
            "\n"
            "cinderx.jit.lazy_compile(compute)\n"
            "compute()\n"
            "cinderjit.after_fork_child()\n"
            "cinderjit.after_fork_child()\n"
            'print(f"survived({os.getpid()})", flush=True)\n'
        )
        proc: subprocess.CompletedProcess[str] = subprocess.run(
            [
                sys.executable,
                "-X",
                "cinderx-jit-perf-map",
                "-X",
                f"cinderx-jit-dump-dir={tempfile.gettempdir()}",
                "-X",
                "jit-enable-hir-inliner=0",
                "-c",
                script,
            ],
            stdout=subprocess.PIPE,
            encoding=ENCODING,
            env=subprocess_env(),
        )
        self.assertEqual(proc.returncode, 0)
        self.assertIn("survived(", proc.stdout)

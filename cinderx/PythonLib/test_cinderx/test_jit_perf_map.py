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
# Record id of a CodeLoadRecord, the only kind of record CinderX writes.
_JIT_CODE_LOAD: int = 0
# sizeof(CodeLoadRecord): two u32s (id, total_size), a u64 timestamp, two u32s
# (pid, tid), then four u64s (vma, code_addr, code_size, code_index).  The
# symbol name follows it.
_CODE_LOAD_RECORD_SIZE: int = 56
_CODE_INDEX_OFFSET: int = 48

_FUNC_PREFIX: str = "__CINDER_JIT:__main__:"


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

    def _find_pid(self, stdout: str, which: str) -> int:
        pattern = rf"{which}\(([0-9]+)\) computed "
        m = re.search(pattern, stdout)
        self.assertIsNotNone(m, f"Couldn't find /{pattern}/ in stdout:\n\n{stdout}")
        return int(m[1])

    def _find_mapped_funcs(self, stdout: str, which: str) -> set[str]:
        map_contents = self._read_perf_map(self._find_pid(stdout, which))
        return set(re.findall(f"{_FUNC_PREFIX}(.+)", map_contents))

    def _run_script(self, script: str, dump_dir: str) -> str:
        """Run `script` with perf maps and JIT dumps enabled, passing it
        `dump_dir` as its only argument."""
        proc: subprocess.CompletedProcess[str] = subprocess.run(
            [
                sys.executable,
                "-X",
                "cinderx-jit-perf-map",
                "-X",
                f"cinderx-jit-dump-dir={dump_dir}",
                "-X",
                "jit-enable-hir-inliner=0",
                "-c",
                script,
                dump_dir,
            ],
            stdout=subprocess.PIPE,
            encoding=ENCODING,
            env=subprocess_env(),
        )
        self.assertEqual(proc.returncode, 0)
        return proc.stdout

    def _read_jitdump_names(self, path: str) -> list[str]:
        """Check that a JIT dump is a single header followed by well-formed
        records running exactly to EOF, and return the records' symbol names."""
        with open(path, "rb") as f:
            data = f.read()
        self.assertGreaterEqual(len(data), _JITDUMP_HEADER_SIZE, f"{path} is empty")
        magic, _version, header_size = struct.unpack_from("<III", data)
        self.assertEqual(magic, _JITDUMP_MAGIC, f"{path} has no header")

        names = []
        code_indices = set()
        offset = header_size
        while offset < len(data):
            record_id, record_size = struct.unpack_from("<II", data, offset)
            where = f"{path} at offset {offset}"
            self.assertEqual(record_id, _JIT_CODE_LOAD, f"Bad record id in {where}")
            self.assertGreater(
                record_size, _CODE_LOAD_RECORD_SIZE, f"Bad record size in {where}"
            )
            (code_index,) = struct.unpack_from("<Q", data, offset + _CODE_INDEX_OFFSET)
            self.assertNotIn(code_index, code_indices, f"Repeated index in {where}")
            code_indices.add(code_index)

            name_start = offset + _CODE_LOAD_RECORD_SIZE
            names.append(data[name_start : data.index(b"\0", name_start)].decode())
            offset += record_size

        self.assertEqual(offset, len(data), f"{path} ends in a truncated record")
        return names

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
        stdout = self._run_script(script, tempfile.gettempdir())
        self.assertIn("survived(", stdout)

    @passUnless(hasattr(os, "fork"), "fork not available on Windows")
    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_forked_jitdump_records(self) -> None:
        # Data buffered in the parent at fork time must land in the parent's
        # dump exactly once, so every dump parses cleanly and holds the same
        # functions as the matching perf map.
        with tempfile.TemporaryDirectory() as dump_dir:
            stdout = self._run_perf_helper(
                "perf_fork_helper.py", {"JITDUMPDIR": dump_dir}
            )

            for which in ["parent", "child1", "child2"]:
                pid = self._find_pid(stdout, which)
                names = self._read_jitdump_names(
                    os.path.join(dump_dir, f"jit-{pid}.dump")
                )
                funcs = {
                    name.removeprefix(_FUNC_PREFIX)
                    for name in names
                    if name.startswith(_FUNC_PREFIX)
                }
                self.assertEqual(funcs, {"main", which, "compute"})

    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_jitdump_written_while_running(self) -> None:
        # perf reads the dump of a live process, so entries must reach the
        # file as soon as the code is compiled, not when the process exits.
        script = (
            "import cinderx.jit\n"
            "import os\n"
            "import shutil\n"
            "import sys\n"
            "\n"
            "\n"
            "def compute() -> int:\n"
            "    n = 0\n"
            "    for i in range(10):\n"
            "        n += i\n"
            "    return n\n"
            "\n"
            "\n"
            "cinderx.jit.force_compile(compute)\n"
            "dump_dir = sys.argv[1]\n"
            "shutil.copyfile(\n"
            '    os.path.join(dump_dir, f"jit-{os.getpid()}.dump"),\n'
            '    os.path.join(dump_dir, "snapshot.dump"),\n'
            ")\n"
        )
        with tempfile.TemporaryDirectory() as dump_dir:
            self._run_script(script, dump_dir)
            names = self._read_jitdump_names(os.path.join(dump_dir, "snapshot.dump"))
        self.assertIn(f"{_FUNC_PREFIX}compute", names)

    @passUnless(hasattr(os, "fork"), "fork not available on Windows")
    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_child_survives_failed_jitdump_copy(self) -> None:
        # With the parent's dump gone the child can't copy it; the child must
        # carry on without a dump rather than keep using the parent's
        # already-closed file.
        script = (
            "import cinderx.jit\n"
            "import os\n"
            "import sys\n"
            "\n"
            "\n"
            "def before_fork() -> int:\n"
            "    n = 0\n"
            "    for i in range(10):\n"
            "        n += i\n"
            "    return n\n"
            "\n"
            "\n"
            "def child_only() -> int:\n"
            "    n = 1\n"
            "    for i in range(10):\n"
            "        n *= i + 1\n"
            "    return n\n"
            "\n"
            "\n"
            "cinderx.jit.force_compile(before_fork)\n"
            'os.unlink(os.path.join(sys.argv[1], f"jit-{os.getpid()}.dump"))\n'
            "pid = os.fork()\n"
            "if pid == 0:\n"
            "    cinderx.jit.force_compile(child_only)\n"
            "    sys.exit(0)\n"
            "_, status = os.waitpid(pid, 0)\n"
            'print(f"child exited with {os.waitstatus_to_exitcode(status)}")\n'
        )
        with tempfile.TemporaryDirectory() as dump_dir:
            stdout = self._run_script(script, dump_dir)
        self.assertIn("child exited with 0", stdout)

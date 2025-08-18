# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

# pyre-strict

import os.path
import re
import subprocess
import sys
import unittest

import cinderx

cinderx.init()

from cinderx.test_support import CINDERX_PATH, ENCODING, skip_unless_jit


class PerfMapTests(unittest.TestCase):
    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_forked_pid_map(self) -> None:
        helper_file = os.path.join(
            os.path.dirname(__file__),
            "perf_fork_helper.py",
        )
        proc: subprocess.CompletedProcess[str] = subprocess.run(
            [sys.executable, "-X", "jit", "-X", "jit-perfmap", helper_file],
            stdout=subprocess.PIPE,
            encoding=ENCODING,
            env={"PYTHONPATH": CINDERX_PATH},
        )
        self.assertEqual(proc.returncode, 0)

        def find_mapped_funcs(which: str) -> set[str]:
            pattern = rf"{which}\(([0-9]+)\) computed "
            m = re.search(pattern, proc.stdout)
            self.assertIsNotNone(
                m, f"Couldn't find /{pattern}/ in stdout:\n\n{proc.stdout}"
            )
            pid = int(m[1])
            map_contents = ""
            try:
                with open(f"/tmp/perf-{pid}.map") as f:
                    map_contents = f.read()
            except FileNotFoundError:
                self.fail(f"{which} process (pid {pid}) did not generate a map")

            funcs = set(re.findall("__CINDER_JIT:__main__:(.+)", map_contents))
            return funcs

        self.assertEqual(find_mapped_funcs("parent"), {"main", "parent", "compute"})
        self.assertEqual(find_mapped_funcs("child1"), {"main", "child1", "compute"})
        self.assertEqual(find_mapped_funcs("child2"), {"main", "child2", "compute"})

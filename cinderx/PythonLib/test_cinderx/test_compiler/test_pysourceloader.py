# Copyright (c) Meta Platforms, Inc. and affiliates.
import itertools
import os
import subprocess
import sys
import tempfile
from unittest import TestCase

import cinderx
from cinderx.test_support import subprocess_env


class PySourceLoaderTest(TestCase):
    def test_basic(self) -> None:
        tf = [False, True]
        for lazy_imports, no_pycs, strict in itertools.product(tf, tf, tf):
            with self.subTest(
                lazy_imports=lazy_imports, no_pycs=no_pycs, strict=strict
            ):
                with tempfile.TemporaryDirectory() as tmpdir:
                    env = os.environ.copy()
                    env.update(subprocess_env())
                    if strict:
                        env["PYTHONINSTALLSTRICTLOADER"] = "1"
                    else:
                        env["PYTHONUSEPYCOMPILER"] = "1"
                    if lazy_imports:
                        env["PYTHONLAZYIMPORTSALL"] = "1"
                    if no_pycs:
                        env["PYTHONPYCACHEPREFIX"] = tmpdir
                    proc = subprocess.run(
                        [sys.executable, "-c", "import xml; xml"],
                        capture_output=True,
                        env=env,
                    )
                    self.assertEqual(proc.returncode, 0, proc.stderr)

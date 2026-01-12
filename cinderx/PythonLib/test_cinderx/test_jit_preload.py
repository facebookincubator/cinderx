# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import itertools
import os.path
import subprocess
import sys
import unittest

import cinderx

cinderx.init()

import cinderx.jit
from cinderx.test_support import ENCODING, passIf, skip_unless_jit, subprocess_env

SKIP_315: bool = sys.version_info >= (3, 15)


class PreloadTests(unittest.TestCase):
    SCRIPT_FILE: str = os.path.join(
        os.path.dirname(__file__), "cinder_preload_helper_main.py"
    )

    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    @passIf(SKIP_315, "no lazy imports on 3.15 T243514540")
    def test_func_destroyed_during_preload(self) -> None:
        proc = subprocess.run(
            [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-batch-compile-workers=4",
                # Enable lazy imports
                "-L",
                "-mcinderx.compiler",
                "--static",
                self.SCRIPT_FILE,
            ],
            cwd=os.path.dirname(__file__),
            stdout=subprocess.PIPE,
            encoding=ENCODING,
            env=subprocess_env(),
        )
        self.assertEqual(proc.returncode, 0)
        expected_stdout = """resolving a_func
loading helper_a
defining main_func()
disabling jit
loading helper_b
jit disabled
<class 'NoneType'>
hello from b_func!
"""
        self.assertEqual(proc.stdout, expected_stdout)

    def test_preload_error(self) -> None:
        # don't include jit/no-jit in this matrix, decide it based on whether
        # overall test run is jit or no-jit; this avoids the confusion of jit
        # bugs showing up as failures in non-jit test runs
        for recursive, batch, lazyimports in itertools.product(
            [True, False],
            [True, False] if cinderx.jit.is_enabled() else [False],
            [True, False],
        ):
            if sys.version_info >= (3, 15) and lazyimports:
                # T243514540: lazy imports isn't available on 3.15
                continue

            root = os.path.join(
                os.path.dirname(__file__),
                "data/preload_error_recursive" if recursive else "data/preload_error",
            )
            jitlist = os.path.join(root, "jitlist.txt")
            cmd = [
                sys.executable,
                "-X",
                "install-strict-loader",
            ]
            if cinderx.jit.is_enabled():
                cmd += [
                    "-X",
                    f"jit-list-file={jitlist}",
                ]
                if batch:
                    cmd += [
                        "-X",
                        "jit-batch-compile-workers=2",
                    ]
            if lazyimports:
                cmd += ["-L"]
            cmd += ["main.py"]
            with self.subTest(
                recursive=recursive, batch=batch, lazyimports=lazyimports
            ):
                proc = subprocess.run(
                    cmd,
                    cwd=root,
                    capture_output=True,
                    env=subprocess_env(),
                )
                self.assertEqual(proc.returncode, 1, proc.stderr)
                self.assertIn(b"RuntimeError: boom\n", proc.stderr)

    def test_error_preloading_inlined(self) -> None:
        root = os.path.join(os.path.dirname(__file__), "data/error_preloading_inlined")
        jitlist = os.path.join(root, "jitlist.txt")
        main = os.path.join(root, "main.py")
        for lazy_imports, jit in itertools.product(
            [True, False],
            [True, False] if cinderx.jit.is_enabled() else [False],
        ):
            if sys.version_info >= (3, 15) and lazy_imports:
                # T243514540: lazy imports isn't available on 3.15
                continue

            with self.subTest(lazy_imports=lazy_imports, jit=jit):
                cmd = [sys.executable]
                if jit:
                    cmd.extend(
                        [
                            "-X",
                            f"jit-list-file={jitlist}",
                            "-X",
                            "jit-enable-hir-inliner",
                        ]
                    )
                if lazy_imports:
                    cmd.append("-L")
                cmd.append(main)
                proc = subprocess.run(
                    cmd,
                    cwd=root,
                    capture_output=True,
                    env=subprocess_env(),
                )
                # We expect an exception, but not a crash!
                self.assertEqual(proc.returncode, 1, proc.stderr)
                self.assertIn(
                    "RuntimeError: boom",
                    proc.stderr.decode(),
                    proc.stderr,
                )

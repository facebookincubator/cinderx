# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import itertools
import os.path
import subprocess
import sys
import unittest

import cinderx.jit
from cinderx.test_support import (
    ENCODING,
    has_meta_lazy_imports,
    passIf,
    passUnless,
    run_in_fork,
    skip_if_ft,
    skip_unless_jit,
    subprocess_env,
)

SKIP_315: bool = sys.version_info >= (3, 15)
META_LAZY_IMPORTS: bool = has_meta_lazy_imports()


class PreloadTests(unittest.TestCase):
    SCRIPT_FILE: str = os.path.join(
        os.path.dirname(__file__), "cinder_preload_helper_main.py"
    )
    RECURSIVE_SCRIPT_FILE: str = os.path.join(
        os.path.dirname(__file__), "cinder_recursive_preload_helper_main.py"
    )
    MP_SCRIPT_FILE: str = os.path.join(
        os.path.dirname(__file__), "cinder_mp_preload_helper_main.py"
    )

    @passIf(sys.version_info < (3, 14), "Requires deferred annotations")
    @run_in_fork
    def test_reentrant_preload_preserves_deletion_tracking(self) -> None:
        """
        Re-enter the JIT while outer is preloading:

            compile outer -> preload outer -> compile inner -> preload inner
                          -> replace outer.__code__
                          -> outer notices and drops its stale preload

        This must not leave outer interpreter-only. Its replacement should stay
        scheduled and compile on the next call.
        """
        cinderx.jit.enable_emit_type_annotation_guards()

        def outer_replacement(x: int) -> int:
            return x + 2

        def inner_annotation_hook() -> type:
            outer.__code__ = outer_replacement.__code__
            return int

        # pyrefly: ignore [invalid-annotation]
        def inner(x: inner_annotation_hook()) -> int:
            return x

        def outer_annotation_hook() -> type:
            self.assertEqual(inner(42), 42)
            return int

        # pyrefly: ignore [invalid-annotation]
        def outer(x: outer_annotation_hook()) -> int:
            return x + 1

        self.assertTrue(cinderx.jit.lazy_compile(inner))
        self.assertTrue(cinderx.jit.lazy_compile(outer))

        self.assertEqual(outer(1), 3)
        cinderx.jit.wait_for_background_compiles()

        self.assertTrue(
            cinderx.jit.is_jit_compiled(inner),
            "nested compilation did not run",
        )
        self.assertEqual(outer(1), 3)
        cinderx.jit.wait_for_background_compiles()
        self.assertTrue(
            cinderx.jit.is_jit_compiled(outer),
            "replacement code was not compiled on the next call",
        )

    @passIf(sys.version_info < (3, 14), "Requires deferred annotations")
    @run_in_fork
    def test_reentrant_preload_background_compile(self) -> None:
        """
        Re-enter the JIT while outer is preloading under background compilation:

            call outer -> schedule outer -> preload outer
                       -> call inner -> schedule inner -> preload inner
                       -> replace outer.__code__
                       -> outer notices and drops its stale preload
        """
        cinderx.jit.enable_emit_type_annotation_guards()
        cinderx.jit.background_compile(True)

        qual_prefix = (
            f"{self.__class__.__qualname__}."
            f"test_reentrant_preload_background_compile.<locals>"
        )
        cinderx.jit.append_jit_list(f"{__name__}:{qual_prefix}.inner")
        cinderx.jit.append_jit_list(f"{__name__}:{qual_prefix}.outer")

        def outer_replacement(x: int) -> int:
            return x + 2

        def inner_annotation_hook() -> type:
            outer.__code__ = outer_replacement.__code__
            return int

        # pyrefly: ignore [invalid-annotation]
        def inner(x: inner_annotation_hook()) -> int:
            return x

        def outer_annotation_hook() -> type:
            self.assertEqual(inner(42), 42)
            return int

        # pyrefly: ignore [invalid-annotation]
        def outer(x: outer_annotation_hook()) -> int:
            return x + 1

        self.assertEqual(outer(1), 3)
        cinderx.jit.wait_for_background_compiles()

        self.assertTrue(
            cinderx.jit.is_jit_compiled(inner),
            "nested compilation did not run",
        )
        self.assertEqual(outer(1), 3)
        cinderx.jit.wait_for_background_compiles()
        self.assertTrue(
            cinderx.jit.is_jit_compiled(outer),
            "replacement code was not compiled on the next call",
        )

    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    @passUnless(META_LAZY_IMPORTS, "Uses -L to enable Meta Python Lazy Imports")
    @skip_if_ft("Batch multi-threaded compile not supported with free threading")
    def test_func_destroyed_during_preload_multiprocessing(self) -> None:
        """
        Repro for T266490160 / D101755188 in a multiprocessing setup.

        Forked workers inherit any unit_deleted_during_preload callback the
        parent left installed, then trigger their own compilations whose
        preload runs JIT-compiled code that destroys functions.  If the
        unit-deleted callbacks are referencing invalid memory, they will
        crash.
        """

        proc = subprocess.run(
            [
                sys.executable,
                "-X",
                "jit-all",
                "-X",
                "jit-batch-compile-workers=2",
                "-L",
                self.MP_SCRIPT_FILE,
            ],
            cwd=os.path.dirname(__file__),
            capture_output=True,
            encoding=ENCODING,
            env={
                **subprocess_env(),
                "DISABLE_LAZY_IMPORTS": "1",
                "CINDERX_JIT_BACKGROUND_COMPILE": "0",
            },
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("ok ", proc.stdout, proc.stdout)

    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    @passUnless(META_LAZY_IMPORTS, "Uses -L to enable Meta Python Lazy Imports")
    @skip_if_ft("Batch multi-threaded compile not supported with free threading")
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
            # DISABLE_LAZY_IMPORTS prevents the safer_lazy_imports startup
            # function from overriding -L with selective lazy imports, which
            # would make the helper modules' imports eager and break this test.
            env={
                **subprocess_env(),
                "DISABLE_LAZY_IMPORTS": "1",
                "CINDERX_JIT_BACKGROUND_COMPILE": "0",
            },
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

    @passIf(
        sys.platform == "win32", "asyncio is failing to load in subprocess on Windows"
    )
    def test_preload_error(self) -> None:
        # don't include jit/no-jit in this matrix, decide it based on whether
        # overall test run is jit or no-jit; this avoids the confusion of jit
        # bugs showing up as failures in non-jit test runs
        for recursive, batch, lazyimports in itertools.product(
            [True, False],
            [True, False] if cinderx.jit.is_enabled() else [False],
            [True, False] if META_LAZY_IMPORTS else [False],
        ):
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

    @passIf(
        sys.platform == "win32", "asyncio is failing to load in subprocess on Windows"
    )
    def test_error_preloading_inlined(self) -> None:
        root = os.path.join(os.path.dirname(__file__), "data/error_preloading_inlined")
        jitlist = os.path.join(root, "jitlist.txt")
        main = os.path.join(root, "main.py")
        for lazy_imports, jit in itertools.product(
            [True, False] if META_LAZY_IMPORTS else [False],
            [True, False] if cinderx.jit.is_enabled() else [False],
        ):
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

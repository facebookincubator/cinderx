# Copyright (c) Meta Platforms, Inc. and affiliates.
from __future__ import annotations

import itertools
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest import TestCase

from cinderx.test_support import subprocess_env


_STATIC_PYTHON_PROBE = """
import json
import sys
import cinderx

evaluator_installed = cinderx.is_frame_evaluator_installed()
import static_startup
from cinderx.compiler.consts import CI_CO_STATICALLY_COMPILED

hooks = list(sys.path_hooks)
cinderx.init()
try:
    static_startup.f.__code__ = static_startup.f.__code__
    audit_blocked = False
except RuntimeError:
    audit_blocked = True
print(json.dumps({
    "initialized": cinderx.is_initialized(),
    "evaluator_installed": evaluator_installed,
    "static": bool(static_startup.f.__code__.co_flags & CI_CO_STATICALLY_COMPILED),
    "result": static_startup.f(),
    "audit_blocked": audit_blocked,
    "patching": isinstance(static_startup, cinderx.StrictModule)
        and cinderx.strict_module_patch_enabled(static_startup),
    "idempotent": hooks == sys.path_hooks,
}))
"""


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
                        env["CINDERX_STATIC_PYTHON"] = "1"
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

    def test_static_python_startup(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            Path(tmpdir, "static_startup.py").write_text(
                "import __static__\ndef f() -> int:\n    return 42\n"
            )
            for value, patching in itertools.product((None, "", "0", "1"), ("0", "1")):
                with self.subTest(value=value, patching=patching):
                    env = {**os.environ, **subprocess_env()}
                    env.pop("CINDERX_STATIC_PYTHON", None)
                    if value is not None:
                        env["CINDERX_STATIC_PYTHON"] = value
                    env["PYTHONENABLEPATCHING"] = patching
                    env["PYTHONPATH"] = tmpdir + os.pathsep + env["PYTHONPATH"]
                    proc = subprocess.run(
                        [sys.executable, "-c", _STATIC_PYTHON_PROBE],
                        capture_output=True,
                        env=env,
                    )
                    self.assertEqual(proc.returncode, 0, proc.stderr)
                    result = json.loads(proc.stdout)
                    enabled = value == "1"
                    self.assertTrue(result["initialized"])
                    self.assertEqual(result["static"], enabled)
                    self.assertEqual(result["audit_blocked"], enabled)
                    self.assertEqual(result["patching"], enabled and patching == "1")
                    self.assertEqual(result["result"], 42)
                    self.assertTrue(result["idempotent"])
                    if enabled:
                        self.assertTrue(result["evaluator_installed"])

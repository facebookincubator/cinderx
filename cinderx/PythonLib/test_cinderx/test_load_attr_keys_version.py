# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Regression tests for gh-115: a load site specialized to
LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES or LOAD_ATTR_METHOD_WITH_VALUES must
deopt once the attribute is stored on an instance. Runs in a subprocess
because it depends on precise interpreter warm-up state."""

import subprocess
import sys
import unittest

import cinderx

cinderx.init()

from cinderx.test_support import ENCODING, subprocess_env


def run_snippet(source: str) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(
        [sys.executable, "-c", source],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        encoding=ENCODING,
        env=subprocess_env(),
    )


class LoadAttrKeysVersionTests(unittest.TestCase):
    @unittest.skipUnless(sys.version_info >= (3, 14), "3.14+ opcodes")
    def test_instance_attr_shadows_class_default(self) -> None:
        proc = run_snippet(
            """if 1:
            import cinderx.jit
            cinderx.jit.auto()
            import dis

            class C:
                rcs = None

            def make_without():
                o = C.__new__(C)
                o.x1 = 1
                return o

            def make_with():
                o = C.__new__(C)
                o.x1 = 1
                o.rcs = (1, 2, 3)
                return o

            def read(o):
                return o.rcs

            for _ in range(300):
                assert read(make_without()) is None

            names = {i.opname for i in dis.get_instructions(read, adaptive=True)}
            assert "LOAD_ATTR_NONDESCRIPTOR_WITH_VALUES" in names, sorted(names)

            got = read(make_with())
            assert got == (1, 2, 3), f"stale class default returned: {got!r}"
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stdout)

    @unittest.skipUnless(sys.version_info >= (3, 14), "3.14+ opcodes")
    def test_instance_attr_shadows_method(self) -> None:
        proc = run_snippet(
            """if 1:
            import cinderx.jit
            cinderx.jit.auto()
            import dis

            class C:
                def m(self):
                    return "class-method"

            def make_plain():
                o = C.__new__(C)
                o.x1 = 1
                return o

            def make_shadowed():
                o = C.__new__(C)
                o.x1 = 1
                o.m = lambda: "instance-attr"
                return o

            def call_m(o):
                return o.m()

            for _ in range(300):
                assert call_m(make_plain()) == "class-method"

            names = {i.opname for i in dis.get_instructions(call_m, adaptive=True)}
            assert "LOAD_ATTR_METHOD_WITH_VALUES" in names, sorted(names)

            got = call_m(make_shadowed())
            assert got == "instance-attr", f"shadowed method ignored: {got!r}"
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stdout)


if __name__ == "__main__":
    unittest.main()

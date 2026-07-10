# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Regression test for gh-115: an AttributeError from a descriptor's
__get__ must fall back to __getattr__ in the JIT's method-call load
path. Runs in a subprocess because it depends on JIT warm-up state."""

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


class LoadMethodGetattrTests(unittest.TestCase):
    def test_unset_slot_falls_back_to_getattr(self) -> None:
        # An AttributeError from a descriptor's __get__ (here an unset slot)
        # must fall back to __getattr__ in the JIT's method-call load path.
        proc = run_snippet(
            """if 1:
            import cinderx.jit
            cinderx.jit.auto()

            class Base:
                __slots__ = ("__dict__",)

                def __getattr__(self, name):
                    if name == "evt":
                        def ls(*args):
                            return "called"
                        setattr(self, "evt", ls)
                        return ls
                    raise AttributeError(name)

            D = type("D", (Base,), {"__slots__": ("evt",)})

            def call_evt(d):
                return d.evt()

            assert cinderx.jit.force_compile(call_evt)

            warm = D()
            _ = warm.evt
            assert call_evt(warm) == "called"

            # Fresh instance: the slot is unset.
            assert call_evt(D()) == "called"
            """
        )
        self.assertEqual(proc.returncode, 0, proc.stdout)


if __name__ == "__main__":
    unittest.main()

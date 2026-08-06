# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import contextlib
import unittest
from collections.abc import Iterator
from types import FunctionType
from typing import cast

from cinderx import _context


@contextlib.contextmanager
def _one() -> Iterator[int]:
    yield 1


@contextlib.contextmanager
def _two_yields() -> Iterator[int]:
    yield 1
    yield 2


@unittest.skipIf(
    _context._patched_exit is None, "CinderX __exit__ replacement is not installed"
)
class ContextTest(unittest.TestCase):
    def test_normal_exit(self) -> None:
        with _one() as value:
            self.assertEqual(value, 1)

    def test_exception_propagates(self) -> None:
        with self.assertRaises(ValueError):
            with _one():
                raise ValueError("boom")

    def test_exception_suppressed_by_generator(self) -> None:
        @contextlib.contextmanager
        def swallow() -> Iterator[None]:
            try:
                yield
            except ValueError:
                pass

        with swallow():
            raise ValueError("boom")

    def test_generator_that_does_not_stop(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "generator didn't stop"):
            with _two_yields():
                pass

    def test_install_is_idempotent(self) -> None:
        installed = contextlib._GeneratorContextManager.__exit__
        _context.install()
        self.assertIs(contextlib._GeneratorContextManager.__exit__, installed)

    def test_next_or_sentinel(self) -> None:
        # pyre-ignore[21]: _cinderx is only importable where CinderX is
        # supported, which is where this test runs.
        from _cinderx import _next_or_sentinel, _NEXT_SENTINEL

        iterator = iter([1])
        self.assertEqual(_next_or_sentinel(iterator), 1)
        self.assertIs(_next_or_sentinel(iterator), _NEXT_SENTINEL)

    def test_patch_does_not_reference_cinderx_module(self) -> None:
        # Type dictionaries are never cleared during interpreter shutdown, so
        # anything the replacement captures outlives finalize_modules().  A
        # reference to _cinderx there keeps module_free() -- and with it the
        # deopt of every JIT-compiled function -- from ever running.
        # pyre-ignore[21]: See the matching ignore above.
        import _cinderx

        patched = cast(FunctionType, _context._patched_exit)
        reachable = [cell.cell_contents for cell in patched.__closure__ or ()]
        reachable.extend(patched.__globals__.values())
        for obj in reachable:
            self.assertIsNot(obj, _cinderx)
            self.assertIsNot(getattr(obj, "__self__", None), _cinderx)

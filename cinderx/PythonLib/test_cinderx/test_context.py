# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import contextlib
import os
import unittest
from collections.abc import Iterator
from types import FunctionType
from typing import Any, cast

from cinderx import _context
from cinderx.test_support import passIf


@contextlib.contextmanager
def _one() -> Iterator[int]:
    yield 1


@contextlib.contextmanager
def _two_yields() -> Iterator[int]:
    yield 1
    yield 2


@passIf(_context._patched_exit is None, "CinderX __exit__ replacement is not installed")
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
        # supported, which is where this test runs.
        from _cinderx import _next_or_sentinel, _NEXT_SENTINEL

        iterator = iter([1])
        self.assertEqual(_next_or_sentinel(iterator), 1)
        self.assertIs(_next_or_sentinel(iterator), _NEXT_SENTINEL)

    def test_environ_getitem(self) -> None:
        self.assertIs(os._Environ.__getitem__, _context._patched_getitem)

        key = "CINDERX_CONTEXT_TEST_VAR"
        os.environ.pop(key, None)
        try:
            os.environ[key] = "value"
            self.assertEqual(os.environ[key], "value")

            del os.environ[key]
            with self.assertRaises(KeyError) as ctx:
                os.environ[key]
            self.assertEqual(ctx.exception.args, (key,))
            self.assertIsNone(ctx.exception.__cause__)
            self.assertTrue(ctx.exception.__suppress_context__)
        finally:
            os.environ.pop(key, None)

    def test_environ_getitem_encodes_key(self) -> None:
        with self.assertRaises(TypeError):
            os.environ[cast(Any, 123)]

    def test_environ_get(self) -> None:
        self.assertIs(os._Environ.get, _context._patched_get)

        key = "CINDERX_CONTEXT_TEST_GET_VAR"
        os.environ.pop(key, None)
        try:
            os.environ[key] = "value"
            self.assertEqual(os.environ.get(key), "value")
            self.assertEqual(os.environ.get(key, "default"), "value")

            del os.environ[key]
            self.assertIsNone(os.environ.get(key))
            self.assertEqual(os.environ.get(key, "default"), "default")
            default = object()
            self.assertIs(os.environ.get(key, default), default)
        finally:
            os.environ.pop(key, None)

    def test_environ_get_encodes_key(self) -> None:
        with self.assertRaises(TypeError):
            os.environ.get(cast(Any, 123))
        with self.assertRaises(TypeError):
            os.environ.get(cast(Any, 123), "default")

    def test_environ_install_is_idempotent(self) -> None:
        installed = os._Environ.__getitem__
        installed_get = os._Environ.get
        _context.install()
        self.assertIs(os._Environ.__getitem__, installed)
        self.assertIs(os._Environ.get, installed_get)

    def test_patch_does_not_reference_cinderx_module(self) -> None:
        # Type dictionaries are never cleared during interpreter shutdown, so
        # anything the replacement captures outlives finalize_modules().  A
        # reference to _cinderx there keeps module_free() -- and with it the
        # deopt of every JIT-compiled function -- from ever running.
        import _cinderx

        for name in ("_patched_exit", "_patched_get", "_patched_getitem"):
            patched = cast(FunctionType, getattr(_context, name))
            reachable = [cell.cell_contents for cell in patched.__closure__ or ()]
            reachable.extend(patched.__globals__.values())
            for obj in reachable:
                self.assertIsNot(obj, _cinderx)
                self.assertIsNot(getattr(obj, "__self__", None), _cinderx)

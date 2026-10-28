# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import asyncio
import ctypes
import gc
import unittest

import cinderx.jit
from cinderx.test_support import failUnlessJITCompiled, skip_unless_jit


class CoroutinesTest(unittest.TestCase):
    def tearDown(self):
        # This is needed to avoid an "environment changed" error
        asyncio.set_event_loop(None)

    @failUnlessJITCompiled
    async def _f1(self):
        return 1

    @failUnlessJITCompiled
    async def _f2(self, await_target):
        return await await_target

    def test_basic_coroutine(self):
        c = self._f2(self._f1())
        with self.assertRaises(StopIteration) as exc:
            c.send(None)
        self.assertEqual(exc.exception.value, 1)

    def test_cannot_await_coro_already_awaiting_on_a_sub_iterator(self):
        class DummyAwaitable:
            def __await__(self):
                return iter([1])

        c = self._f2(DummyAwaitable())
        self.assertEqual(c.send(None), 1)
        with self.assertRaises(RuntimeError) as exc:
            self._f2(c).send(None)
        self.assertEqual(str(exc.exception), "coroutine is being awaited already")

    def test_works_with_asyncio(self):
        asyncio.run(self._f2(asyncio.sleep(0.1)))

    @staticmethod
    @failUnlessJITCompiled
    async def _use_async_with(mgr_type):
        async with mgr_type():
            pass

    def test_bad_awaitable_in_with(self):
        class BadAEnter:
            def __aenter__(self):
                pass

            async def __aexit__(self, exc, ty, tb):
                pass

        class BadAExit:
            async def __aenter__(self):
                pass

            def __aexit__(self, exc, ty, tb):
                pass

        with self.assertRaisesRegex(
            TypeError,
            "'async with' received an object from __aenter__ "
            "that does not implement __await__: NoneType",
        ):
            asyncio.run(self._use_async_with(BadAEnter))
        with self.assertRaisesRegex(
            TypeError,
            "'async with' received an object from __aexit__ "
            "that does not implement __await__: NoneType",
        ):
            asyncio.run(self._use_async_with(BadAExit))

    class FakeFuture:
        def __init__(self, obj):
            self._obj = obj

        def __await__(self):
            i = iter([self._obj])
            self._obj = None
            return i

    @skip_unless_jit("Exercises JIT-specific bug")
    def test_jit_coro_awaits_interp_coro(self):
        @cinderx.jit.jit_suppress
        async def eager_suspend(suffix):
            await self.FakeFuture("hello, " + suffix)

        @failUnlessJITCompiled
        async def jit_coro():
            await eager_suspend("bob")

        coro = jit_coro()
        v1 = coro.send(None)
        with self.assertRaises(StopIteration):
            coro.send(None)
        self.assertEqual(v1, "hello, bob")

    def assert_already_awaited(self, coro):
        with self.assertRaisesRegex(RuntimeError, "coroutine is being awaited already"):
            asyncio.run(coro)

    def test_already_awaited_coroutine_in_try_except(self):
        """Except blocks should execute when a coroutine is already awaited"""

        async def f():
            await asyncio.sleep(0.1)

        executed_except_block = False

        async def runner():
            nonlocal executed_except_block
            coro = f()
            loop = asyncio.get_running_loop()
            t = loop.create_task(coro)
            try:
                await asyncio.sleep(0)
                await coro
            except RuntimeError:
                executed_except_block = True
                t.cancel()
                raise

        self.assert_already_awaited(runner())
        self.assertTrue(executed_except_block)

    def test_already_awaited_coroutine_in_try_finally(self):
        """Finally blocks should execute when a coroutine is already awaited"""

        async def f():
            await asyncio.sleep(0.1)

        executed_finally_block = False

        async def runner():
            nonlocal executed_finally_block
            coro = f()
            loop = asyncio.get_running_loop()
            t = loop.create_task(coro)
            try:
                await asyncio.sleep(0)
                await coro
            finally:
                executed_finally_block = True
                t.cancel()

        self.assert_already_awaited(runner())
        self.assertTrue(executed_finally_block)

    def test_already_awaited_coroutine_in_try_except_finally(self):
        """Except and finally blocks should execute when a coroutine is already
        awaited.
        """

        async def f():
            await asyncio.sleep(0.1)

        executed_except_block = False
        executed_finally_block = False

        async def runner():
            nonlocal executed_except_block, executed_finally_block
            coro = f()
            loop = asyncio.get_running_loop()
            t = loop.create_task(coro)
            try:
                await asyncio.sleep(0)
                await coro
            except RuntimeError:
                executed_except_block = True
                raise
            finally:
                executed_finally_block = True
                t.cancel()

        self.assert_already_awaited(runner())
        self.assertTrue(executed_except_block)
        self.assertTrue(executed_finally_block)


class _ExcStateMarker(Exception):
    pass


_LEAK_MAP = {"key": "value"}


def _cython_style_get(mapping, key, default=None):
    """Simulate Cython's try/except exception save/reset (thrift Map.get).

    Cython's ExceptionSave reads the topmost handled exception
    (PyErr_GetExcInfo) and ExceptionReset writes it back into the current
    _PyErr_StackItem (PyErr_SetExcInfo). When the current item is a JIT
    coroutine's gi_exc_state, the saved exception (owned by an enclosing
    except block) is copied into the coroutine's exception state.
    """
    exc_type = ctypes.c_void_p()
    exc_value = ctypes.c_void_p()
    exc_tb = ctypes.c_void_p()
    ctypes.pythonapi.PyErr_GetExcInfo(
        ctypes.byref(exc_type), ctypes.byref(exc_value), ctypes.byref(exc_tb)
    )
    try:
        return mapping[key]
    except KeyError:
        return default
    finally:
        # PyErr_SetExcInfo steals a reference to each argument.
        ctypes.pythonapi.PyErr_SetExcInfo(exc_type, exc_value, exc_tb)


def _count_alive_markers() -> int:
    gc.collect()
    return sum(1 for obj in gc.get_objects() if isinstance(obj, _ExcStateMarker))


class ExcStateLeakTest(unittest.TestCase):
    """Regression test: a finished JIT coroutine/generator must not retain
    an exception in gi_exc_state.exc_value (see send_core in
    Jit/generators_rt.cpp). Retaining it leaks the exception, its
    traceback, and every frame local reachable from the traceback.
    """

    def tearDown(self):
        asyncio.set_event_loop(None)

    @failUnlessJITCompiled
    async def _callee(self):
        return _cython_style_get(_LEAK_MAP, "key")

    @failUnlessJITCompiled
    async def _handler(self):
        try:
            raise _ExcStateMarker()
        except _ExcStateMarker:
            return await self._callee()

    def test_coro_exc_state_cleared_on_finish(self):
        async def run() -> int:
            # Warm up (JIT compilation) before measuring.
            for _ in range(5):
                self.assertEqual(await self._handler(), "value")
            before = _count_alive_markers()
            for _ in range(50):
                self.assertEqual(await self._handler(), "value")
            return _count_alive_markers() - before

        self.assertEqual(asyncio.run(run()), 0)

    @failUnlessJITCompiled
    def _gen_callee(self):
        yield _cython_style_get(_LEAK_MAP, "key")

    @failUnlessJITCompiled
    def _gen_handler(self):
        try:
            raise _ExcStateMarker()
        except _ExcStateMarker:
            yield from self._gen_callee()

    def test_gen_exc_state_cleared_on_finish(self):
        for _ in range(5):
            self.assertEqual(list(self._gen_handler()), ["value"])
        before = _count_alive_markers()
        for _ in range(50):
            self.assertEqual(list(self._gen_handler()), ["value"])
        self.assertEqual(_count_alive_markers() - before, 0)

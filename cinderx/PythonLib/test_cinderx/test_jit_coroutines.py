# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import asyncio
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
            # pyrefly: ignore [not-async]
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

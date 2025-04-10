# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe

import asyncio
import dis
import sys
import unittest

import cinderx
cinderx.init()

import cinderx.test_support as cinder_support


AT_LEAST_312 = sys.version_info[:2] >= (3, 12)


@unittest.skipIf(AT_LEAST_312, "T194022335: Async generators not supported in 3.12 JIT yet")
class AsyncGeneratorsTest(unittest.TestCase):
    def tearDown(self):
        # This is needed to avoid an "environment changed" error
        asyncio.set_event_loop_policy(None)

    @cinder_support.failUnlessJITCompiled
    async def _f1(self, awaitable):
        x = yield 1
        yield x
        await awaitable

    def test_basic_coroutine(self):
        class DummyAwaitable:
            def __await__(self):
                return iter([3])

        async_gen = self._f1(DummyAwaitable())

        # Step 1: move through "yield 1"
        async_itt1 = async_gen.asend(None)
        with self.assertRaises(StopIteration) as exc:
            async_itt1.send(None)
        self.assertEqual(exc.exception.value, 1)

        # Step 2: send in and receive out 2 via "yield x"
        async_itt2 = async_gen.asend(2)
        with self.assertRaises(StopIteration) as exc:
            async_itt2.send(None)
        self.assertEqual(exc.exception.value, 2)

        # Step 3: yield of "3" from DummyAwaitable
        async_itt3 = async_gen.asend(None)
        self.assertEqual(async_itt3.send(None), 3)

        # Step 4: complete
        with self.assertRaises(StopAsyncIteration):
            async_itt3.send(None)

    @cinder_support.failUnlessJITCompiled
    async def _f2(self, asyncgen):
        res = []
        async for x in asyncgen:
            res.append(x)
        return res

    def test_for_iteration(self):
        async def asyncgen():
            yield 1
            yield 2

        self.assertEqual(asyncio.run(self._f2(asyncgen())), [1, 2])

    def _assertExceptionFlowsThroughYieldFrom(self, exc):
        tb_prev = None
        tb = exc.__traceback__
        while tb.tb_next:
            tb_prev = tb
            tb = tb.tb_next
        instrs = [x for x in dis.get_instructions(tb_prev.tb_frame.f_code)]
        self.assertEqual(
            instrs[tb_prev.tb_lasti // 2].opname,
            "YIELD_VALUE" if AT_LEAST_312 else "YIELD_FROM",
        )

    def test_for_exception(self):
        async def asyncgen():
            yield 1
            raise ValueError

        # Can't use self.assertRaises() as this clears exception tracebacks
        try:
            asyncio.run(self._f2(asyncgen()))
        except ValueError as e:
            self._assertExceptionFlowsThroughYieldFrom(e)
        else:
            self.fail("Expected ValueError to be raised")

    @cinder_support.failUnlessJITCompiled
    async def _f3(self, asyncgen):
        return [x async for x in asyncgen]

    def test_comprehension(self):
        async def asyncgen():
            yield 1
            yield 2

        self.assertEqual(asyncio.run(self._f3(asyncgen())), [1, 2])

    def test_comprehension_exception(self):
        async def asyncgen():
            yield 1
            raise ValueError

        # Can't use self.assertRaises() as this clears exception tracebacks
        try:
            asyncio.run(self._f3(asyncgen()))
        except ValueError as e:
            self._assertExceptionFlowsThroughYieldFrom(e)
        else:
            self.fail("Expected ValueError to be raised")

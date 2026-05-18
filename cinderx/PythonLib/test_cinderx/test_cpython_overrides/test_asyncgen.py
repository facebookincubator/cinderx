# Copyright (c) Meta Platforms, Inc. and affiliates.

import asyncio
import gc
import unittest

import cinderx.jit
from cinderx.test_support import passIf


class AwaitException(Exception):
    pass


class CinderX_AsyncGenAsyncioTest(unittest.TestCase):
    def setUp(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(None)

    def tearDown(self):
        self.loop.close()
        self.loop = None

    @passIf(cinderx.jit.is_enabled(), "fails under Cinder JIT")
    def test_async_gen_asyncio_gc_aclose_09(self) -> None:
        DONE = 0

        async def gen():
            nonlocal DONE
            try:
                while True:
                    yield 1
            finally:
                await asyncio.sleep(0.01)
                await asyncio.sleep(0.01)
                DONE = 1

        async def run():
            g = gen()
            await g.__anext__()
            await g.__anext__()
            del g

            gc.collect()
            gc.collect()
            gc.collect()

            # CinderX: This is changed from asyncio.sleep(1)
            await asyncio.sleep(0.1)

        self.loop.run_until_complete(run())
        self.assertEqual(DONE, 1)

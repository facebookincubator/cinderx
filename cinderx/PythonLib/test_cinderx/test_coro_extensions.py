# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

import sys
import types
import unittest
from collections.abc import AsyncGenerator, Callable, Coroutine, Generator, Iterator

from cinderx.test_support import hasCinderX, passIf, skip_if_jit, skip_module_if_oss

skip_module_if_oss()

# pyre-ignore[21]: can't find test.support
from test.support import import_helper, maybe_get_event_loop_policy


if hasCinderX():
    import cinder


def run_async(
    coro: Generator[object, None, object] | Coroutine[object, None, object],
) -> tuple[list[object], object | None]:
    assert coro.__class__ in {types.GeneratorType, types.CoroutineType}

    buffer = []
    result = None
    while True:
        try:
            buffer.append(coro.send(None))
        except StopIteration as ex:
            result = ex.args[0] if ex.args else None
            break
    return buffer, result


# in 3.12, awaiter tests are in GetAsyncStackTests in
# Lib/test/test_asyncio/test_tasks.py
# And eager execution is an entirely different implementation:
# https://docs.python.org/3.12/library/asyncio-task.html#eager-task-factory
@passIf(sys.version_info >= (3, 12), "not supported on 3.12+")
class CoroutineAwaiterTest(unittest.TestCase):
    def test_basic_await(self) -> None:
        async def coro() -> str:
            nonlocal awaiter_obj
            self.assertIs(cinder._get_coro_awaiter(coro_obj), awaiter_obj)
            return "success"

        async def awaiter() -> str:
            return await coro_obj

        coro_obj: Coroutine[object, None, str] = coro()
        awaiter_obj: Coroutine[object, None, str] = awaiter()
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))
        self.assertEqual(run_async(awaiter_obj), ([], "success"))
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))
        del awaiter_obj
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))

    class FakeFuture:
        def __await__(self) -> Iterator[str]:
            return iter(["future"])

    @skip_if_jit("no eager await with JIT")
    def test_eager_await(self) -> None:
        async def awaitee() -> str:
            nonlocal awaitee_frame
            awaitee_frame = sys._getframe()
            # pyre-ignore[16]: no attribute _get_frame_gen
            self.assertIsNone(cinder._get_frame_gen(awaitee_frame))
            # pyre-ignore[12]: FakeFuture implements __await__
            await self.FakeFuture()

            # Our caller verified our awaiter while we were suspended; ensure
            # it's still set while running.
            # pyre-ignore[16]: no attribute _get_frame_gen
            awaitee_obj = cinder._get_frame_gen(awaitee_frame)
            self.assertIsInstance(awaitee_obj, types.CoroutineType)
            self.assertIs(cinder._get_coro_awaiter(awaitee_obj), awaiter_obj)
            return "good!"

        async def awaiter() -> str:
            nonlocal awaiter_frame
            awaiter_frame = sys._getframe()
            return await awaitee()

        awaitee_frame: types.FrameType | None = None
        awaiter_frame: types.FrameType | None = None
        awaiter_obj: Coroutine[object, None, str] = awaiter()
        self.assertIsNone(awaiter_frame)
        self.assertIsNone(awaitee_frame)

        v1 = awaiter_obj.send(None)
        self.assertEqual(v1, "future")
        self.assertIsInstance(awaitee_frame, types.FrameType)
        self.assertIsInstance(awaiter_frame, types.FrameType)
        # pyre-ignore[16]: no attribute _get_frame_gen
        self.assertIs(cinder._get_frame_gen(awaiter_frame), awaiter_obj)
        self.assertIsNone(cinder._get_coro_awaiter(awaiter_obj))

        # pyre-ignore[16]: no attribute _get_frame_gen
        awaitee_obj = cinder._get_frame_gen(awaitee_frame)
        self.assertIsInstance(awaitee_obj, types.CoroutineType)
        self.assertIs(cinder._get_coro_awaiter(awaitee_obj), awaiter_obj)

        with self.assertRaises(StopIteration) as cm:
            awaiter_obj.send(None)
        self.assertEqual(cm.exception.value, "good!")
        self.assertIsNone(cinder._get_coro_awaiter(awaitee_obj))

        # Run roughly the same sequence again, with awaiter() executed eagerly.
        async def awaiter2() -> str:
            return await awaiter()

        awaitee_frame = None
        awaiter_frame = None
        awaiter2_obj = awaiter2()
        self.assertIsNone(cinder._get_coro_awaiter(awaiter2_obj))
        awaiter2_obj.send(None)

        self.assertIsInstance(awaitee_frame, types.FrameType)
        self.assertIsInstance(awaiter_frame, types.FrameType)
        # pyre-ignore[16]: no attribute _get_frame_gen
        awaitee_obj = cinder._get_frame_gen(awaitee_frame)
        # pyre-ignore[16]: no attribute _get_frame_gen
        awaiter_obj = cinder._get_frame_gen(awaiter_frame)
        self.assertIsInstance(awaitee_obj, types.CoroutineType)
        self.assertIsInstance(awaiter_obj, types.CoroutineType)
        self.assertIs(cinder._get_coro_awaiter(awaitee_obj), awaiter_obj)
        self.assertIs(cinder._get_coro_awaiter(awaiter_obj), awaiter2_obj)
        self.assertIsNone(cinder._get_coro_awaiter(awaiter2_obj))

        with self.assertRaises(StopIteration) as cm:
            awaiter2_obj.send(None)
        self.assertEqual(cm.exception.value, "good!")
        self.assertIsNone(cinder._get_coro_awaiter(awaitee_obj))
        self.assertIsNone(cinder._get_coro_awaiter(awaiter_obj))

    def test_coro_outlives_awaiter(self) -> None:
        async def coro() -> None:
            # pyre-ignore[12]: FakeFuture implements __await__
            await self.FakeFuture()

        async def awaiter(cr: Coroutine[object, None, None]) -> None:
            await cr

        coro_obj = coro()
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))
        awaiter_obj = awaiter(coro_obj)
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))

        v1 = awaiter_obj.send(None)
        self.assertEqual(v1, "future")
        self.assertIs(cinder._get_coro_awaiter(coro_obj), awaiter_obj)

        del awaiter_obj
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))

    def test_async_gen_doesnt_set(self) -> None:
        async def coro() -> None:
            # pyre-ignore[12]: FakeFuture implements __await__
            await self.FakeFuture()

        async def async_gen(
            cr: Coroutine[object, None, None],
        ) -> AsyncGenerator[str, None]:
            await cr
            yield "hi"

        # ci_cr_awaiter should always be None or a coroutine object, and async
        # generators aren't coroutines.
        coro_obj = coro()
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))
        agen = async_gen(coro_obj)
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))

        v1 = agen.asend(None).send(None)
        self.assertEqual(v1, "future")
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))

        del agen
        self.assertIsNone(cinder._get_coro_awaiter(coro_obj))


class TestEagerExecution(unittest.TestCase):
    def setUp(self) -> None:
        self._asyncio = import_helper.import_module("asyncio")
        policy = maybe_get_event_loop_policy()
        self.addCleanup(lambda: self._asyncio.set_event_loop_policy(policy))

    async def _raise_IndexError_eager(self, x: object = None) -> None:
        try:
            raise IndexError
        except:  # noqa B001
            pass

    async def _raise_IndexError_suspended(self, x: object = None) -> None:
        try:
            raise IndexError
        except:  # noqa B001
            await self._asyncio.sleep(0)

    def _check(
        self,
        expected_coro: Coroutine[object, None, object],
        actual_coro: Coroutine[object, None, object],
    ) -> None:
        def run(coro: Coroutine[object, None, object]) -> type[BaseException] | None:
            try:
                self._asyncio.run(coro)
                self.fail("Exception expected")
            except RuntimeError as e:
                if e.__context__ is None:
                    return None
                return type(e.__context__)

        self.assertEqual(run(expected_coro), run(actual_coro))

    def _do_test_exc_handler(  # noqa: C901
        self, f: Callable[..., Coroutine[object, None, None]]
    ) -> None:
        async def actual_1() -> None:
            try:
                raise ValueError
            except:  # noqa B001
                await f()
                raise RuntimeError

        async def expected_1() -> None:
            try:
                raise ValueError
            except:  # noqa B001
                coro = f()
                await coro
                raise RuntimeError

        async def actual_2() -> None:
            try:
                raise ValueError
            except:  # noqa B001
                await f(x=1)
                raise RuntimeError

        async def expected_2() -> None:
            try:
                raise ValueError

            except:  # noqa B001
                coro = f(x=1)
                await coro

                raise RuntimeError

        self._check(expected_1(), actual_1())
        self._check(expected_2(), actual_2())

    def _do_test_no_err(self, f: Callable[..., Coroutine[object, None, None]]) -> None:
        async def actual_1() -> None:
            await f()
            raise RuntimeError

        async def expected_1() -> None:
            coro = f()
            await coro
            raise RuntimeError

        async def actual_2() -> None:
            await f(x=1)

            raise RuntimeError

        async def expected_2() -> None:
            coro = f(x=1)

            await coro
            raise RuntimeError

        self._check(expected_1(), actual_1())

        self._check(expected_2(), actual_2())

    def test_eager_await_no_error_eager(self) -> None:
        self._do_test_no_err(self._raise_IndexError_eager)

    def test_suspended_await_no_error_suspended(self) -> None:
        self._do_test_no_err(self._raise_IndexError_suspended)

    def test_suspended_await_in_catch_eager(self) -> None:
        self._do_test_exc_handler(self._raise_IndexError_eager)

    def test_suspended_await_in_catch_suspended(self) -> None:
        self._do_test_exc_handler(self._raise_IndexError_suspended)


if __name__ == "__main__":
    unittest.main()

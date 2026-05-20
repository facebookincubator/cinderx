# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

import asyncio
import types
import unittest
from collections.abc import Callable, Coroutine, Generator


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


class TestEagerExecution(unittest.TestCase):
    async def _raise_IndexError_eager(self, x: object = None) -> None:
        try:
            raise IndexError
        except:  # noqa B001
            pass

    async def _raise_IndexError_suspended(self, x: object = None) -> None:
        try:
            raise IndexError
        except:  # noqa B001
            await asyncio.sleep(0)

    def _check(
        self,
        expected_coro: Coroutine[object, None, object],
        actual_coro: Coroutine[object, None, object],
    ) -> None:
        def run(coro: Coroutine[object, None, object]) -> type[BaseException] | None:
            try:
                asyncio.run(coro)
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

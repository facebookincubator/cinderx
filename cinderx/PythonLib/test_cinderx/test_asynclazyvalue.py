# Copyright (c) Meta Platforms, Inc. and affiliates.

import asyncio
import ctypes
import functools
import gc
import inspect
import os
import subprocess
import sys
import unittest
import weakref
from typing import Any, Callable, Coroutine, Generator

try:
    from _cinderx import AsyncLazyValue
except ImportError:
    # pyrefly: ignore [missing-module-attribute]
    from _asyncio import AsyncLazyValue

from functools import wraps
from textwrap import dedent
from time import time

from cinderx.test_support import ENCODING, subprocess_env


def async_test(
    f: Callable[..., Coroutine[Any, Any, Any]],
) -> Callable[..., None]:
    assert inspect.iscoroutinefunction(f)

    @wraps(f)
    def impl(*args: Any, **kwargs: Any) -> None:
        asyncio.run(f(*args, **kwargs))

    return impl


def make_task(
    task_type: type[asyncio.Task[Any]],
    loop: asyncio.AbstractEventLoop,
    coro: Any,
    **kwargs: Any,
) -> asyncio.Task[Any]:
    return task_type(coro, loop=loop, **kwargs)


class AsyncLazyValueCoroTest(unittest.TestCase):
    def setUp(self) -> None:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        self.loop = loop

    def tearDown(self) -> None:
        self.loop.close()
        asyncio.set_event_loop(None)

    @async_test
    async def test_close_not_started(self) -> None:
        async def g() -> None:
            pass

        # close non-started asynclazy value is no-op
        (AsyncLazyValue(g).__await__()).close()
        pass

    @async_test
    async def test_close_normal(self) -> None:
        async def g(fut: "asyncio.Future[Any]") -> None:
            await fut

        # close non-started asynclazy value is no-op
        alv = AsyncLazyValue(g, asyncio.Future())
        i = alv.__await__()  # get main coro
        i1 = alv.__await__()  # get future
        i.send(None)
        i.close()
        try:
            # check future has gotten generator exit
            next(i1)
            self.fail("should not be here")
        except GeneratorExit:
            pass

    @async_test
    async def test_close_subgen_error(self) -> None:
        class Exc(Exception):
            pass

        async def g(fut: "asyncio.Future[Any]") -> None:
            try:
                await fut
            except GeneratorExit:
                raise Exc("error")

        alv = AsyncLazyValue(g, asyncio.Future())
        i = alv.__await__()  # get main coro
        i1 = alv.__await__()  # get future
        i.send(None)
        try:
            i.close()
            self.fail("Error expected")
        except Exc:
            pass
        try:
            # check future has gotten Exc exit
            next(i1)
            self.fail("should not be here")
        except Exc as e:
            self.assertIs(type(e.__context__), GeneratorExit)
            pass

    @async_test
    async def test_throw_not_started(self) -> None:
        class Exc(Exception):
            pass

        async def g() -> None:
            pass

        alv = AsyncLazyValue(g)
        c = alv.__await__()
        try:
            c.throw(Exc)
            self.fail("Error expected")
        except Exc:
            pass

    @async_test
    async def test_throw_handled_in_subgen(self) -> None:
        class Exc(Exception):
            pass

        async def g(fut: "asyncio.Future[Any]") -> int | None:
            try:
                await fut
            except Exc:
                return 10
            return None

        alv = AsyncLazyValue(g, asyncio.Future())
        c = alv.__await__()
        c1 = alv.__await__()
        c.send(None)
        with self.assertRaises(StopIteration) as ctx:
            c.throw(Exc)
        self.assertEqual(ctx.exception.value, 10)

        with self.assertRaises(StopIteration) as ctx:
            next(c1)
        self.assertEqual(ctx.exception.value, 10)

    @async_test
    async def test_throw_unhandled_in_subgen(self) -> None:
        class Exc(Exception):
            pass

        async def g(fut: "asyncio.Future[Any]") -> None:
            try:
                await fut
            except Exc:
                raise IndexError

        alv = AsyncLazyValue(g, asyncio.Future())
        c = alv.__await__()
        c1 = alv.__await__()
        c.send(None)
        with self.assertRaises(IndexError) as ctx:
            c.throw(Exc)
        self.assertIs(type(ctx.exception.__context__), Exc)

        with self.assertRaises(IndexError) as ctx:
            next(c1)
        self.assertIs(type(ctx.exception.__context__), Exc)

    def test_coro_target_is_bound_method(self) -> None:
        class X:
            def __init__(self) -> None:
                self.a = 1

            async def m(self, b: int, c: int, d: int) -> tuple[int, int, int, int]:
                return self.a, b, c, d

        with self.assertRaises(StopIteration) as ctx:
            AsyncLazyValue(X().m, 2, 3, 4).__await__().send(None)

        self.assertEqual(ctx.exception.value, (1, 2, 3, 4))

    def assert_iteration_raises_type_error(self, alv: AsyncLazyValue) -> None:
        with self.assertRaisesRegex(TypeError, "needs to be awaited"):
            next(alv)

        with self.assertRaisesRegex(TypeError, "needs to be awaited"):
            for _ in alv:
                self.fail("iteration should not produce a value")

        def delegate() -> Generator[Any, None, Any]:
            return (yield from alv)

        with self.assertRaisesRegex(TypeError, "needs to be awaited"):
            next(delegate())

        py_iter_send = ctypes.PYFUNCTYPE(
            ctypes.c_int,
            ctypes.py_object,
            ctypes.py_object,
            ctypes.POINTER(ctypes.c_void_p),
        )(("PyIter_Send", ctypes.pythonapi))
        result = ctypes.c_void_p(1)
        with self.assertRaisesRegex(TypeError, "needs to be awaited"):
            py_iter_send(alv, None, ctypes.byref(result))
        self.assertIsNone(result.value)

    def test_iterate_not_started(self) -> None:
        async def g() -> int:
            return 1

        alv = AsyncLazyValue(g)
        # pyrefly: ignore [missing-attribute]
        self.assertEqual(alv.alv_state, "not_started")
        self.assert_iteration_raises_type_error(alv)

    def test_iterate_running(self) -> None:
        async def g(fut: "asyncio.Future[Any]") -> None:
            await fut

        alv = AsyncLazyValue(g, asyncio.Future())
        compute = alv.__await__()
        self.addCleanup(compute.close)
        compute.send(None)
        # pyrefly: ignore [missing-attribute]
        self.assertEqual(alv.alv_state, "running")
        self.assert_iteration_raises_type_error(alv)


class AsyncLazyValueTest(unittest.TestCase):
    def setUp(self) -> None:
        self.events: list[str] = []
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        self.cancelled = asyncio.Event()
        self.coro_running = asyncio.Event()
        self.loop = loop

    def tearDown(self) -> None:
        self.loop.close()
        asyncio.set_event_loop(None)

    def log(self, msg: str) -> None:
        self.events.append(msg)

    @async_test
    async def test_ok_path(self) -> None:
        async def async_func(arg1: int, arg2: int) -> int:
            return arg1 + arg2

        alv = AsyncLazyValue(async_func, 1000, 2000)
        self.assertEqual(await alv, 3000)

    def test_ensure_future_with_loop(self) -> None:
        async def async_func(arg1: int, arg2: int) -> int:
            return arg1 + arg2

        self.loop.set_debug(True)
        self.addCleanup(self.loop.set_debug, False)
        alv = AsyncLazyValue(async_func, 1, 2)
        # pyrefly: ignore [missing-attribute]
        task = alv.ensure_future(self.loop)
        self.assertIsInstance(task, asyncio.Task)
        self.assertEqual(self.loop.run_until_complete(task), 3)

    def test_ensure_future_without_asyncio(self) -> None:
        # A fresh process has not imported `_asyncio` yet, so making that
        # import fail leaves ensure_future() without an event loop getter,
        # a future type or a method table. Each must raise, and must not
        # leave a broken method table behind for when the import works.
        source = """
            import sys
            from _cinderx import AsyncLazyValue

            async def compute():
                return 1

            class Pending:
                def __await__(self):
                    yield

            async def blocked():
                await Pending()

            class Task:
                @property
                def _source_traceback(self):
                    print("traceback read")

            class Loop:
                def create_task(self, coro):
                    coro.close()
                    return Task()

            running = AsyncLazyValue(blocked)
            running.__await__().send(None)
            sys.modules["_asyncio"] = None
            for alv, loop in (
                (AsyncLazyValue(compute), None),
                (running, object()),
                (AsyncLazyValue(compute), Loop()),
            ):
                try:
                    alv.ensure_future(loop)
                except ImportError:
                    print("ImportError")

            del sys.modules["_asyncio"]
            AsyncLazyValue(compute).ensure_future(Loop())
        """
        result = subprocess.run(
            [sys.executable, "-c", dedent(source)],
            capture_output=True,
            encoding=ENCODING,
            # The whole environment, because Windows cannot initialize
            # sockets for asyncio without SYSTEMROOT.
            env={**os.environ, **subprocess_env()},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.splitlines(), ["ImportError"] * 3 + ["traceback read"]
        )

    def test_ensure_future_with_broken_asyncio_future(self) -> None:
        # `_asyncio.Future` that is not a type, then one without the
        # `_source_traceback` getset: each must raise and leave no method
        # table behind, so the task type reads its traceback as an attribute
        # once the getset is there.
        source = """
            import sys
            import types
            import _asyncio
            from _cinderx import AsyncLazyValue

            async def compute():
                return 1

            class Task:
                @property
                def _source_traceback(self):
                    print("traceback read")

            class Loop:
                def create_task(self, coro):
                    coro.close()
                    return Task()

            class Future:
                pass

            fake = types.ModuleType("_asyncio")
            sys.modules["_asyncio"] = fake
            for future in (42, Future):
                fake.Future = future
                try:
                    AsyncLazyValue(compute).ensure_future(Loop())
                except (AttributeError, TypeError) as e:
                    print(type(e).__name__)

            getset = _asyncio.Future.__dict__["_source_traceback"]
            Future._source_traceback = getset
            AsyncLazyValue(compute).ensure_future(Loop())
        """
        result = subprocess.run(
            [sys.executable, "-c", dedent(source)],
            capture_output=True,
            encoding=ENCODING,
            env={**os.environ, **subprocess_env()},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.splitlines(),
            ["TypeError", "AttributeError", "traceback read"],
        )

    def test_ensure_future_reuses_task_type(self) -> None:
        async def async_func(x: int) -> int:
            return x

        for i in range(2):
            # pyrefly: ignore [missing-attribute]
            task = AsyncLazyValue(async_func, i).ensure_future(self.loop)
            self.assertEqual(self.loop.run_until_complete(task), i)

    def test_ensure_future_task_type_can_be_collected(self) -> None:
        async def async_func(x: int) -> int:
            return x

        def run_with_custom_task_type() -> "weakref.ref[type[asyncio.Task]]":
            class CustomTask(asyncio.Task):
                pass

            self.loop.set_task_factory(functools.partial(make_task, CustomTask))
            # pyrefly: ignore [missing-attribute]
            task = AsyncLazyValue(async_func, 1).ensure_future(self.loop)
            self.assertIs(type(task), CustomTask)
            self.assertEqual(self.loop.run_until_complete(task), 1)
            self.loop.set_task_factory(None)
            return weakref.ref(CustomTask)

        custom_task_type = run_with_custom_task_type()
        # A different task type replaces CustomTask as the last one used.
        # pyrefly: ignore [missing-attribute]
        task = AsyncLazyValue(async_func, 2).ensure_future(self.loop)
        self.assertEqual(self.loop.run_until_complete(task), 2)
        gc.collect()
        self.assertIsNone(custom_task_type())

    def test_ensure_future_task_type_address_reuse(self) -> None:
        async def async_func(x: int) -> int:
            return x

        traceback_reads = []

        class FutureLike:
            @property
            def _source_traceback(self) -> None:
                traceback_reads.append(self)

        def make_future_like(
            task_type: type[FutureLike],
            loop: asyncio.AbstractEventLoop,
            coro: Any,
            **kwargs: Any,
        ) -> Any:
            coro.close()
            return task_type()

        # Alternates asyncio.Task subclasses, whose method table reads the
        # traceback straight from the Future, with future-like classes, whose
        # table looks it up as an attribute. Each round's type dies once
        # collected and the next type usually reuses its address, which must
        # not find the dead type's table.
        for i in range(20):
            if i % 2 == 0:
                task_type = type("T", (asyncio.Task,), {})
                factory = functools.partial(make_task, task_type)
            else:
                task_type = type("F", (FutureLike,), {})
                factory = functools.partial(make_future_like, task_type)
            self.loop.set_task_factory(factory)
            # pyrefly: ignore [missing-attribute]
            task = AsyncLazyValue(async_func, i).ensure_future(self.loop)
            self.assertIs(type(task), task_type)
            if i % 2 == 0:
                self.assertEqual(self.loop.run_until_complete(task), i)
            self.loop.set_task_factory(None)
            del task_type, factory, task
            gc.collect()
        self.assertEqual(len(traceback_reads), 10)

    def test_ensure_future_survives_method_table_callback_call(self) -> None:
        async def async_func(x: int) -> int:
            return x

        task_type = type("CallbackTask", (asyncio.Task,), {})
        self.loop.set_task_factory(functools.partial(make_task, task_type))
        # pyrefly: ignore [missing-attribute]
        task = AsyncLazyValue(async_func, 1).ensure_future(self.loop)
        self.assertEqual(self.loop.run_until_complete(task), 1)

        # The method table's weakref callback is reachable from Python, and
        # calling it drops the table's own reference while the type is alive.
        tables = [w for w in weakref.getweakrefs(task_type) if w.__callback__]
        self.assertEqual(len(tables), 1)
        tables[0].__callback__(tables[0])
        del tables

        # pyrefly: ignore [missing-attribute]
        task = AsyncLazyValue(async_func, 2).ensure_future(self.loop)
        self.assertEqual(self.loop.run_until_complete(task), 2)

        self.loop.set_task_factory(None)
        # pyrefly: ignore [missing-attribute]
        task = AsyncLazyValue(async_func, 3).ensure_future(self.loop)
        self.assertEqual(self.loop.run_until_complete(task), 3)
        del task_type, task
        gc.collect()

    @async_test
    async def test_two_tasks(self) -> None:
        call_count = 0

        async def async_func(arg1: int, arg2: int) -> int:
            nonlocal call_count
            call_count += 1
            return arg1 + arg2

        alv = AsyncLazyValue(async_func, 1, 2)
        ta = asyncio.ensure_future(alv)
        tb = asyncio.ensure_future(alv)

        res = await asyncio.gather(ta, tb)
        self.assertEqual(res, [3, 3])

        # should only be called once
        self.assertEqual(call_count, 1)

    @async_test
    async def test_single_task_cancelled(self) -> None:
        """
        Should raise CancelledError()
        """

        async def async_func(arg1: int, arg2: int) -> int:
            self.log("ran-coro")
            self.coro_running.set()
            await asyncio.sleep(3)
            raise RuntimeError("async_func never got cancelled")

        async def async_cancel(task: asyncio.Task, alv: AsyncLazyValue) -> None:
            await self.coro_running.wait()
            self.log("cancelling")
            task.cancel()
            self.log("cancelled")

        alv = AsyncLazyValue(async_func, 1, 2)
        ta = asyncio.ensure_future(alv)
        tc = asyncio.ensure_future(async_cancel(ta, alv))

        ta_result, tc_result = await asyncio.gather(ta, tc, return_exceptions=True)
        self.assertSequenceEqual(self.events, ["ran-coro", "cancelling", "cancelled"])
        self.assertTrue(isinstance(ta_result, asyncio.CancelledError))
        self.assertEqual(tc_result, None)

    @async_test
    async def test_two_tasks_parent_cancelled(self) -> None:
        """
        Creates two tasks from the same AsyncLazyValue. Cancels the task which
        calls the coroutine first.
        """

        async def async_func(arg1: int, arg2: int) -> int:
            self.log("ran-coro")
            await asyncio.sleep(3)
            self.log("completed-coro")
            raise RuntimeError("async_func never got cancelled")

        async def async_cancel(task: asyncio.Task, alv: AsyncLazyValue) -> None:
            start = time()
            # pyrefly: ignore [missing-attribute]
            while alv._awaiting_tasks < 1:
                # Sleep until both tasks are awaiting on the future (one of them
                # is awaiting on async_func, so we only wait until the count
                # is 1). Also, we timeout if we wait more than 3 seconds
                now = time()
                if now - start > 3:
                    raise RuntimeError(
                        "cannot cancel since the tasks are not waiting on the future"
                    )
                await asyncio.sleep(0)

            self.log("cancelling")
            task.cancel()
            self.log("cancelled")

        alv = AsyncLazyValue(async_func, 1, 2)
        ta = asyncio.ensure_future(alv)
        tb = asyncio.ensure_future(alv)
        tc = asyncio.ensure_future(async_cancel(ta, alv))

        ta_result, tb_result, tc_result = await asyncio.gather(
            ta, tb, tc, return_exceptions=True
        )

        self.assertSequenceEqual(self.events, ["ran-coro", "cancelling", "cancelled"])
        self.assertTrue(isinstance(ta_result, asyncio.CancelledError))
        self.assertTrue(isinstance(tb_result, asyncio.CancelledError))
        self.assertEqual(tc_result, None)

    @async_test
    async def test_two_tasks_child_cancelled(self) -> None:
        """
        Creates two tasks from the same AsyncLazyValue. Cancels the task which
        calls the coroutine second.
        """

        async def async_func(arg1: int, arg2: int) -> int:
            self.log("ran-coro")
            await self.cancelled.wait()
            return arg1 + arg2

        async def async_cancel(task: asyncio.Task, alv: AsyncLazyValue) -> None:
            start = time()
            # pyrefly: ignore [missing-attribute]
            while alv._awaiting_tasks < 1:
                # Sleep until both tasks are awaiting on the future (one of them
                # is awaiting on async_func, so we only wait until the count
                # is 1). Also, we timeout if we wait more than 3 seconds
                now = time()
                if now - start > 3:
                    raise RuntimeError(
                        "cannot cancel since the tasks are not waiting on the future"
                    )
                await asyncio.sleep(0)
            self.log("cancelling")
            task.cancel()
            self.cancelled.set()
            self.log("cancelled")

        alv = AsyncLazyValue(async_func, 1, 2)
        ta = asyncio.ensure_future(alv)
        tb = asyncio.ensure_future(alv)
        tc = asyncio.ensure_future(async_cancel(tb, alv))

        ta_result, tb_result, tc_result = await asyncio.gather(
            ta, tb, tc, return_exceptions=True
        )

        self.assertSequenceEqual(self.events, ["ran-coro", "cancelling", "cancelled"])
        self.assertEqual(ta_result, 3)
        self.assertTrue(isinstance(tb_result, asyncio.CancelledError))
        self.assertEqual(tc_result, None)

    @async_test
    async def test_throw_1(self) -> None:
        async def l0(alv: Any) -> Any:
            return await l1(alv)

        async def l1(alv: Any) -> Any:
            return await l2(alv)

        async def l2(alv: Any) -> Any:
            return await alv

        async def val(f: "asyncio.Future[Any]") -> int:
            try:
                await f
            except:  # noqa: B001
                pass
            return 42

        f = asyncio.Future()
        alv = AsyncLazyValue(val, f)

        loop = asyncio.get_running_loop()
        loop.call_later(1, lambda: f.set_exception(NotImplementedError))
        x = await l0(alv)
        self.assertEqual(x, 42)


if __name__ == "__main__":
    unittest.main()

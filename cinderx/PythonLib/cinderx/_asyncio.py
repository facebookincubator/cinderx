# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from __future__ import annotations

from asyncio import AbstractEventLoop, CancelledError, Future, get_event_loop
from collections.abc import Awaitable, Callable, Generator
from typing import NoReturn, Optional, TypeVar


try:
    # pyre-ignore[21]: Unknwn import
    from _cinderx import AsyncLazyValue, AwaitableValue
except ImportError:

    class _AsyncLazyValueState:
        NotStarted = 0
        Running = 1
        Done = 2

    _TParams = TypeVar("_TParams")
    _T = TypeVar("_T", covariant=True)

    class AsyncLazyValue(Awaitable[_T]):
        """
        This is a low-level class used mainly for two things:
        * It helps to avoid calling a coroutine multiple times, by caching the
        result of a previous call
        * It ensures that the coroutine is called only once

        AsyncLazyValue has well defined cancellation behavior in these cases:

        1. When we have a single task stack (call stack for you JS folks), which is
        awaiting on the AsyncLazyValue
        -> In this case, we mimic the behavior of a normal await. i.e: If the
            task stack gets cancelled, we cancel the coroutine (by raising a
            CancelledError in the underlying future)

        2. When we have multiple task stacks awaiting on the future.
        We have two sub cases here.

        2.1. The initial task stack (which resulted in an await of the coroutine)
                gets cancelled.
                -> In this case, we cancel the coroutine, and all the tasks depending
                on it. If we don't do that, we'd have to implement retry logic,
                which is a bad idea in such low level code. Even if we do implement
                retries, there's no guarantee that they would succeed, so it's better
                to just fail here.

                Also, the number of times this happens is very small (I don't have
                data to prove it, but qualitative arguments suggest this is the
                case).

        2.2. One of the many task stacks gets cancelled (but not the one which ended
            up awaiting the coroutine)
            -> In this case, we just allow the task stack to be cancelled, but
                the rest of them are processed without being affected.
        """

        def __init__(
            self,
            # pyre-fixme[31]: Expression `typing.Callable[(_TParams,
            #  typing.Awaitable[_T])]` is not a valid type.
            coro_func: Callable[_TParams, Awaitable[_T]],
            # pyre-fixme[11]: Annotation `args` is not defined as a type.
            *args: _TParams.args,
            # pyre-fixme[11]: Annotation `kwargs` is not defined as a type.
            **kwargs: _TParams.kwargs,
        ) -> None:
            # pyre-fixme[31]: Expression `typing.Optional[typing.Callable[(_TParams,
            #  typing.Awaitable[_T])]]` is not a valid type.
            self.coro_func: Optional[Callable[_TParams, Awaitable[_T]]] = coro_func
            self.args: tuple[object, ...] = args
            self.kwargs: dict[str, object] = kwargs
            self.state: int = _AsyncLazyValueState.NotStarted
            self.res: Optional[_T] = None
            self._futures: list[Future] = []
            self._awaiting_tasks = 0

        async def _async_compute(self) -> _T:
            futures = self._futures
            try:
                coro_func = self.coro_func
                # lint-fixme: NoAssertsRule
                assert coro_func is not None
                self.res = res = await coro_func(*self.args, **self.kwargs)

                self.state = _AsyncLazyValueState.Done

                # pyre-ignore[1001]: Pyre is worried about how we're accessing futures
                # without awaiting them.
                for value in futures:
                    if not value.done():
                        value.set_result(self.res)

                self.args = ()
                self.kwargs.clear()
                del self._futures[:]
                self.coro_func = None

                return res

            except (Exception, CancelledError) as e:
                # pyre-ignore[1001]: Pyre is worried about how we're accessing futures
                # without awaiting them.
                for value in futures:
                    if not value.done():
                        value.set_exception(e)
                self._futures = []
                self.state = _AsyncLazyValueState.NotStarted
                raise

        def _get_future(self, loop: Optional[AbstractEventLoop]) -> Future:
            if loop is None:
                loop = get_event_loop()
            f = Future(loop=loop)
            self._futures.append(f)
            self._awaiting_tasks += 1
            return f

        def __iter__(self) -> AsyncLazyValue[_T]:
            return self

        def __next__(self) -> NoReturn:
            raise StopIteration(self.res)

        def __await__(self) -> Generator[None, None, _T]:
            if self.state == _AsyncLazyValueState.Done:
                # pyre-ignore[7]: Expected `Generator[None, None, Variable[_T](covariant)]`
                # but got `_AsyncLazyValue[Variable[_T](covariant)]`.
                return self
            elif self.state == _AsyncLazyValueState.Running:
                c = self._get_future(None)
                return c.__await__()
            else:
                self.state = _AsyncLazyValueState.Running
                c = self._async_compute()
                return c.__await__()

        def as_future(self, loop: AbstractEventLoop) -> Future:
            if self.state == _AsyncLazyValueState.Done:
                f = Future(loop=loop)
                f.set_result(self.res)
                return f
            elif self.state == _AsyncLazyValueState.Running:
                return self._get_future(loop)
            else:
                if loop is None:
                    loop = get_event_loop()
                t = loop.create_task(self._async_compute())
                self.state = _AsyncLazyValueState.Running
                # pyre-ignore[16]: Undefined attribute `asyncio.tasks.Task`
                # has no attribute `_source_traceback`.
                if t._source_traceback:
                    del t._source_traceback[-1]
                return t

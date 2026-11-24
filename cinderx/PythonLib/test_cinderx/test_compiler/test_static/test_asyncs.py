# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __static__ import ContextDecorator, make_context_decorator_wrapper

from collections.abc import Callable, Generator
from typing import Any, cast

from .common import StaticTestBase

# A static `async def` overridden in a non-static subclass by a function that
# returns an arbitrary awaitable: static callers get it wrapped in an
# `awaitable_wrapper` that owns the awaitable's iterator.
OVERRIDDEN_ASYNC = """
class C:
    async def m(self) -> int:
        return 1

def call(c: C):
    return c.m()
"""


class AsyncTests(StaticTestBase):
    def test_async_for(self) -> None:
        codestr = """
        from typing import Awaitable, List
        async def foo(awaitables) -> int:
             sum = 0
             async for x in awaitables:
                 sum += x
             return sum
        """
        # Ensure that this compiles without hassle - we don't do any type analysis.
        self.compile(codestr)

    def test_async_for_name_error(self) -> None:
        codestr = """
        from typing import Awaitable, List
        async def foo(awaitables) -> int:
             sum = 0
             async for x in awaitables:
                 sum += y
             return sum
        """
        self.type_error(codestr, "Name `y` is not defined.")

    def test_async_for_primitive_error(self) -> None:
        codestr = """
        from __static__ import int64
        from typing import Awaitable, List
        async def foo() -> int:
             awaitables: int64 = 1
             async for x in awaitables:
                 sum += x
             return sum

        async def asyncify(x):
             return x
        """
        self.type_error(codestr, "cannot await a primitive value")

    def test_async_with(self) -> None:
        codestr = """
        from typing import Awaitable, List
        async def foo(acm) -> None:
             async with acm() as c:
                 c.m()
        """
        # Ensure that this compiles without hassle - we don't do any type analysis.
        self.compile(codestr)

    def test_async_with_name_error(self) -> None:
        codestr = """
        from typing import Awaitable, List
        async def foo(acm) -> int:
             async with acm() as c:
                 d.m()
        """
        self.type_error(codestr, "Name `d` is not defined.")

    def test_async_with_may_not_terminate(self) -> None:
        codestr = """
        from typing import Awaitable, List
        async def foo(acm) -> int:
             async with acm() as c:
                 return 42
        """
        self.type_error(
            codestr,
            "Function has declared return type 'int' but can implicitly return None.",
        )

    def drive_reentrant_wrapper(self, reenter: str) -> tuple[object, list[str]]:
        """Run a wrapped awaitable whose iterator re-enters its own wrapper
        (`wrapper.__await__()` or `wrapper.close()`) while the wrapper is
        driving it, which drops the wrapper's reference to the running
        iterator."""
        events: list[str] = []
        wrappers: list[object] = []

        def body() -> Generator[None, None, int]:
            yield None
            getattr(wrappers[0], reenter)()
            events.append("reentered")
            yield None
            return 1

        class Awaitable:
            def __await__(self) -> Generator[None, None, int]:
                return body()

        with self.in_module(OVERRIDDEN_ASYNC) as mod:

            class D(mod.C):
                def m(self) -> Awaitable:
                    return Awaitable()

            wrapper = mod.call(D())
            self.assertEqual(type(wrapper).__name__, "awaitable_wrapper")
            wrappers.append(wrapper)
            self.assertIsNone(wrapper.send(None))
            try:
                result = wrapper.send(None)
            except ValueError as e:
                result = e
            wrappers.clear()
            return result, events

    def test_awaitable_wrapper_await_reentered_during_send(self) -> None:
        self.assertEqual(
            self.drive_reentrant_wrapper("__await__"), (None, ["reentered"])
        )

    def test_awaitable_wrapper_close_reentered_during_send(self) -> None:
        result, events = self.drive_reentrant_wrapper("close")
        # Closing the running iterator fails inside it; the wrapper returns
        # that error instead of freeing the iterator.
        self.assertIsInstance(result, ValueError)
        self.assertEqual(str(result), "generator already executing")
        self.assertEqual(events, [])

    def close_reentrant_wrapper(self, reenter: str) -> list[str]:
        """Close a wrapped awaitable whose generator re-enters its own wrapper
        (`wrapper.__await__()` or `wrapper.close()`) from its `finally` while
        the wrapper is closing it."""
        events: list[str] = []
        wrappers: list[object] = []

        def body() -> Generator[None, None, int]:
            try:
                yield None
                yield None
            finally:
                try:
                    getattr(wrappers[0], reenter)()
                except ValueError as e:
                    events.append(str(e))
                events.append("finally")
            return 1

        class Awaitable:
            def __await__(self) -> Generator[None, None, int]:
                return body()

        with self.in_module(OVERRIDDEN_ASYNC) as mod:

            class D(mod.C):
                def m(self) -> Awaitable:
                    return Awaitable()

            wrapper = mod.call(D())
            wrappers.append(wrapper)
            self.assertIsNone(wrapper.send(None))
            self.assertIsNone(wrapper.close())
            wrappers.clear()
        return events

    def test_awaitable_wrapper_await_reentered_during_close(self) -> None:
        self.assertEqual(self.close_reentrant_wrapper("__await__"), ["finally"])

    def test_awaitable_wrapper_close_reentered_during_close(self) -> None:
        self.assertEqual(
            self.close_reentrant_wrapper("close"),
            ["generator already executing", "finally"],
        )

    def test_awaitable_wrapper_await_reentered_during_onsend(self) -> None:
        # The context decorator's first-send hook runs `__enter__` before the
        # wrapper sends to the iterator; `__enter__` re-enters the wrapper.
        wrappers: list[Any] = []

        class C(ContextDecorator):
            def __enter__(self) -> "C":
                wrappers[0].__await__()
                return self

        async def wrapper() -> None:
            pass

        def gen() -> Generator[str, None, int]:
            yield "y1"
            return 3

        class Awaitable:
            def __await__(self) -> Generator[str, None, int]:
                return gen()

        def wrapped() -> Awaitable:
            return Awaitable()

        f = cast(
            Callable[[], Any], make_context_decorator_wrapper(C(), wrapper, wrapped)
        )
        aw = f()
        wrappers.append(aw)
        self.assertEqual(aw.send(None), "y1")
        wrappers.clear()

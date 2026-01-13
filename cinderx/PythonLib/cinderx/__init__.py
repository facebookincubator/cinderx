# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""High-performance Python runtime extensions."""

from __future__ import annotations

import gc
import platform
import sys
from os import environ

# ============================================================================
# Note!
#
# At Meta, this module is currently loaded as part of Lib/site.py in an attempt
# to get benefits from using it as soon as possible.  However
# Lib/test/test_site.py will assert that site.py does not import too many
# modules.  Be careful with adding import statements here.
#
# The plan is to move applications over to using an explicit initialization
# step rather than Lib/site.py.  Once that is done we can add all the imports
# we want here.
# ============================================================================


_import_error: ImportError | None = None


def is_supported_runtime() -> bool:
    """
    Check that the current Python runtime will be able to load the _cinderx
    native extension.
    """

    if sys.platform not in ("darwin", "linux"):
        return False

    version = (sys.version_info.major, sys.version_info.minor)
    if version == (3, 14) or version == (3, 15):
        # Can't load the native extension if the GIL is forcibly being disabled.  The
        # native extension doesn't support free-threading properly yet.
        return environ.get("PYTHON_GIL") != "0"
    if version == (3, 12):
        return "+meta" in sys.version
    if version == (3, 10):
        return "+cinder" in sys.version
    return False


try:
    # Currently if we try to import _cinderx on runtimes without our internal patches
    # the import will crash.  This is meant to go away in the future.
    if not is_supported_runtime():
        raise ImportError(
            f"The _cinderx native extension is not supported for Python version '{sys.version}' on platform '{sys.platform}'"
        )

    # pyre-ignore[21]: _cinderx is not a real cpp_python_extension() yet.
    from _cinderx import (  # noqa: F401
        _compile_perf_trampoline_pre_fork,
        _is_compile_perf_trampoline_pre_fork_enabled,
        async_cached_classproperty,
        async_cached_property,
        cached_classproperty,
        cached_property,
        cached_property_with_descr,
        clear_caches,
        clear_classloader_caches,
        disable_parallel_gc,
        enable_parallel_gc,
        freeze_type,
        get_parallel_gc_settings,
        has_parallel_gc,
        immortalize_heap,
        is_immortal,
        strict_module_patch,
        strict_module_patch_delete,
        strict_module_patch_enabled,
        StrictModule,
        watch_sys_modules,
    )

    if sys.version_info < (3, 11):
        # In 3.12+ use the versions in the polyfill cinder library instead.
        from _cinderx import (
            _get_entire_call_stack_as_qualnames_with_lineno,
            _get_entire_call_stack_as_qualnames_with_lineno_and_frame,
            clear_all_shadow_caches,
        )

    if sys.version_info >= (3, 12):
        from _cinderx import delay_adaptive, get_adaptive_delay, set_adaptive_delay

except ImportError as e:
    if "undefined symbol:" in str(e):
        # If we're on a dev build report this as an error, otherwise muddle along with alternative definitions
        # on unsupported Python's.
        from os.path import dirname, exists, join

        if exists(join(dirname(__file__), ".dev_build")):
            raise ImportError(
                "The _cinderx native extension is not available due to a missing symbol. This is likely a bug you introduced.  "
                "Please ensure that the cinderx kernel is being used."
            ) from e
    _import_error = e

    def _compile_perf_trampoline_pre_fork() -> None:
        pass

    def _get_entire_call_stack_as_qualnames_with_lineno() -> list[tuple[str, int]]:
        return []

    def _get_entire_call_stack_as_qualnames_with_lineno_and_frame() -> list[
        tuple[str, int, object]
    ]:
        return []

    def _is_compile_perf_trampoline_pre_fork_enabled() -> bool:
        return False

    from asyncio import AbstractEventLoop, Future
    from typing import (
        Awaitable,
        Callable,
        Dict,
        final,
        Generator,
        Generic,
        List,
        NoReturn,
        Optional,
        overload,
        Tuple,
        Type,
        TYPE_CHECKING,
        TypeVar,
    )

    _TClass = TypeVar("_TClass")
    _TReturnType = TypeVar("_TReturnType")

    @final
    class NoValueSet:
        pass

    NO_VALUE_SET = NoValueSet()

    class _BaseCachedProperty(Generic[_TClass, _TReturnType]):
        fget: Callable[[_TClass], _TReturnType]
        __name__: str

        def __init__(
            self,
            f: Callable[[_TClass], _TReturnType],
            slot: Optional[Descriptor[_TReturnType]] = None,
        ) -> None:
            self.fget: Callable[[_TClass], _TReturnType] = f
            self.__name__ = f.__name__
            self.__doc__: str | None = f.__doc__
            self.slot = slot

        @overload
        def __get__(
            self, obj: None, cls: Type[_TClass]
        ) -> _BaseCachedProperty[_TClass, _TReturnType]: ...

        @overload
        def __get__(self, obj: _TClass, cls: Type[_TClass]) -> _TReturnType: ...

        def __get__(
            self, obj: Optional[_TClass], cls: Type[_TClass]
        ) -> _BaseCachedProperty[_TClass, _TReturnType] | _TReturnType:
            if obj is None:
                return self
            slot = self.slot
            if slot is not None:
                try:
                    res = slot.__get__(obj, cls)
                except AttributeError:
                    res = self.fget(obj)
                    slot.__set__(obj, res)
                return res

            result = self.fget(obj)
            obj.__dict__[self.__name__] = result
            return result

    class _AsyncLazyValueState:
        NotStarted = 0
        Running = 1
        Done = 2

    _T = TypeVar("_T", covariant=True)
    _TParams = TypeVar("_TParams")

    # noqa: F401
    import asyncio

    class _AsyncLazyValue(Awaitable[_T]):
        """
        This is a low-level class used mainly for two things:
        * It helps to avoid calling a coroutine multiple times, by caching the
        result of a previous call
        * It ensures that the coroutine is called only once

        _AsyncLazyValue has well defined cancellation behavior in these cases:

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
            global asyncio
            # pyre-fixme[31]: Expression `typing.Optional[typing.Callable[(_TParams,
            #  typing.Awaitable[_T])]]` is not a valid type.
            self.coro_func: Optional[Callable[_TParams, Awaitable[_T]]] = coro_func
            self.args: Tuple[object, ...] = args
            self.kwargs: Dict[str, object] = kwargs
            self.state: int = _AsyncLazyValueState.NotStarted
            self.res: Optional[_T] = None
            self._futures: List[Future] = []
            self._awaiting_tasks = 0

        async def _async_compute(self) -> _T:
            futures = self._futures
            try:
                coro_func = self.coro_func
                # lint-fixme: NoAssertsRule
                assert coro_func is not None
                self.res = res = await coro_func(*self.args, **self.kwargs)

                self.state = _AsyncLazyValueState.Done

                # pyre-fixme[1001]: Awaitable assigned to `value` is never awaited.
                for value in futures:
                    if not value.done():
                        value.set_result(self.res)

                self.args = ()
                self.kwargs.clear()
                del self._futures[:]
                self.coro_func = None

                return res

            except (Exception, asyncio.CancelledError) as e:
                # pyre-fixme[1001]: Awaitable assigned to `value` is never awaited.
                for value in futures:
                    if not value.done():
                        value.set_exception(e)
                self._futures = []
                self.state = _AsyncLazyValueState.NotStarted
                raise

        def _get_future(self, loop: Optional[AbstractEventLoop]) -> Future:
            if loop is None:
                loop = asyncio.get_event_loop()
            f = asyncio.Future(loop=loop)
            self._futures.append(f)
            self._awaiting_tasks += 1
            return f

        def __iter__(self) -> _AsyncLazyValue[_T]:
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
                f = asyncio.Future(loop=loop)
                f.set_result(self.res)
                return f
            elif self.state == _AsyncLazyValueState.Running:
                return self._get_future(loop)
            else:
                if loop is None:
                    loop = asyncio.get_event_loop()
                t = loop.create_task(self._async_compute())
                self.state = _AsyncLazyValueState.Running
                # pyre-ignore[16]: Undefined attribute `asyncio.tasks.Task`
                # has no attribute `_source_traceback`.
                if t._source_traceback:
                    del t._source_traceback[-1]
                # pyre-fixme[7]: Expected `Future[Any]` but got `Task[_T]`.
                return t

    _TAwaitableReturnType = TypeVar("_TAwaitableReturnType")

    class async_cached_property(
        Generic[_TAwaitableReturnType, _TClass],
        _BaseCachedProperty[_TClass, Awaitable[_TAwaitableReturnType]],
    ):
        def __init__(
            self,
            f: Callable[[_TClass], _TReturnType],
            slot: Optional[Descriptor[_TReturnType]] = None,
        ) -> None:
            super().__init__(f, slot)

        def __get__(
            self, obj: Optional[_TClass], cls: Type[_TClass]
        ) -> (
            _BaseCachedProperty[_TClass, Awaitable[_TAwaitableReturnType]]
            | Awaitable[_TAwaitableReturnType]
        ):
            if obj is None:
                return self

            slot = self.slot
            if slot is not None:
                try:
                    res = slot.__get__(obj, cls)
                except AttributeError:
                    res = _AsyncLazyValue(self.fget, obj)
                    slot.__set__(obj, res)
                return res

            lazy_value = _AsyncLazyValue(self.fget, obj)
            setattr(obj, self.__name__, lazy_value)
            return lazy_value

    class async_cached_classproperty(
        Generic[_TAwaitableReturnType, _TClass],
        _BaseCachedProperty[Type[_TClass], Awaitable[_TAwaitableReturnType]],
    ):
        def __init__(
            self,
            f: Callable[[_TClass], Awaitable[_TAwaitableReturnType]],
            slot: Optional[Descriptor[Awaitable[_TAwaitableReturnType]]] = None,
        ) -> None:
            super().__init__(f, slot)
            self._value: NoValueSet | Awaitable[_TAwaitableReturnType] = NO_VALUE_SET

        def __get__(
            self, obj: Optional[_TClass], cls: Type[_TClass]
        ) -> Awaitable[_TAwaitableReturnType]:
            lazy_value = self._value
            if not isinstance(lazy_value, NoValueSet):
                return lazy_value
            self._value = lazy_value = _AsyncLazyValue(self.fget, cls)
            return lazy_value

    class cached_classproperty(_BaseCachedProperty[Type[_TClass], _TReturnType]):
        def __init__(
            self,
            f: Callable[[_TClass], _TReturnType],
            slot: Optional[Descriptor[_TReturnType]] = None,
        ) -> None:
            super().__init__(f, slot)
            self._value: NoValueSet | _TReturnType = NO_VALUE_SET

        def __get__(self, obj: Optional[_TClass], cls: Type[_TClass]) -> _TReturnType:
            result = self._value
            if not isinstance(result, NoValueSet):
                return result
            self._value = result = self.fget(cls)
            return result

    _TClass = TypeVar("_TClass")
    _TReturnType = TypeVar("_TReturnType")

    if TYPE_CHECKING:
        from abc import ABC

        @final
        class Descriptor(ABC, Generic[_TReturnType]):
            __name__: str
            __objclass__: Type[object]

            def __get__(
                self, inst: object, ctx: Optional[Type[object]] = None
            ) -> _TReturnType: ...

            def __set__(self, inst: object, value: _TReturnType) -> None:
                pass

            def __delete__(self, inst: object) -> None:
                pass

    class cached_property(_BaseCachedProperty[_TClass, _TReturnType]):
        def __init__(
            self,
            f: Callable[[_TClass], _TReturnType],
            slot: Optional[Descriptor[_TReturnType]] = None,
        ) -> None:
            super().__init__(f, slot)
            if slot is not None:
                if (
                    type(self) is not cached_property
                    and type(self) is not cached_property_with_descr
                ):
                    raise TypeError(
                        "slot can't be used with subtypes of cached_property"
                    )
                # pyre-ignore[4]: Missing attribute annotation for __class__
                self.__class__ = cached_property_with_descr

    class cached_property_with_descr(cached_property[_TClass, _TReturnType]):
        def __set__(self, inst: object, value: _TReturnType) -> None:
            slot = self.slot
            if slot is not None:
                slot.__set__(inst, value)
            else:
                setattr(inst, self.__name__, value)

        def __delete__(self, inst: object) -> None:
            slot = self.slot
            if slot is not None:
                slot.__delete__(inst)
            else:
                delattr(inst, self.__name__)

    if sys.version_info < (3, 11):

        def clear_all_shadow_caches() -> None:
            pass

    def clear_caches() -> None:
        pass

    def clear_classloader_caches() -> None:
        pass

    def disable_parallel_gc() -> None:
        pass

    def enable_parallel_gc(min_generation: int = 2, num_threads: int = 0) -> None:
        raise RuntimeError(
            "No Parallel GC support because _cinderx did not load correctly"
        )

    def freeze_type(ty: object) -> object:
        return ty

    def get_parallel_gc_settings() -> dict[str, int] | None:
        return None

    def has_parallel_gc() -> bool:
        return False

    def immortalize_heap() -> None:
        pass

    def is_immortal(obj: object) -> bool:
        raise RuntimeError(
            "Can't answer whether an object is mortal or immortal from Python code"
        )

    def strict_module_patch(mod: object, name: str, value: object) -> None:
        pass

    def strict_module_patch_delete(mod: object, name: str) -> None:
        pass

    def strict_module_patch_enabled(mod: object) -> bool:
        return False

    class StrictModule:
        def __init__(self, d: dict[str, object], b: bool) -> None:
            pass

    def watch_sys_modules() -> None:
        pass


def maybe_enable_parallel_gc() -> None:
    """Conditionally enable parallel GC based on environment variables."""
    is_parallel_gc_enabled = environ.get("PARALLEL_GC_ENABLED", "0") == "1"
    if not has_parallel_gc() or not is_parallel_gc_enabled:
        return
    thresholds = gc.get_threshold()
    parallel_gc_threshold_gen0 = int(
        environ.get("PARALLEL_GC_THRESHOLD_GEN0", thresholds[0])
    )
    parallel_gc_threshold_gen1 = int(
        environ.get("PARALLEL_GC_THRESHOLD_GEN1", thresholds[1])
    )
    parallel_gc_threshold_gen2 = int(
        environ.get("PARALLEL_GC_THRESHOLD_GEN2", thresholds[2])
    )
    gc.set_threshold(
        parallel_gc_threshold_gen0,
        parallel_gc_threshold_gen1,
        parallel_gc_threshold_gen2,
    )

    parallel_gc_num_threads = int(environ.get("PARALLEL_GC_NUM_THREADS", "0"))
    parallel_gc_min_generation = int(environ.get("PARALLEL_GC_MIN_GENERATION", "2"))

    enable_parallel_gc(
        min_generation=parallel_gc_min_generation,
        num_threads=parallel_gc_num_threads,
    )


_is_init: bool = False


def init() -> None:
    """Initialize CinderX."""
    global _is_init

    # Failed to import _cinderx, nothing to initialize.
    if _import_error is not None:
        return

    # Already initialized.
    if _is_init:
        return

    maybe_enable_parallel_gc()

    _is_init = True


def is_initialized() -> bool:
    """
    Check if the cinderx extension has been properly initialized.
    """
    return _is_init


def get_import_error() -> ImportError | None:
    """
    Get the ImportError that occurred when _cinderx was imported, if there was
    an error.
    """
    return _import_error


init()

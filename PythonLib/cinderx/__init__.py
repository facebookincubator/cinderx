# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""High-performance Python runtime extensions."""

from __future__ import annotations

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

import sys

try:
    from os import RTLD_GLOBAL
    from sys import getdlopenflags, setdlopenflags
except ImportError:
    RTLD_GLOBAL: int = 0

    def getdlopenflags() -> int:
        return 0

    def setdlopenflags(flags: int) -> None:
        pass


_import_error: ImportError | None = None


def is_supported_runtime() -> bool:
    """
    Check that the current Python runtime will be able to load the _cinderx
    native extension.
    """

    if sys.platform != "linux":
        return False

    version = (sys.version_info.major, sys.version_info.minor)
    if version == (3, 12):
        return "+meta" in sys.version
    if version == (3, 10):
        return "+cinder" in sys.version
    return False


# We need to make the symbols from _cinderx available process wide as they
# are used in other CinderX modules like _static, etc.
old_dlopen_flags: int = getdlopenflags()
setdlopenflags(old_dlopen_flags | RTLD_GLOBAL)
try:
    # Currently if we try to import _cinderx on runtimes without our internal patches
    # the import will crash.  This is meant to go away in the future.
    if not is_supported_runtime():
        raise ImportError(
            f"The _cinderx native extension is not supported for Python version '{sys.version}' on platform '{sys.platform}'"
        )

    try:
        # pyre-ignore[21]: _cinderx isn't known by Pyre
        from _cinderx import (
            disable_parallel_gc,
            enable_parallel_gc,
            get_parallel_gc_settings,
            has_parallel_gc,
        )
    except ImportError:

        def disable_parallel_gc() -> None:
            pass

        def enable_parallel_gc(min_generation: int = 2, num_threads: int = 0) -> None:
            pass

        def get_parallel_gc_settings() -> dict[str, int]:
            return {}

        def has_parallel_gc() -> bool:
            return False

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
        freeze_type,
        immortalize_heap,
        init as cinderx_init,
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
    _import_error = e

    cinderx_init = None

    def _compile_perf_trampoline_pre_fork() -> None:
        pass

    def _get_entire_call_stack_as_qualnames_with_lineno() -> list[tuple[str, int]]:
        return []

    def _get_entire_call_stack_as_qualnames_with_lineno_and_frame() -> (
        list[tuple[str, int, object]]
    ):
        return []

    def _is_compile_perf_trampoline_pre_fork_enabled() -> bool:
        return False

    class async_cached_classproperty:
        pass

    class async_cached_property:
        pass

    class cached_classproperty:
        pass

    from typing import (
        Callable,
        final,
        Generic,
        Optional,
        overload,
        Type,
        TYPE_CHECKING,
        TypeVar,
    )

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
        pass

    def freeze_type(ty: object) -> object:
        return ty

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

finally:
    setdlopenflags(old_dlopen_flags)


def strictify_static() -> None:
    """Turn _static into a StrictModule so we can do direct invokes against it."""

    # pyre-ignore[21]: _static is magically created by _cinderx.
    import _static

    # if it has a __file__ attribute, libregrtest will try to write to it
    if hasattr(_static, "__file__"):
        del _static.__file__
        # pyre-ignore[6]: Can't type this as ModuleType because this file can't import
        # `types`, see the big comment at the top.
        sys.modules["_static"] = StrictModule(_static.__dict__, False)


_is_init: bool = False


def init() -> None:
    """Initialize CinderX."""
    if cinderx_init is None:
        return

    cinderx_init()
    strictify_static()

    global _is_init
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

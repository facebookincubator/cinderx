# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""High-performance Python runtime extensions."""

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

import os
import sys


_import_error: ImportError | None = None


# We need to make the symbols from _cinderx available process wide as they
# are used in other CinderX modules like _static, etc.
old_dlopen_flags: int = sys.getdlopenflags()
sys.setdlopenflags(old_dlopen_flags | os.RTLD_GLOBAL)
try:
    # pyre-ignore[21]: _cinderx is not a real cpp_python_extension() yet.
    from _cinderx import (  # noqa: F401
        _compile_perf_trampoline_pre_fork,
        _get_entire_call_stack_as_qualnames_with_lineno,
        _get_entire_call_stack_as_qualnames_with_lineno_and_frame,
        _is_compile_perf_trampoline_pre_fork_enabled,
        async_cached_classproperty,
        async_cached_property,
        cached_classproperty,
        cached_property,
        clear_caches,
        clear_classloader_caches,
        disable_parallel_gc,
        enable_parallel_gc,
        freeze_type,
        get_parallel_gc_settings,
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
        # pyre-ignore[21]: _cinderx is not a real cpp_python_extension() yet.
        from _cinderx import clear_all_shadow_caches
    else:

        def clear_all_shadow_caches() -> None:
            pass

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

    class cached_property:
        pass

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

    def get_parallel_gc_settings() -> dict[str, int]:
        return {}

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
    sys.setdlopenflags(old_dlopen_flags)


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

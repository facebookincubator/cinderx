# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
"""High-performance Python runtime extensions."""

import os
import sys

# We need to make the symbols from _cinderx available process wide as they
# are used in other CinderX modules like _static, etc.
old_dlopen_flags: int = sys.getdlopenflags()
sys.setdlopenflags(old_dlopen_flags | os.RTLD_GLOBAL)
try:
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
        get_parallel_gc_settings,
        init as cinderx_init,
        strict_module_patch,
        strict_module_patch_delete,
        strict_module_patch_enabled,
        StrictModule,
        watch_sys_modules,
    )

    if sys.version_info < (3, 11):
        from _cinderx import clear_all_shadow_caches
    else:

        def clear_all_shadow_caches() -> None:
            pass

except ImportError:
    if os.environ.get("CINDERX_ALLOW__CINDERX_FAILURE") is None:
        raise
    cinderx_init = None
finally:
    sys.setdlopenflags(old_dlopen_flags)


def strictify_static() -> None:
    """Turn _static into a StrictModule so we can do direct invokes against it."""

    import _static

    # if it has a __file__ attribute, libregrtest will try to write to it
    if hasattr(_static, "__file__"):
        del _static.__file__
        sys.modules["_static"] = StrictModule(_static.__dict__, False)


def init() -> None:
    """Initialize CinderX."""
    if cinderx_init is None:
        return

    cinderx_init()

    strictify_static()

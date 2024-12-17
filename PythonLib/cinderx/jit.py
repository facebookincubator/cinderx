# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# pyre-strict

from types import FunctionType
from warnings import warn


INSTALLED: bool = False


try:
    from cinderjit import (
        disable,
        enable,
        force_compile,
        is_enabled,
        is_jit_compiled,
        jit_suppress,
    )

    INSTALLED = True

except ImportError:

    def disable(compile_all: bool = True, deopt_all: bool = False) -> None:
        pass

    def enable() -> None:
        # Warn here because users might think this is function is how to enable the JIT
        # when it is not installed.
        warn(
            "Cinder JIT is not installed, calling cinderx.jit.enable() is doing nothing"
        )

    def force_compile(func: FunctionType) -> bool:
        return False

    def is_enabled() -> bool:
        return False

    def is_jit_compiled(func: FunctionType) -> bool:
        return False

    def jit_suppress(func: FunctionType) -> FunctionType:
        return func

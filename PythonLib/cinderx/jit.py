# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# pyre-strict

from contextlib import contextmanager
from typing import Any, Callable, Generator
from warnings import catch_warnings, simplefilter, warn


INSTALLED: bool = False

# The JIT compiles arbitrary Python functions.  Ideally this type would exclude native
# functions, but that doesn't seem possible yet.
#
# pyre-ignore[33]: Not going to add a new type variable for every use of FuncAny.
FuncAny = Callable[..., Any]


try:
    from cinderjit import (
        disable,
        enable,
        force_compile,
        is_enabled,
        is_jit_compiled,
        jit_suppress,
        lazy_compile,
        precompile_all,
    )

    INSTALLED = True

except ImportError:

    def disable(compile_all: bool = False, deopt_all: bool = False) -> None:
        pass

    def enable() -> None:
        # Warn here because users might think this is function is how to enable the JIT
        # when it is not installed.
        warn(
            "Cinder JIT is not installed, calling cinderx.jit.enable() is doing nothing"
        )

    def force_compile(func: FuncAny) -> bool:
        return False

    def lazy_compile(func: FuncAny) -> bool:
        return False

    def is_enabled() -> bool:
        return False

    def is_jit_compiled(func: FuncAny) -> bool:
        return False

    def jit_suppress(func: FuncAny) -> FuncAny:
        return func

    def precompile_all(workers: int = 0) -> bool:
        return False


@contextmanager
def pause(deopt_all: bool = False) -> Generator[None, None, None]:
    """
    Context manager for temporarily pausing the JIT.

    This will disable the JIT from running on new functions, and if you set
    `deopt_all`, will also de-optimize all currently compiled functions to the
    interpreter.  When the JIT is unpaused, the compiled functions will be put
    back.
    """

    prev_enabled = is_enabled()
    if prev_enabled:
        disable(deopt_all=deopt_all)

    try:
        yield
    finally:
        if prev_enabled:
            # Disable the warning from enable() when the JIT is not
            # installed/initialized.
            with catch_warnings():
                simplefilter("ignore")
                enable()

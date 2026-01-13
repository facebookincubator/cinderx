# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# pyre-strict

from contextlib import contextmanager
from typing import Any, AsyncGenerator, Callable, Coroutine, Generator, TypeVar
from warnings import catch_warnings, simplefilter, warn


# The JIT compiles arbitrary Python functions.  Ideally this type would exclude native
# functions, but that doesn't seem possible yet.
#
# pyre-ignore[33]: Not going to add a new type variable for every use of FuncAny.
FuncAny = Callable[..., Any]


try:
    from cinderjit import (
        _deopt_gen,
        append_jit_list,
        auto,
        clear_runtime_stats,
        compile_after_n_calls,
        count_interpreted_calls,
        disable,
        disable_emit_type_annotation_guards,
        disable_hir_inliner,
        disable_specialized_opcodes,
        enable,
        enable_emit_type_annotation_guards,
        enable_hir_inliner,
        enable_specialized_opcodes,
        force_compile,
        force_uncompile,
        get_allocator_stats,
        get_and_clear_inline_cache_stats,
        get_and_clear_runtime_stats,
        get_compilation_time,
        get_compile_after_n_calls,
        get_compiled_functions,
        get_compiled_size,
        get_compiled_spill_stack_size,
        get_compiled_stack_size,
        get_function_compilation_time,
        get_function_hir_opcode_counts,
        get_inlined_functions_stats,
        get_jit_list,
        get_num_inlined_functions,
        is_enabled,
        is_hir_inliner_enabled,
        is_inline_cache_stats_collection_enabled,
        is_jit_compiled,
        jit_suppress,
        jit_unsuppress,
        lazy_compile,
        mlock_profiler_dependencies,
        multithreaded_compile_test,
        page_in_profiler_dependencies,
        precompile_all,
        read_jit_list,
        set_max_code_size,
    )

except ImportError:
    TDeoptGenYield = TypeVar("TDeoptGenYield")
    TDeoptGenSend = TypeVar("TDeoptGenSend")
    TDeoptGenReturn = TypeVar("TDeoptGenReturn")

    def _deopt_gen(
        gen: Generator[TDeoptGenYield, TDeoptGenSend, TDeoptGenReturn]
        | AsyncGenerator[TDeoptGenYield, TDeoptGenSend]
        | Coroutine[TDeoptGenYield, TDeoptGenSend, TDeoptGenReturn],
    ) -> bool:
        return False

    def append_jit_list(entry: str) -> None:
        return None

    def auto() -> None:
        return None

    def clear_runtime_stats() -> None:
        return None

    def compile_after_n_calls(calls: int) -> None:
        return None

    def count_interpreted_calls(func: FuncAny) -> int:
        return 0

    def disable(deopt_all: bool = False) -> None:
        return None

    def disable_emit_type_annotation_guards() -> None:
        return None

    def disable_hir_inliner() -> None:
        return None

    def disable_specialized_opcodes() -> None:
        return None

    def enable() -> None:
        # Warn here because users might think this is function is how to enable the JIT
        # when it is not installed.
        warn(
            "Cinder JIT is not installed, calling cinderx.jit.enable() is doing nothing"
        )

    def enable_emit_type_annotation_guards() -> None:
        return None

    def enable_hir_inliner() -> None:
        return None

    def enable_specialized_opcodes() -> None:
        return None

    def force_compile(func: FuncAny) -> bool:
        return False

    def force_uncompile(func: FuncAny) -> bool:
        return False

    def get_allocator_stats() -> dict[str, int]:
        return {}

    def get_and_clear_inline_cache_stats() -> dict[str, object]:
        return {}

    def get_and_clear_runtime_stats() -> dict[str, object]:
        return {}

    def get_compilation_time() -> int:
        return 0

    def get_compile_after_n_calls() -> int | None:
        return None

    def get_compiled_functions() -> list[FuncAny]:
        return []

    def get_compiled_size(func: FuncAny) -> int:
        return 0

    def get_compiled_spill_stack_size(func: FuncAny) -> int:
        return 0

    def get_compiled_stack_size(func: FuncAny) -> int:
        return 0

    def get_function_compilation_time(func: FuncAny) -> int:
        return 0

    def get_function_hir_opcode_counts(func: FuncAny) -> dict[str, int] | None:
        return {}

    def get_inlined_functions_stats(func: FuncAny) -> dict[str, object]:
        return {}

    def get_jit_list() -> tuple[dict[str, set[str]], dict[str, dict[str, set[int]]]]:
        return ({}, {})

    def get_num_inlined_functions(func: FuncAny) -> int:
        return 0

    def is_enabled() -> bool:
        return False

    def is_hir_inliner_enabled() -> bool:
        return False

    def is_inline_cache_stats_collection_enabled() -> bool:
        return False

    def is_jit_compiled(func: FuncAny) -> bool:
        return False

    def jit_suppress(func: FuncAny) -> FuncAny:
        return func

    def jit_unsuppress(func: FuncAny) -> FuncAny:
        return func

    def lazy_compile(func: FuncAny) -> bool:
        return False

    def mlock_profiler_dependencies() -> None:
        return None

    def multithreaded_compile_test() -> None:
        return None

    def page_in_profiler_dependencies() -> list[str]:
        return []

    def precompile_all(workers: int = 0) -> bool:
        return False

    def read_jit_list(path: str) -> None:
        return None

    def set_max_code_size(max_code_size: int) -> None:
        return None


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

# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

import ast
import sys
from typing import Callable, Iterable

from cinderx.compiler.static.pyrefly_info import EMPTY_TYPE_INFO, Pyrefly
from cinderx.compiler.static.pyrefly_type_binder import PyreflyTypeBinder
from cinderx.compiler.static.type_binder import TypeBinder
from cinderx.compiler.strict.compiler import Compiler
from cinderx.compiler.strict.flag_extractor import Flags
from cinderx.compiler.symbols import SymbolVisitor


class PyreflyCompiler(Compiler):
    def __init__(
        self,
        pyrefly: Pyrefly | None = None,
        static_opt_out: set[str] | None = None,
        static_opt_in: set[str] | None = None,
        path: Iterable[str] | None = None,
        stub_path: str | None = None,
        allow_list_prefix: Iterable[str] | None = None,
        allow_list_exact: Iterable[str] | None = None,
        log_time_func: Callable[[], TIMING_LOGGER_TYPE] | None = None,
        enable_patching: bool = False,
        use_py_compiler: bool = False,
        allow_list_regex: Iterable[str] | None = None,
    ):
        super().__init__(
            path or sys.path,
            stub_path or "",
            allow_list_prefix or [],
            allow_list_exact or [],
            log_time_func,
            enable_patching,
            use_py_compiler,
            allow_list_regex,
        )
        self.pyrefly = pyrefly
        self.static_opt_in = static_opt_in
        self.static_opt_out = static_opt_out or set()

    def get_flags(
        self, module_name: str, pyast: ast.Module, override_flags: Flags
    ) -> Flags:
        if self.static_opt_in is not None:
            return Flags(is_static=module_name in self.static_opt_in).merge(
                override_flags
            )

        return Flags(is_static=module_name not in self.static_opt_out).merge(
            override_flags
        )

    def make_type_binder(
        self,
        symbols: SymbolVisitor,
        filename: str,
        compiler: Compiler,
        module_name: str,
        optimize: int,
        enable_patching: bool = False,
    ) -> TypeBinder:
        type_info = EMPTY_TYPE_INFO
        if self.pyrefly is not None:
            type_info = self.pyrefly.load_type_info(module_name) or EMPTY_TYPE_INFO
        return PyreflyTypeBinder(
            symbols,
            filename,
            compiler,
            module_name,
            optimize,
            enable_patching,
            type_info,
        )

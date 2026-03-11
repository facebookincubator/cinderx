# Copyright (c) Meta Platforms, Inc. and affiliates.

import ast
import sys

from cinderx.compiler.static.pyrefly_type_binder import (
    PyreflyTypeBinder,
    PyreflyTypeInfo,
)
from cinderx.compiler.static.type_binder import TypeBinder
from cinderx.compiler.strict.compiler import Compiler
from cinderx.compiler.strict.flag_extractor import Flags
from cinderx.compiler.symbols import SymbolVisitor


class PyreflyCompiler(Compiler):
    def __init__(
        self,
        type_info: PyreflyTypeInfo | None = None,
        non_static_modules: set[str] | None = None,
    ):
        super().__init__(sys.path, "", [], [])
        assert type_info is not None
        self.type_info = type_info
        self.non_static_modules = non_static_modules

    def get_flags(
        self, module_name: str, pyast: ast.Module, override_flags: Flags
    ) -> Flags:
        if self.non_static_modules is not None:
            if module_name in self.non_static_modules:
                return Flags().merge(override_flags)

            return Flags(is_static=True).merge(override_flags)

        return super().get_flags(module_name, pyast, override_flags)

    def make_type_binder(
        self,
        symbols: SymbolVisitor,
        filename: str,
        compiler: Compiler,
        module_name: str,
        optimize: int,
        enable_patching: bool = False,
    ) -> TypeBinder:
        return PyreflyTypeBinder(
            symbols,
            filename,
            compiler,
            module_name,
            optimize,
            enable_patching,
            self.type_info,
        )

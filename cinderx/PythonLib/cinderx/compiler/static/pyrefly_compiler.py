# Copyright (c) Meta Platforms, Inc. and affiliates.

from cinderx.compiler.errors import ErrorSink
from cinderx.compiler.static import Compiler, StaticCodeGenBase
from cinderx.compiler.static.pyrefly_type_binder import (
    PyreflyTypeBinder,
    PyreflyTypeInfo,
)
from cinderx.compiler.static.type_binder import TypeBinder
from cinderx.compiler.symbols import SymbolVisitor


class PyreflyCompiler(Compiler):
    def __init__(
        self,
        code_generator: type[StaticCodeGenBase],
        error_sink: ErrorSink | None = None,
        type_info: PyreflyTypeInfo | None = None,
    ):
        super().__init__(code_generator, error_sink)
        assert type_info is not None
        self.type_info = type_info

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

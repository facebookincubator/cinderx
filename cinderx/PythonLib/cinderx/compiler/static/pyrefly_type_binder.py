# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
from ast import AST, Name, Return
from typing import TYPE_CHECKING

from ..errors import TypedSyntaxError
from ..symbols import SymbolVisitor
from .effects import NarrowingEffect
from .module_table import ModuleTable
from .pyrefly_info import PyreflyTypeInfo
from .type_binder import TerminalKind, TypeBinder
from .types import Class

if TYPE_CHECKING:
    from .compiler import Compiler


class PyreflyTypeBinder(TypeBinder):
    """TypeBinder that uses pyrefly type inference to set types on expression nodes.

    For each expression node, looks up the pyrefly-inferred type,
    resolves it via the module table, and sets it on the node.
    """

    def __init__(
        self,
        symbols: SymbolVisitor,
        filename: str,
        compiler: Compiler,
        module_name: str,
        optimize: int,
        enable_patching: bool = False,
        type_info: PyreflyTypeInfo | None = None,
    ) -> None:
        super().__init__(
            symbols, filename, compiler, module_name, optimize, enable_patching
        )
        assert type_info is not None
        self._type_info = type_info

    def visit(self, node: AST, *args: object) -> NarrowingEffect | None:
        ret = super().visit(node, *args)
        if isinstance(node, ast.expr) and self._type_info is not None:
            # For now, we only try to get type information for class instances
            # and literals, disregarding any type parameters, and doing nothing
            # if we see a some other type_info kind like a callable.
            classname = self._type_info.lookup_typename(node)
            declared_type = None
            if classname:
                resolved = self._type_info.resolve_classname(
                    classname, self.modules, self.type_env
                )
                if resolved is not None:
                    declared_type = resolved.instance

            if declared_type is None:
                declared_type = self.type_env.dynamic.instance

            self.set_type(node, declared_type)
            if isinstance(node, Name) and isinstance(node.ctx, ast.Store):
                try:
                    self.declare_local(node.id, declared_type)
                except TypedSyntaxError:
                    pass  # already declared, just update the type
        return ret

    def visitReturn(self, node: Return) -> None:
        self.set_terminal_kind(node, TerminalKind.RaiseOrReturn)
        if node.value is not None:
            self.visit(node.value)

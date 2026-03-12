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


def _resolve_classname(qname: str, modules: dict[str, ModuleTable]) -> Class | None:
    """Resolve a dotted qname like 'builtins.int' to a Class.

    Splits the qname on '.' and tries progressively shorter prefixes
    as module names, then walks the remainder as nested attributes.
    """
    parts = qname.split(".")

    # Try progressively shorter prefixes as module names
    for i in range(len(parts) - 1, 0, -1):
        mod_name = ".".join(parts[:i])
        if mod_name in modules:
            mod = modules[mod_name]
            result = mod.get_child(parts[i], mod_name)
            if result is None:
                continue
            # Walk any remaining parts (e.g. nested classes)
            for part in parts[i + 1 :]:
                if isinstance(result, Class):
                    result = result.get_child(part, mod_name)
                else:
                    return None
                if result is None:
                    return None
            if isinstance(result, Class):
                return result
            return None

    # No dot — try builtins
    if len(parts) == 1:
        builtins = modules.get("builtins")
        if builtins is not None:
            result = builtins.get_child(parts[0], "builtins")
            if isinstance(result, Class):
                return result

    return None


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
                resolved = _resolve_classname(classname, self.modules)
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

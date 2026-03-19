# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
from ast import AST, Attribute, Call, Compare, Constant, Expr, Name, Return
from collections.abc import Sequence
from typing import TYPE_CHECKING

from ..errors import TypedSyntaxError
from ..symbols import SymbolVisitor
from .effects import NarrowingEffect
from .pyrefly_info import PyreflyTypeInfo
from .type_binder import (
    PRESERVE_REFINED_FIELDS,
    PreserveRefinedFields,
    TerminalKind,
    TypeBinder,
)
from .types import (
    # CInstance,
    # Class,
    # CType,
    Dataclass,
    DataclassField,
    ModuleInstance,
    TypeDescr,
)

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
        if isinstance(node, ast.expr) and self._type_info is not None:
            ret = super().generic_visit(node, *args)
            # pyre-fixme[16]: Optional type has no attribute `lookup`.
            declared_type = self._type_info.lookup(node, self.modules, self.type_env)

            if declared_type is None:
                declared_type = self.type_env.dynamic.instance
                if isinstance(node, (ast.List, ast.ListComp)):
                    declared_type = self.type_env.list.instance
                elif isinstance(node, (ast.Dict, ast.DictComp)):
                    declared_type = self.type_env.dict.instance
            self.set_type(node, declared_type)

            if isinstance(node, Compare):
                for op in node.ops:
                    self.set_type(op, self.type_env.DYNAMIC)

            # Name: set PreserveRefinedFields (always), declare locals for
            # Store context, and set TypeDescr for module-level names so
            # that bind_call can emit direct invocations.  When pyrefly
            # doesn't resolve the type, fall back to the module table so
            # CinderX-specific types (e.g. ModuleInstance) are preserved.
            elif isinstance(node, Name):
                self.set_node_data(node, PreserveRefinedFields, PRESERVE_REFINED_FIELDS)
                if isinstance(node.ctx, ast.Store):
                    try:
                        self.declare_local(node.id, declared_type)
                    except TypedSyntaxError:
                        pass  # already declared, just update the type
                mod_typ, descr = self.module.resolve_name_with_descr(
                    node.id, self.context_qualname
                )
                if descr is not None:
                    self.set_node_data(node, TypeDescr, descr)
                if (
                    mod_typ is not None
                    and declared_type is self.type_env.dynamic.instance
                ):
                    self.set_type(node, mod_typ)

            # Attribute: when the base is a ModuleInstance, set TypeDescr
            # for direct access and call bind_attr to resolve from the
            # CinderX module table — this ensures CinderX-specific types
            # (e.g. DataclassFieldFunction, DataclassDecorator) are used.
            # Set PreserveRefinedFields when the attribute is refinable.
            elif isinstance(node, Attribute):
                base = self.get_type(node.value)
                if isinstance(base, ModuleInstance):
                    self.set_node_data(
                        node, TypeDescr, ((base.module_name,), node.attr)
                    )
                    # Always call bind_attr for module attributes so that
                    # CinderX-specific types (e.g. DataclassFieldFunction,
                    # DataclassDecorator) are used regardless of what
                    # pyrefly resolved.
                    base.bind_attr(node, self, None)
                if self.is_refinable(node):
                    self.set_node_data(
                        node, PreserveRefinedFields, PRESERVE_REFINED_FIELDS
                    )

            # Call: invoke bind_call on the func's type to populate
            # ArgMapping (and ClassCallInfo for class instantiation).
            elif isinstance(node, Call):
                self.get_type(node.func).bind_call(node, self, None)
                # When pyrefly compiles modules that aren't normally
                # statically compiled, dataclasses.field() may resolve
                # to DataclassField via the CinderX module table.
                # In non-Dataclass classes (e.g. decorated with
                # @deprecated on top of @dataclass), this causes type
                # errors in visitAnnAssign.  Reset to dynamic and mark
                # the class for non-static compilation in that case.
                if isinstance(self.get_type(node), DataclassField) and not (
                    isinstance(self.scope, ast.ClassDef)
                    and isinstance(self.get_type(self.scope), Dataclass)
                ):
                    self.set_type(node, declared_type)
                    if isinstance(self.scope, ast.ClassDef):
                        self.module.compile_non_static.add(self.scope)
        else:
            return super().visit(node, *args)

        return ret

    def visit_check_terminal(self, nodes: Sequence[ast.stmt]) -> TerminalKind:
        # Treat a body consisting of just `...` (Ellipsis) as a terminal
        # statement, so that stub functions like Protocol methods don't
        # trigger "can implicitly return None" errors.
        if (
            len(nodes) == 1
            and isinstance(nodes[0], Expr)
            and isinstance(nodes[0].value, Constant)
            and nodes[0].value.value is ...
        ):
            self.visit(nodes[0])
            self.set_terminal_kind(nodes[0], TerminalKind.RaiseOrReturn)
            return TerminalKind.RaiseOrReturn
        return super().visit_check_terminal(nodes)

    def visitReturn(self, node: Return) -> None:
        self.set_terminal_kind(node, TerminalKind.RaiseOrReturn)
        if node.value is not None:
            self.visit(node.value)

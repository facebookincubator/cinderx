# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
import json
from ast import AST, Name
from typing import TYPE_CHECKING

from ..errors import TypedSyntaxError
from ..symbols import SymbolVisitor
from .effects import NarrowingEffect
from .type_binder import TypeBinder

if TYPE_CHECKING:
    from .compiler import Compiler


class PyreflyTypeInfo:
    """Loads and indexes a pyrefly type trace JSON file.

    The JSON file contains:
    - type_table: array of type entries (class, literal, callable)
    - locations: array of {loc: {start_line, start_col, end_line, end_col}, type: index}

    Locations map source positions to type_table indices. Positions use
    1-based lines and 0-based columns (end_col is exclusive), matching
    Python AST conventions.
    """

    def __init__(self, json_path: str) -> None:
        with open(json_path) as f:
            data = json.load(f)
        self._type_table: list[dict[str, object]] = data["type_table"]
        self._locations: dict[tuple[int, int, int, int], int] = {}
        for entry in data["locations"]:
            loc = entry["loc"]
            key = (
                loc["start_line"],
                loc["start_col"],
                loc["end_line"],
                loc["end_col"],
            )
            self._locations[key] = entry["type"]

    def _type_to_str(self, type_index: int) -> str:
        """Convert a type_table entry to a Python annotation string."""
        entry = self._type_table[type_index]
        kind = entry["kind"]
        if kind == "literal":
            return ""
        elif kind == "class":
            qname = str(entry["qname"])
            args = entry.get("args", [])
            assert isinstance(args, list)
            if not args:
                return qname
            arg_strs = [self._type_to_str(a) for a in args]
            if any(not s for s in arg_strs):
                return qname
            return f"{qname}[{', '.join(arg_strs)}]"
        elif kind == "callable":
            params = entry.get("params", [])
            assert isinstance(params, list)
            ret = entry.get("return_type")
            param_strs = [self._type_to_str(p) for p in params]
            ret_str = self._type_to_str(ret) if isinstance(ret, int) else ""
            if any(not s for s in param_strs) or not ret_str:
                return ""
            return f"Callable[[{', '.join(param_strs)}], {ret_str}]"
        return ""

    def lookup(self, node: AST) -> str:
        """Look up the type string for an AST node by its source position."""
        key = (
            node.lineno,  # pyre-ignore[16]
            node.col_offset,  # pyre-ignore[16]
            node.end_lineno,  # pyre-ignore[16]
            node.end_col_offset,  # pyre-ignore[16]
        )
        type_index = self._locations.get(key)
        if type_index is None:
            return ""
        return self._type_to_str(type_index)


class PyreflyTypeBinder(TypeBinder):
    """TypeBinder that uses pyrefly type inference to set types on expression nodes.

    For each expression node, looks up the pyrefly-inferred type,
    parses it as an annotation, resolves it, and sets it on the node.
    """

    def __init__(
        self,
        symbols: SymbolVisitor,
        filename: str,
        compiler: Compiler,
        module_name: str,
        optimize: int,
        type_info: PyreflyTypeInfo,
        enable_patching: bool = False,
    ) -> None:
        super().__init__(
            symbols, filename, compiler, module_name, optimize, enable_patching
        )
        self._type_info = type_info

    def visit(self, node: AST, *args: object) -> NarrowingEffect | None:
        ret = super().visit(node, *args)
        if isinstance(node, ast.expr):
            type_str = self._type_info.lookup(node)
            if type_str:
                annotation_node = ast.parse(type_str, "", "eval").body
                comp_type = self.module.resolve_annotation(
                    annotation_node, self.context_qualname
                )
                if comp_type is not None:
                    declared_type = comp_type.instance
                    self.set_type(node, declared_type)
                    if isinstance(node, Name) and isinstance(node.ctx, ast.Store):
                        try:
                            self.declare_local(node.id, declared_type)
                        except TypedSyntaxError:
                            pass  # already declared, just update the type
        return ret

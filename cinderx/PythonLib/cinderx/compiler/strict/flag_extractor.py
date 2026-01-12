# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
from ast import Import, Module
from dataclasses import dataclass
from typing import Any, Sequence

from ..symbols import ModuleScope, Scope, SymbolVisitor


@dataclass
class Flags:
    is_static: bool = False
    is_strict: bool = False

    def merge(self, other: Flags | None) -> Flags:
        if other is None:
            return self

        return Flags(
            is_static=self.is_static or other.is_static,
            is_strict=self.is_strict or other.is_strict,
        )


class BadFlagException(Exception):
    pass


class FlagExtractor(SymbolVisitor):
    is_static: bool
    is_strict: bool

    flag_may_appear: bool
    seen_docstring: bool
    seen_import: bool

    def __init__(self, future_flags: int = 0) -> None:
        super().__init__(future_flags)
        self.is_static = False
        self.is_strict = False

        self.flag_may_appear = True
        self.seen_docstring = False
        self.seen_import = False

    def get_flags(self, node: ast.AST) -> Flags:
        assert isinstance(node, ast.Module)
        self._handle_module(node)
        return Flags(is_static=self.is_static, is_strict=self.is_strict)

    def _handle_module(self, node: Module) -> None:
        for child in node.body:
            match child:
                case ast.Expr(ast.Constant(value)) if (
                    isinstance(value, str) and not self.seen_docstring
                ):
                    self.seen_docstring = True
                case ast.ImportFrom(module) if module == "__future__":
                    pass
                case ast.Constant(_):
                    pass
                case ast.Import(_) as import_node:
                    self._handle_import(import_node)
                case _:
                    self.flag_may_appear = False

    def _handle_import(self, node: Import) -> None:
        for import_ in node.names:
            name = import_.name

            if name not in ("__static__", "__strict__", "__future__"):
                self.flag_may_appear = False
                continue

            if not self.flag_may_appear:
                raise BadFlagException(
                    f"Cinder flag {name} must be at the top of a file"
                )

            if len(node.names) > 1:
                raise BadFlagException(
                    f"{name} flag may not be combined with other imports",
                )

            if import_.asname is not None:
                raise BadFlagException(f"{name} flag may not be aliased")

            match name:
                case "__static__":
                    self.is_static = True
                case "__strict__":
                    self.is_strict = True

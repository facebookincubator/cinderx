# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
import builtins
import sys
from types import CodeType
from typing import Any

from ..pyassem import PyFlowGraph312, PyFlowGraph314, PyFlowGraph315
from ..pycodegen import (
    CinderCodeGenerator312,
    CinderCodeGenerator314,
    CinderCodeGenerator315,
)
from .code_gen_base import StrictCodeGenBase


# Unused but still present until we remove it from IGSRV
enable_strict_features: bool = True


class StrictCodeGenerator312(StrictCodeGenBase, CinderCodeGenerator312):
    flow_graph = PyFlowGraph312


class StrictCodeGenerator314(StrictCodeGenerator312, CinderCodeGenerator314):
    flow_graph = PyFlowGraph314


class StrictCodeGenerator315(StrictCodeGenerator312, CinderCodeGenerator315):
    flow_graph = PyFlowGraph315


def strict_compile(
    name: str,
    filename: str,
    tree: ast.Module,
    source: str | bytes,
    optimize: int = 0,
    builtins: dict[str, Any] = builtins.__dict__,
) -> CodeType:
    code_gen = StrictCodeGenerator.make_code_gen(
        name, tree, filename, source, flags=0, optimize=optimize, builtins=builtins
    )
    return code_gen.getCode()


if sys.version_info >= (3, 15):
    StrictCodeGenerator = StrictCodeGenerator315
elif sys.version_info >= (3, 14):
    StrictCodeGenerator = StrictCodeGenerator314
else:
    StrictCodeGenerator = StrictCodeGenerator312

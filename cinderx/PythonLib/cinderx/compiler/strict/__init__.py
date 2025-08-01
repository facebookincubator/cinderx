# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
import builtins
import sys
from types import CodeType
from typing import Any

from ..pyassem import PyFlowGraph312, PyFlowGraphCinder310
from ..pycodegen import CinderCodeGenerator310, CinderCodeGenerator312
from .code_gen_base import StrictCodeGenBase


# Unused but still present until we remove it from IGSRV
enable_strict_features: bool = True


class StrictCodeGenerator310(StrictCodeGenBase, CinderCodeGenerator310):
    flow_graph = PyFlowGraphCinder310


class StrictCodeGenerator312(StrictCodeGenBase, CinderCodeGenerator312):
    flow_graph = PyFlowGraph312


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


if sys.version_info >= (3, 12):
    StrictCodeGenerator = StrictCodeGenerator312
else:
    StrictCodeGenerator = StrictCodeGenerator310

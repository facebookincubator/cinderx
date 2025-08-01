# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

from ast import AST, Name


def is_mutable(node: AST) -> bool:
    return isinstance(node, Name) and node.id == "mutable"

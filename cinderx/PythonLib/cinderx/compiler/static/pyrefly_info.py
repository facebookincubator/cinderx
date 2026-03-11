# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import json
import os
from ast import AST


class PyreflyTypeInfo:
    """Loads and indexes a pyrefly type trace JSON file.

    The JSON file contains:
    - type_table: array of type entries (class, literal, callable)
    - locations: array of {loc: {start_line, start_col, end_line, end_col}, type: index}

    Locations map source positions to type_table indices. Positions use
    1-based lines and 0-based columns (end_col is exclusive), matching
    Python AST conventions.
    """

    def __init__(self, data: dict[str, object]) -> None:
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

    def _lookup(self, node: AST) -> int | None:
        """Look up the type_table index for an AST node by its source position."""
        key = (
            node.lineno,  # pyre-ignore[16]
            node.col_offset,  # pyre-ignore[16]
            node.end_lineno,  # pyre-ignore[16]
            node.end_col_offset,  # pyre-ignore[16]
        )
        return self._locations.get(key)

    def lookup(self, node: AST) -> str:
        """Look up the type string for an AST node by its source position."""
        type_index = self._lookup(node)
        if type_index is None:
            return ""
        return self._type_to_str(type_index)

    def lookup_class_qname(self, node: AST) -> str:
        """Look up the qname for an AST node if its type is a simple class.

        We treat generic classes as their unparametrised "base" version,
        e.g. A[T] -> A
        """
        type_index = self._lookup(node)
        if type_index is None:
            return ""
        entry = self._type_table[type_index]
        if entry["kind"] == "class":
            # Ignore the generic args
            return str(entry["qname"])
        return ""

    @classmethod
    def load_json(cls, json_path: str) -> PyreflyTypeInfo:
        with open(json_path) as f:
            data = json.load(f)
        return cls(data)

    @classmethod
    def empty(cls) -> PyreflyTypeInfo:
        return cls({"type_table": [], "locations": []})


class Pyrefly:
    """Manages type information emitted by pyrefly."""

    def __init__(self, type_dir: str):
        self.type_dir = type_dir

    def load_type_info(self, module_name: str) -> PyreflyTypeInfo | None:
        if self.type_dir is None:
            return None
        json_path = os.path.join(self.type_dir, "types", f"{module_name}.json")
        if not os.path.isfile(json_path):
            return None
        return PyreflyTypeInfo.load_json(json_path)


EMPTY_TYPE_INFO = PyreflyTypeInfo.empty()

# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import json
import os
from ast import AST
from dataclasses import dataclass
from typing import TypedDict

from cinderx.compiler.static.module_table import ModuleTable
from cinderx.compiler.static.types import Class, TypeEnvironment, Value


class Location(TypedDict):
    start_line: int
    start_col: int
    end_line: int
    end_col: int


TypeKind = int


class LocationEntry(TypedDict):
    loc: LocationInfo
    type: TypeKind


class TypeInfo(TypedDict):
    type_table: list[TypeTableEntry]
    locations: list[LocationEntry]


class TypeTableEntry(TypedDict):
    kind: str
    qname: object
    promoted_type: TypeKind


@dataclass(slots=True, eq=True)
class LocationInfo:
    start_line: int
    start_col: int
    end_line: int
    end_col: int

    def __hash__(self) -> int:
        return (
            self.start_col
            ^ self.start_line << 14
            ^ self.end_col << 7
            ^ self.end_line << 32
        )

    @classmethod
    def from_location(cls, loc: Location):
        return cls(
            start_line=loc["start_line"],
            start_col=loc["start_col"],
            end_line=loc["end_line"],
            end_col=loc["end_col"],
        )

    @classmethod
    def from_node(cls, node: AST):
        return cls(
            start_line=node.lineno,  # pyre-ignore[16]
            start_col=node.col_offset,  # pyre-ignore[16]
            end_line=node.end_lineno,  # pyre-ignore[16]
            end_col=node.end_col_offset,  # pyre-ignore[16]
        )


class PyreflyTypeInfo:
    """Loads and indexes a pyrefly type trace JSON file.

    The JSON file contains:
    - type_table: array of type entries (class, literal, callable)
    - locations: array of {loc: {start_line, start_col, end_line, end_col}, type: index}

    Locations map source positions to type_table indices. Positions use
    1-based lines and 0-based columns (end_col is exclusive), matching
    Python AST conventions.
    """

    def __init__(self, data: TypeInfo) -> None:
        self._type_table: list[TypeTableEntry] = data["type_table"]
        self._locations: dict[LocationInfo, int] = {}
        for entry in data["locations"]:
            key = LocationInfo.from_location(entry["loc"])
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
        key = LocationInfo.from_node(node)
        return self._locations.get(key)

    def lookup(self, node: AST) -> str:
        """Look up the type string for an AST node by its source position."""
        type_index = self._lookup(node)
        if type_index is None:
            return ""
        return self._type_to_str(type_index)

    def _get_type_qname(self, type_index: TypeKind) -> str:
        entry = self._type_table[type_index]
        if entry["kind"] == "class":
            # Ignore the generic args
            return str(entry["qname"])
        elif entry["kind"] == "literal":
            if "promoted_type" in entry:
                return self._get_type_qname(entry["promoted_type"])
        return ""

    def lookup_typename(self, node: AST) -> str:
        """Look up the qname for an AST node if its type is a simple class.

        We treat generic classes as their unparametrised "base" version,
        e.g. A[T] -> A
        """
        type_index = self._lookup(node)
        if type_index is None:
            return ""
        return self._get_type_qname(type_index)

    @classmethod
    def load_json(cls, json_path: str) -> PyreflyTypeInfo:
        with open(json_path) as f:
            data = json.load(f)
        return cls(data)

    def resolve_classname(
        self, qname: str, modules: dict[str, ModuleTable], type_env: TypeEnvironment
    ) -> Class | None:
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
                    return result.inexact_type()
                elif isinstance(result, Value):
                    return result.klass
                return None

        # No dot — try builtins
        if len(parts) == 1:
            builtins = modules.get("builtins")
            if builtins is not None:
                result = builtins.get_child(parts[0], "builtins")
                if isinstance(result, Class):
                    return result

        return None

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

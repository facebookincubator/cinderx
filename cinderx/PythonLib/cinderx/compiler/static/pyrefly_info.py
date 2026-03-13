# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import json
import os
from ast import AST, Attribute
from dataclasses import dataclass
from typing import TypedDict

from cinderx.compiler.static.module_table import ModuleTable
from cinderx.compiler.static.types import (
    Class,
    Function,
    MethodType,
    TypeEnvironment,
    Value,
)

# Remove this once we drop the Python 3.10 tests.
try:
    from typing import Self
except ImportError:
    from typing import TypeVar

    Self = TypeVar("T", bound="Self")


class Location(TypedDict):
    start_line: int
    start_col: int
    end_line: int
    end_col: int


TypeKind = int


class LocationEntry(TypedDict):
    loc: Location
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
    def from_location(cls, loc: Location) -> Self:
        return cls(
            start_line=loc["start_line"],
            start_col=loc["start_col"],
            end_line=loc["end_line"],
            end_col=loc["end_col"],
        )

    @classmethod
    def from_node(cls, node: AST) -> Self:
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

    def _lookup(self, node: AST) -> int | None:
        """Look up the type_table index for an AST node by its source position."""
        key = LocationInfo.from_node(node)
        return self._locations.get(key)

    def lookup(
        self,
        node: AST,
        modules: dict[str, ModuleTable],
        type_env: TypeEnvironment,
    ) -> Value | None:
        """Look up the type string for an AST node by its source position.

        For now, we only try to get type information for class instances
        and literals, disregarding any type parameters, bound methods, and
        doing nothing if we see a some other type_info kind like a callable.
        """
        type_index = self._lookup(node)
        if type_index is None:
            return None

        entry = self._type_table[type_index]
        # Try non-types
        if entry["kind"] == "bound_method":
            # pyre-ignore[27]: We need a more elaborate type declaration to represent tagged union data layout
            defining_class_qname = str(entry["defining_class"])
            resolved_class = self.resolve_classname(
                defining_class_qname, modules, type_env
            )
            if resolved_class is not None and isinstance(node, Attribute):
                member = resolved_class.get_member(node.attr)
                if isinstance(member, Function):
                    return MethodType(
                        resolved_class.type_name,
                        member.node,
                        node.value,
                        member,
                    )
        elif entry["kind"] == "callable" and "defining_func" in entry:
            # pyre-ignore[27]: We need a more elaborate type declaration to represent tagged union data layout
            defining_func_qname = str(entry["defining_func"])
            resolved_func = self.resolve_func(defining_func_qname, modules, type_env)
            if resolved_func is not None:
                return resolved_func

        # Fallback to types
        return self.lookup_type(type_index, modules, type_env)

    def lookup_type(
        self,
        type_index: TypeKind,
        modules: dict[str, ModuleTable],
        type_env: TypeEnvironment,
    ) -> Value | None:
        entry = self._type_table[type_index]
        if entry["kind"] == "class":
            qname = str(entry["qname"])
            resolved = self.resolve_classname(qname, modules, type_env)
            if resolved is not None:
                return resolved.instance
        elif entry["kind"] == "literal":
            if "promoted_type" in entry:
                return self.lookup_type(entry["promoted_type"], modules, type_env)
        elif entry["kind"] == "other_form":
            if entry["qname"] == "None":
                resolved = self.resolve_classname("builtins.None", modules, type_env)
                if resolved is not None:
                    return resolved.instance

        return None

    @classmethod
    def load_json(cls, json_path: str) -> PyreflyTypeInfo:
        with open(json_path) as f:
            data = json.load(f)
        return cls(data)

    def _resolve_qname(
        self,
        qname: str,
        modules: dict[str, ModuleTable],
    ) -> tuple[str, Value, list[str]] | None:
        """Resolve a dotted qname into a top-level value and a chain of attributes.

        Splits the qname on '.' and tries progressively shorter prefixes
        as module names, then looks up the next name in the chain as a
        top-level value in that module. Returns the module name, the top-level
        value, and the remaining attributes, e.g. if foo.bar.baz is a module,
          _resolve_qname("foo.bar.baz.A.B.f") ->
              ("foo.bar.baz", foo.bar.baz.A, ["B", "f"])
        """
        parts = qname.split(".")
        for i in range(len(parts) - 1, 0, -1):
            mod_name = ".".join(parts[:i])
            if mod_name in modules:
                mod = modules[mod_name]
                result = mod.get_child(parts[i], mod_name)
                if result is not None:
                    return mod_name, result, parts[i + 1 :]
        return None

    def resolve_func(
        self,
        qname: str,
        modules: dict[str, ModuleTable],
        type_env: TypeEnvironment,
    ) -> Value | None:
        """Resolve a dotted qname like 'module.path.func_name' to a callable Value.

        This handles both user-defined Functions and built-in callable
        types like LenFunction.
        """
        if ret := self._resolve_qname(qname, modules):
            _, result, parts = ret
        else:
            return None
        for part in parts:
            if isinstance(result, Class):
                result = result.get_member(part)
            else:
                result = None
            if result is None:
                break
        if result is not None:
            return result

        return None

    def resolve_classname(
        self, qname: str, modules: dict[str, ModuleTable], type_env: TypeEnvironment
    ) -> Class | None:
        """Resolve a dotted qname like 'builtins.int' to a Class."""
        if ret := self._resolve_qname(qname, modules):
            mod_name, result, parts = ret
            # Walk any remaining parts (e.g. nested classes)
            for part in parts:
                if isinstance(result, Class):
                    # pyre-ignore[16]: `Class` has no attribute `get_child`
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
        if "." not in qname:
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

    def __init__(self, type_dir: str) -> None:
        self.type_dir = type_dir

    def load_type_info(self, module_name: str) -> PyreflyTypeInfo | None:
        if self.type_dir is None:
            return None
        json_path = os.path.join(self.type_dir, "types", f"{module_name}.json")
        if not os.path.isfile(json_path):
            return None
        return PyreflyTypeInfo.load_json(json_path)


EMPTY_TYPE_INFO: PyreflyTypeInfo = PyreflyTypeInfo.empty()

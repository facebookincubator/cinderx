# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe

from ast import Module
from symtable import SymbolTable
from typing import Any, Callable, List, Protocol

NONSTRICT_MODULE_KIND: int
STATIC_MODULE_KIND: int
STRICT_MODULE_KIND: int
STUB_KIND_MASK_ALLOWLIST: int
STUB_KIND_MASK_NONE: int
STUB_KIND_MASK_STRICT: int
STUB_KIND_MASK_TYPING: int

class StrictModuleError(Exception):
    pass


class StrictAnalysisResult:
    ast: Module
    errors: List[tuple[str, str, int, int]]
    file_name: str
    is_valid: bool
    module_kind: int
    module_name: str
    stub_kind: int
    symtable: SymbolTable
    def __init__(
        self,
        _module_name: str,
        _file_name: str,
        _mod_kind: int,
        _stub_kind: int,
        _ast: Module,
        _symtable: SymbolTable,
        _errors: List[tuple[str, str, int, int]],
        /,
    ) -> None: ...

class IStrictModuleLoader(Protocol):
    def check(self, _mod_name: str, /) -> StrictAnalysisResult: ...
    def check_source(
        self,
        _source: str | bytes,
        _file_name: str,
        _mod_name: str,
        _submodule_search_locations: List[str],
        /,
    ) -> StrictAnalysisResult: ...
    def set_force_strict(self, _force: bool, /) -> None: ...
    def set_force_strict_by_name(self, _name: str, /) -> None: ...

class StrictModuleLoader(IStrictModuleLoader):
    def __init__(
        self,
        _import_paths: List[str],
        _stub_import_path: str,
        _allow_list: List[str],
        _allow_list_exact: List[str],
        _load_strictmod_builtin: bool = True,
        _allow_list_regex: List[str] | None = None,
        _verbose_logs: bool = False,
        _disable_analysis: bool = False,
        /,
    ) -> None: ...
    def delete_module(self, name: str) -> bool: ...
    def get_analyzed_count(self) -> int: ...
    def set_force_strict(self, force: bool) -> None: ...
    def set_force_strict_by_name(self, *args, **kwargs) -> Any: ...

StrictModuleLoaderFactory = Callable[
    [List[str], str, List[str], List[str], bool, List[str], bool, bool],
    IStrictModuleLoader,
]

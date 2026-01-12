# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

import _imp
import ast
import importlib
import os
import sys
import zipimport
from importlib.machinery import (
    BYTECODE_SUFFIXES,
    ExtensionFileLoader,
    FileFinder,
    SOURCE_SUFFIXES,
    SourceFileLoader,
    SourcelessFileLoader,
)
from types import CodeType

from .pycodegen import compile_code


# pyre-fixme[13]: path inherited but not initialized
class PySourceFileLoader(SourceFileLoader):
    # pyre-fixme[14]: source_to_code invalid override
    def source_to_code(
        self,
        data: ast.Expression | ast.Interactive | ast.Module | str,
        path: os.PathLike[str] | str,
        *,
        _optimize: int | list[int] = -1,
    ) -> CodeType:
        """Similar to SourceFileLoader.source_to_code
        but use the python based bytecode generator from
        compiler/pycodegen.py
        """
        # pyre-ignore[16]
        return importlib._bootstrap._call_with_frames_removed(
            compile_code, data, path, "exec", optimize=_optimize
        )


def _install_source_loader_helper(source_loader_type: type[PySourceFileLoader]) -> None:
    extensions = ExtensionFileLoader, _imp.extension_suffixes()
    source = source_loader_type, SOURCE_SUFFIXES
    bytecode = SourcelessFileLoader, BYTECODE_SUFFIXES
    supported_loaders = [extensions, source, bytecode]
    sys.path_hooks[:] = [
        zipimport.zipimporter,
        FileFinder.path_hook(*supported_loaders),
    ]
    sys.path_importer_cache.clear()


def _install_py_loader() -> None:
    _install_source_loader_helper(PySourceFileLoader)


def _install_strict_loader(enable_patching: bool) -> None:
    from .strict.loader import install

    install(enable_patching)

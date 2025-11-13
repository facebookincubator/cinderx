# Portions copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Package for compiling Python source code

There are several functions defined at the top level that are imported
from modules contained in the package.

compile(source, filename, mode, flags=None, dont_inherit=None)
    Returns a code object.  A replacement for the builtin compile() function.

compileFile(filename)
    Generates a .pyc file by compiling filename.
"""

import ast
from types import CodeType
from typing import Any

from .pycodegen import CinderCodeGenerator, compile, compile_code, compileFile


def exec_cinder(
    source: str | bytes | ast.Module | ast.Expression | ast.Interactive | CodeType,
    locals: dict[str, Any],
    globals: dict[str, Any],
    modname: str = "<module>",
) -> None:
    if isinstance(source, CodeType):
        code = source
    else:
        code = compile_code(
            source, "<module>", "exec", compiler=CinderCodeGenerator, modname=modname
        )

    exec(code, locals, globals)


__all__ = ("compile", "compile_code", "compileFile", "exec_cinder")

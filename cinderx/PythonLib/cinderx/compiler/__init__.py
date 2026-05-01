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
import dis
import sys
from io import StringIO
from types import CodeType
from typing import Any

from .opcodes import STATIC_CONST_OPCODES, STATIC_OPNAMES
from .pycodegen import CinderCodeGenerator, compile, compile_code, compileFile


def make_static_instr(instr: dis.Instruction, co: object) -> dis.Instruction:
    if instr.opcode in STATIC_CONST_OPCODES:
        # pyre-fixme[21]: Could not find name `_get_code_object` in `dis` (stubbed).
        from dis import _get_code_object

        return dis.Instruction(
            STATIC_OPNAMES[instr.opcode],
            instr[1],
            instr.arg,
            _get_code_object(co).co_consts[instr.arg],
            *instr[4:],
        )
    return dis.Instruction(STATIC_OPNAMES[instr.opcode], *instr[1:])


def get_disassembly_as_string(co: object, recurse: bool = False) -> str:
    s = StringIO()
    if sys.version_info < (3, 14):
        dis.dis(co, file=s)
        return s.getvalue()

    # pyre-fixme[21]: Could not find name `Formatter` in `dis` (stubbed).
    # pyre-fixme[21]: Could not find name `_get_code_object` in `dis` (stubbed).
    from dis import _get_code_object, Bytecode, Formatter

    formatter = Formatter(file=s, offset_width=3)
    bc = Bytecode(co)
    extended = False
    for instr in bc:
        if extended and instr.opname != "EXTENDED_ARG":
            extended = False
            instr = make_static_instr(instr, co)
        elif instr.opname == "EXTENDED_OPCODE":
            extended = True
        formatter.print_instruction(instr, False)

    if recurse:
        for const in _get_code_object(co).co_consts:
            if isinstance(const, CodeType):
                s.write(f"\nDisassembly of {const!r}:\n")
                s.write(get_disassembly_as_string(const, recurse=True))

    return s.getvalue()


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


__all__ = (
    "compile",
    "compile_code",
    "compileFile",
    "exec_cinder",
    "get_disassembly_as_string",
    "make_static_instr",
)

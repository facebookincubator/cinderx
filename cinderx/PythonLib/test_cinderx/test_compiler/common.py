# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import dis
import glob
import inspect
import re
import sys

from dataclasses import dataclass
from io import StringIO
from os import path
from subprocess import run
from types import CodeType, FunctionType, MethodType
from typing import Any, Callable, Generator, Iterable
from unittest import TestCase

from cinderx.compiler.opcodes import STATIC_CONST_OPCODES, STATIC_OPNAMES
from cinderx.compiler.pyassem import Instruction, PyFlowGraph
from cinderx.compiler.pycodegen import (
    CinderCodeGenerator310,
    CodeGenerator,
    CodeGenerator312,
    CodeGenerator314,
    make_compiler,
)

if sys.version_info >= (3, 14):
    from dis import (
        _get_code_object,
        _make_labels_map,
        _parse_exception_table,
        ArgResolver,
        Bytecode,
        Formatter,
    )

# Any value that can be passed into dis.dis() and dis.get_instructions().
Disassembleable = Callable[..., Any] | CodeType | FunctionType | MethodType


_UNSPECIFIED = object()

# dis exception table output lines look like:
#   4 to 24 -> 122 [0] lasti
DIS_EXC_RE: re.Pattern[str] = re.compile(r"(\d+) to (\d+) -> (\d+) \[(\d+)\]( lasti)?")


def get_repo_root() -> str:
    dirname = path.dirname(__file__)
    completed_process = run(
        ["git", "rev-parse", "--show-toplevel"], cwd=dirname, capture_output=True
    )
    if completed_process.returncode:
        print("Error occurred", file=sys.stderr)
        sys.exit(1)
    return completed_process.stdout.strip().decode("utf8")


def glob_test(target_dir: str, pattern: str, adder: Callable[[str, str], None]) -> None:
    for fname in glob.glob(path.join(target_dir, pattern), recursive=True):
        modname = path.relpath(fname, target_dir)
        adder(modname, fname)


class CompilerTest(TestCase):
    if sys.version_info >= (3, 12):
        SUPER_ATTR = "LOAD_SUPER_ATTR"
        CALL = "CALL"
        COMPARE_JUMP_NONZERO = "POP_JUMP_IF_NONZERO"
        COMPARE_JUMP_ZERO = "POP_JUMP_IF_ZERO"
    else:
        SUPER_ATTR = "LOAD_ATTR_SUPER"
        CALL = "CALL_FUNCTION"
        COMPARE_JUMP_NONZERO = "JUMP_IF_NONZERO_OR_POP"
        COMPARE_JUMP_ZERO = "JUMP_IF_ZERO_OR_POP"

    def get_disassembly_as_string(self, co: Disassembleable) -> str:
        s = StringIO()
        if sys.version_info < (3, 14):
            dis.dis(co, file=s)
            return s.getvalue()

        # pyre-ignore[10]: Formatter defined for 3.14+
        formatter = Formatter(file=s, offset_width=3)
        # pyre-ignore[10]: Formatter defined for 3.14+
        bc = Bytecode(co)
        extended = False
        for instr in bc:
            if extended and instr.opname != "EXTENDED_ARG":
                extended = False
                instr = self.make_static_instr(instr, co)
            elif instr.opname == "EXTENDED_OPCODE":
                extended = True

            formatter.print_instruction(instr, False)

        return s.getvalue()

    def make_static_instr(
        self, instr: dis.Instruction, co: Disassembleable
    ) -> dis.Instruction:
        if instr.opcode in STATIC_CONST_OPCODES:
            return dis.Instruction(
                STATIC_OPNAMES[instr.opcode],
                instr[1],
                instr.arg,
                # pyre-ignore[10]: Formatter defined for 3.14+
                _get_code_object(co).co_consts[instr.arg],
                *instr[4:],
            )
        return dis.Instruction(STATIC_OPNAMES[instr.opcode], *instr[1:])

    def assertInBytecode(
        self,
        x: Disassembleable,
        opname: str,
        argval: object = _UNSPECIFIED,
        *,
        index: object = _UNSPECIFIED,
    ) -> dis.Instruction | None:
        """Returns instr if op is found, otherwise throws AssertionError"""
        for i, instr in enumerate(self.get_instructions(x)):
            if instr.opname == opname:
                argmatch = argval is _UNSPECIFIED or instr.argval == argval
                indexmatch = index is _UNSPECIFIED or i == index
                if argmatch and indexmatch:
                    return instr
        disassembly = self.get_disassembly_as_string(x)
        loc_msg = "" if index is _UNSPECIFIED else f" at index {index}"
        if argval is _UNSPECIFIED:
            msg = f"{opname} not found in bytecode{loc_msg}:\n{disassembly}"
        else:
            msg = f"({opname},{argval}) not found in bytecode{loc_msg}:\n{disassembly}"
        self.fail(msg)

    def assertLoadFastInBytecode(
        self, x: Disassembleable, argval: object = _UNSPECIFIED
    ) -> None:
        try:
            self.assertInBytecode(x, "LOAD_FAST", argval)
        except AssertionError:
            self.assertInBytecode(x, "LOAD_FAST_BORROW", argval)

    def assertLoadConstInBytecode(self, x: Disassembleable, const: object) -> None:
        if (
            isinstance(const, int)
            and const >= 0
            and const < 256
            and sys.version_info >= (3, 14)
        ):
            self.assertInBytecode(x, "LOAD_SMALL_INT", const)
        else:
            self.assertInBytecode(x, "LOAD_CONST", const)

    def get_instructions(self, x: Disassembleable) -> Iterable[dis.Instruction]:
        if sys.version_info < (3, 14):
            yield from dis.get_instructions(x)
            return

        extended = False
        for instr in dis.get_instructions(x):
            if instr.opname == "EXTENDED_OPCODE":
                extended = True
                yield instr
            elif extended and instr.opname != "EXTENDED_ARG":
                yield self.make_static_instr(instr, x)
                extended = False
            else:
                yield instr

    def assertNotInBytecode(
        self, x: Disassembleable, opname: str, argval: object = _UNSPECIFIED
    ) -> None:
        """Throws AssertionError if op is found"""
        for instr in self.get_instructions(x):
            if instr.opname == opname:
                disassembly = self.get_disassembly_as_string(x)
                if argval is _UNSPECIFIED:
                    msg = f"{opname} occurs in bytecode:\n{disassembly}"
                    self.fail(msg)
                elif instr.argval == argval:
                    msg = f"({opname},{argval!r}) occurs in bytecode:\n{disassembly}"
                    self.fail(msg)

    def assertLoadMethodInBytecode(self, x: Disassembleable, name: str) -> None:
        if sys.version_info >= (3, 12):
            # We may want to do better here and check the oparg flag in the future.
            self.assertInBytecode(x, "LOAD_ATTR", name)
        else:
            self.assertInBytecode(x, "LOAD_METHOD", name)

    def assertBinOpInBytecode(self, x: Disassembleable, binop: str) -> None:
        if sys.version_info >= (3, 12):
            binop = "NB_" + binop.removeprefix("BINARY_")
            from opcode import _nb_ops

            for i, (name, _sign) in enumerate(_nb_ops):
                if name == binop:
                    self.assertInBytecode(x, "BINARY_OP", i)
                    break
            else:
                self.fail(f"Couldn't find binary op {binop}")
        else:
            self.assertInBytecode(x, binop)

    def assertKwCallInBytecode(self, x: Disassembleable) -> None:
        if sys.version_info >= (3, 12):
            self.assertInBytecode(x, "KW_NAMES")
            self.assertInBytecode(x, "CALL")
        else:
            self.assertInBytecode(x, "CALL_FUNCTION_KW")

    def clean_code(self, code: str) -> str:
        return inspect.cleandoc("\n" + code)

    @property
    def cinder_codegen(self) -> type[CodeGenerator]:
        if sys.version_info >= (3, 14):
            return CodeGenerator314
        elif sys.version_info >= (3, 12):
            return CodeGenerator312

        return CinderCodeGenerator310

    def compile(
        self,
        code: str,
        generator: type[CodeGenerator] | None = None,
        modname: str = "<module>",
        optimize: int = 0,
        ast_optimizer_enabled: bool = True,
    ) -> CodeType:
        gen = make_compiler(
            self.clean_code(code),
            "",
            "exec",
            generator=generator,
            modname=modname,
            optimize=optimize,
            ast_optimizer_enabled=ast_optimizer_enabled,
        )
        assert isinstance(gen, CodeGenerator)
        return gen.getCode()

    def run_code(
        self,
        code: str,
        generator: type[CodeGenerator] | None = None,
        modname: str = "<module>",
    ) -> dict[str, Any]:
        compiled = self.compile(code, generator, modname)
        d = {}
        exec(compiled, d)
        return d

    def find_code(self, code: CodeType, name: str | None = None) -> CodeType:
        consts = [
            const
            for const in code.co_consts
            if isinstance(const, CodeType) and (name is None or const.co_name == name)
        ]
        if len(consts) == 0:
            self.fail("No const available")
        elif len(consts) != 1:
            self.fail(
                "Too many consts: " + ",".join(f"'{code.co_name}'" for code in consts)
            )
        return consts[0]

    def to_graph(
        self,
        code: str,
        generator: type[CodeGenerator] | None = None,
        ast_optimizer_enabled: bool = True,
    ) -> PyFlowGraph:
        code = inspect.cleandoc("\n" + code)
        gen = make_compiler(
            code,
            "",
            "exec",
            generator=generator,
            ast_optimizer_enabled=ast_optimizer_enabled,
        )
        gen.graph.assemble_final_code()
        return gen.graph

    def dump_graph(self, graph: PyFlowGraph) -> str:
        io = StringIO()
        graph.dump(io)
        return io.getvalue()

    def graph_to_instrs(self, graph: PyFlowGraph) -> Generator[Instruction, None, None]:
        for block in graph.getBlocks():
            yield from block.getInstructions()

    def get_consts_with_names(self, code: CodeType) -> dict[str, Any]:
        return dict(zip(code.co_names, code.co_consts))

    def assertNotInGraph(
        self, graph: PyFlowGraph, opname: str, argval: object = _UNSPECIFIED
    ) -> None:
        for block in graph.getBlocks():
            for instr in block.getInstructions():
                if instr.opname == opname:
                    if argval == _UNSPECIFIED:
                        msg = "{} occurs in bytecode:\n{}".format(
                            opname,
                            self.dump_graph(graph),
                        )
                        self.fail(msg)
                    elif instr.oparg == argval:
                        msg = "(%s,%r) occurs in bytecode:\n%s"
                        msg = msg % (opname, argval, self.dump_graph(graph))
                        self.fail(msg)

    def assertInGraph(
        self, graph: PyFlowGraph, opname: str, argval: object = _UNSPECIFIED
    ) -> Instruction | None:
        """Returns instr if op is found, otherwise throws AssertionError"""
        for block in graph.getBlocks():
            for instr in block.getInstructions():
                if instr.opname == opname:
                    if argval is _UNSPECIFIED or instr.oparg == argval:
                        return instr
        disassembly = self.dump_graph(graph)
        if argval is _UNSPECIFIED:
            msg = "{} not found in bytecode:\n{}".format(opname, disassembly)
        else:
            msg = "(%s,%r) not found in bytecode:\n%s"
            msg = msg % (opname, argval, disassembly)
        self.fail(msg)


class _FakeCodeType:
    __slots__ = ("co_exceptiontable",)

    def __init__(self, exc_table: bytes) -> None:
        self.co_exceptiontable = exc_table


@dataclass
class ExceptionTableEntry:
    # Copy dis._ExceptionTableEntry into our own class so the typechecker
    # doesn't complain every time we use it.
    start: int
    end: int
    target: int
    depth: int
    lasti: bool

    @classmethod
    def from_dis_entry(cls, e: object) -> ExceptionTableEntry:
        # pyre-ignore[16]: No visibility into result of dis._parse_exception_table().
        return cls(e.start, e.end, e.target, e.depth, e.lasti)


class ParsedExceptionTable:
    def __init__(self, entries: list[ExceptionTableEntry]) -> None:
        self.entries = entries

    @classmethod
    def from_bytes(cls, exc_table: bytes) -> ParsedExceptionTable:
        code = _FakeCodeType(exc_table)
        # pyre-ignore[16]: Undefined attribute dis._parse_exception_table
        parsed_table = dis._parse_exception_table(code)
        entries = [ExceptionTableEntry.from_dis_entry(e) for e in parsed_table]
        return cls(entries)

    @classmethod
    def from_dis_output(cls, exc_table: str) -> ParsedExceptionTable:
        # Lets us paste dis output directly into test cases.
        lines = [x.strip() for x in exc_table.strip().split("\n")]
        entries = []
        for line in lines:
            if not (m := re.match(DIS_EXC_RE, line)):
                raise ValueError(f"Invalid exception table entry: {line}")
            args = [int(m.group(x)) for x in range(1, 5)] + [bool(m.group(5))]
            # pyre-ignore[6]: for 5th positional argument, expected `bool` but got `int`.
            e = ExceptionTableEntry(*args)
            # NOTE: From the you-have-died-of-dis-entry-dept...
            # When dis displays the exception table entry, it subtracts 2 from
            # e.end (the stored exception table range has an end one opcode further
            # than the actual range, that is, if we have the bytecode
            #     OP1  <handler1>
            #     OP2  <handler1>
            #     OP3  <handler2>
            # then internally handler1's e.end will be the offset of OP3, not
            # of OP2, so dis tweaks the output to reflect the real range).
            #
            # It is not clear why this is not fixed in the exception table code
            # itself, or why dis always subtracts 2 rather than instrsize(OP2),
            # but we need to add it back here. (The other option would be to
            # subtract 2 in ExceptionTableEntry.from_dis_entry(), so that our
            # internal representation matched the dis printed representation.
            # I'm not sure whether that would be more or less confusing in
            # failed test output.)
            e.end = e.end + 2
            entries.append(e)
        return cls(entries)

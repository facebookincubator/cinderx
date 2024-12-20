# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe
import dis
import glob
import inspect
import re
import sys
from dataclasses import dataclass
from io import StringIO
from os import path
from subprocess import run
from types import CodeType
from unittest import TestCase

from cinderx.compiler.pycodegen import (
    CinderCodeGenerator310,
    CodeGenerator,
    CodeGenerator312,
    make_compiler,
)


_UNSPECIFIED = object()

# dis exception table output lines look like:
#   4 to 24 -> 122 [0] lasti
DIS_EXC_RE = re.compile(r"(\d+) to (\d+) -> (\d+) \[(\d+)\]( lasti)?")


def get_repo_root():
    dirname = path.dirname(__file__)
    completed_process = run(
        ["git", "rev-parse", "--show-toplevel"], cwd=dirname, capture_output=True
    )
    if completed_process.returncode:
        print("Error occurred", file=sys.stderr)
        sys.exit(1)
    return completed_process.stdout.strip().decode("utf8")


def glob_test(target_dir, pattern, adder):
    for fname in glob.glob(path.join(target_dir, pattern), recursive=True):
        modname = path.relpath(fname, target_dir)
        adder(modname, fname)


class CompilerTest(TestCase):
    if sys.version_info >= (3, 12):
        SUPER_ATTR = "LOAD_SUPER_ATTR"
        CALL = "CALL"
    else:
        SUPER_ATTR = "LOAD_ATTR_SUPER"
        CALL = "CALL_FUNCTION"

    def get_disassembly_as_string(self, co):
        s = StringIO()
        dis.dis(co, file=s)
        return s.getvalue()

    def assertInBytecode(self, x, opname, argval=_UNSPECIFIED, *, index=_UNSPECIFIED):
        """Returns instr if op is found, otherwise throws AssertionError"""
        for i, instr in enumerate(dis.get_instructions(x)):
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

    def assertNotInBytecode(self, x, opname, argval=_UNSPECIFIED):
        """Throws AssertionError if op is found"""
        for instr in dis.get_instructions(x):
            if instr.opname == opname:
                disassembly = self.get_disassembly_as_string(x)
                if argval is _UNSPECIFIED:
                    msg = f"{opname} occurs in bytecode:\n{disassembly}"
                    self.fail(msg)
                elif instr.argval == argval:
                    msg = f"({opname},{argval!r}) occurs in bytecode:\n{disassembly}"
                    self.fail(msg)

    def assertLoadMethodInBytecode(self, x, name: str) -> None:
        if sys.version_info >= (3, 12):
            # We may want to do better here and check the oparg flag in the future.
            self.assertInBytecode(x, "LOAD_ATTR", name)
        else:
            self.assertInBytecode(x, "LOAD_METHOD", name)

    def assertBinOpInBytecode(self, x, binop: str) -> None:
        if sys.version_info >= (3, 12):
            binop = "NB_" + binop.removeprefix("BINARY_")
            # pyre-ignore[21]: Undefined attribute
            from opcode import _nb_ops

            # pyre-ignore[16]: Undefined attribute
            for i, (name, sign) in enumerate(_nb_ops):
                if name == binop:
                    self.assertInBytecode(x, "BINARY_OP", i)
                    break
            else:
                self.fail(f"Couldn't find binary op {binop}")
        else:
            self.assertInBytecode(x, binop)

    def clean_code(self, code: str) -> str:
        return inspect.cleandoc("\n" + code)

    @property
    def cinder_codegen(self) -> type[CodeGenerator]:
        if sys.version_info >= (3, 12):
            return CodeGenerator312

        return CinderCodeGenerator310

    def compile(
        self,
        code,
        generator=None,
        modname="<module>",
        optimize=0,
        ast_optimizer_enabled=True,
    ):
        gen = make_compiler(
            self.clean_code(code),
            "",
            "exec",
            generator=generator,
            modname=modname,
            optimize=optimize,
            ast_optimizer_enabled=ast_optimizer_enabled,
        )
        return gen.getCode()

    def run_code(self, code, generator=None, modname="<module>"):
        compiled = self.compile(
            code,
            generator,
            modname,
        )
        d = {}
        exec(compiled, d)
        return d

    def find_code(self, code, name=None):
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

    def to_graph(self, code, generator=None, ast_optimizer_enabled=True):
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

    def dump_graph(self, graph):
        io = StringIO()
        graph.dump(io)
        return io.getvalue()

    def graph_to_instrs(self, graph):
        for block in graph.getBlocks():
            yield from block.getInstructions()

    def get_consts_with_names(self, code):
        return {k: v for k, v in zip(code.co_names, code.co_consts)}

    def assertNotInGraph(self, graph, opname, argval=_UNSPECIFIED):
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

    def assertInGraph(self, graph, opname, argval=_UNSPECIFIED):
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
    def __init__(self, exc_table):
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
    def from_dis_entry(cls, e):
        return cls(e.start, e.end, e.target, e.depth, e.lasti)


class ParsedExceptionTable:
    def __init__(self, entries: list[ExceptionTableEntry]):
        self.entries = entries

    @classmethod
    def from_bytes(cls, exc_table: bytes):
        code = _FakeCodeType(exc_table)
        # pyre-ignore[16]: Undefined attribute dis._parse_exception_table
        parsed_table = dis._parse_exception_table(code)
        entries = [ExceptionTableEntry.from_dis_entry(e) for e in parsed_table]
        return cls(entries)

    @classmethod
    def from_dis_output(cls, exc_table: str):
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

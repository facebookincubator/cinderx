# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe
from __future__ import annotations

import dis
import inspect
import os
import sys
from collections import defaultdict, deque
from functools import partial
from io import SEEK_END, TextIOBase
from re import escape
from typing import Generator, Iterable, Sequence

from cinderx.compiler import pyassem
from cinderx.compiler.pyassem import Instruction, PyFlowGraph312
from cinderx.compiler.pycodegen import CodeGenerator

from .common import CompilerTest, glob_test, ParsedExceptionTable


def format_oparg(instr: Instruction) -> str:
    if instr.target is not None:
        if instr.target.label:
            return f"Block({instr.target.bid}, label={instr.target.label!r})"
        return f"Block({instr.target.bid})"
    elif isinstance(instr.oparg, CodeGenerator):
        # pyre-fixme[16]: `AST` has no attribute `lineno`.
        # pyre-fixme[16]: `AST` has no attribute `col_offset`.
        return f"Code(({instr.oparg.tree.lineno},{instr.oparg.tree.col_offset}))"
    elif isinstance(instr.oparg, (str, int, tuple, frozenset, type(None))):
        return repr(instr.oparg)

    raise NotImplementedError("Unsupported oparg type: " + type(instr.oparg).__name__)


DEFAULT_MARKER = "# This is the default script and should be updated to the minimal byte codes to be verified"
SCRIPT_EXPECTED = "# EXPECTED:"
SCRIPT_EXC_TABLE = "# EXCEPTION_TABLE:"
SCRIPT_EXT = ".scr.py"
SCRIPT_OPCODE_CODE = "CODE_START"
BLOCK_START_CODE = "__BLOCK__"


class _AnyType:
    """Singleton to indicate that we match any opcode or oparg."""

    def __new__(cls) -> _AnyType:
        return Any

    def __repr__(self) -> str:
        return "Any"


Any: _AnyType = object.__new__(_AnyType)


def is_oparg_equal(test: CodeTests, expected: object, instr: Instruction) -> bool:
    if instr.target:
        return instr.target == expected
    else:
        return instr.oparg == expected


def is_instr_match(test, opname: str | _AnyType, oparg, instr) -> bool:
    if opname != Any and opname != instr.opname:
        return False

    if oparg != Any and not is_oparg_equal(test, oparg, instr):
        return False

    return True


def graph_instrs(graph, name=None) -> Generator[Instruction, None, None]:
    if name:
        yield Instruction(SCRIPT_OPCODE_CODE, name)
    for block in graph.getBlocks():
        yield Instruction(BLOCK_START_CODE, None, target=block)
        yield from block.getInstructions()


class TestFile:
    """Parsed test case file."""

    _SECTION_HEADERS = {SCRIPT_EXPECTED: "expected", SCRIPT_EXC_TABLE: "exc_table"}

    def __init__(self, fname: str) -> None:
        self.fname: str = fname
        self.code: str = ""
        self.expected: str | None = None
        self.exc_table: str | None = None
        self._read_test_file()

    def _read_test_file(self):
        with open(self.fname) as f:
            lines = f.readlines()

        sections = defaultdict(list)
        section = "code"
        for line in lines:
            line = line.rstrip()
            if k := self._SECTION_HEADERS.get(line):
                section = k
            else:
                sections[section].append(line)

        self.code = "\n".join(sections["code"])
        for section in self._SECTION_HEADERS.values():
            if section in sections:
                setattr(self, section, "\n".join(sections[section]))

    def write_default_expected(self, instrs):
        """Fill in the EXPECTED section of the file with a default script."""

        with open(self.fname) as f:
            text = f.read()
            has_nl = text.endswith("\n")

        with open(self.fname, "a") as f:
            f.seek(0, SEEK_END)
            if not has_nl:
                f.write("\n")
            self._gen_default_script(f, instrs)

    def _gen_default_script(
        self, scr: TextIOBase, instrs: Iterable[Instruction]
    ) -> None:
        # generate a default script which matches exactly...  This should
        # be fixed up to a minimal match
        scr.write(SCRIPT_EXPECTED + "\n")
        scr.write(DEFAULT_MARKER + "\n")
        scr.write("[\n")
        queue: deque[Iterable[Instruction]] = deque([instrs])
        first = True
        while queue:
            instrs = queue.popleft()
            for instr in instrs:
                if isinstance(instr.oparg, CodeGenerator):
                    queue.append(
                        graph_instrs(instr.oparg.graph, instr.oparg.graph.name)
                    )
                if instr.opname == BLOCK_START_CODE:
                    target = instr.target
                    assert target is not None
                    if not first:
                        scr.write("\n")
                    scr.write(
                        f"    # {instr.opname}('{target.label}: {target.bid}'),\n"
                    )
                else:
                    scr.write(f"    {instr.opname}({format_oparg(instr)}),\n")
                first = False
        scr.write("]\n")


class CodeTests(CompilerTest):
    def check_instrs(
        self, instrs: Iterable[Instruction], script: Sequence[Matcher]
    ) -> None:
        if not script:
            self.fail("Script file is empty")

        cur_scr = 0
        queue = deque([instrs])
        while queue:
            instrs = tuple(queue.popleft())
            for i, instr in enumerate(instrs):
                if isinstance(instr.oparg, CodeGenerator):
                    queue.append(
                        graph_instrs(instr.oparg.graph, instr.oparg.graph.name)
                    )
                if cur_scr == len(script):
                    self.fail("Extra bytecodes not expected")

                op = script[cur_scr]
                inc, error = op.is_match(self, instrs, i, script, cur_scr)
                if error:
                    # Ignore block starts, they're optional in the script.
                    if instr.opname != BLOCK_START_CODE:
                        self.fail(error)

                cur_scr += inc

        # make sure we exhausted the script or ended on a ...
        if cur_scr == len(script):
            return

        script[cur_scr].end(self, script, cur_scr)

    def check_exc_table(self, graph: PyFlowGraph312, fn_name: str, table: str):
        actual = None  # keep pyre happy
        if fn_name == "":
            actual = graph.exception_table
        else:
            # We have already converted function graphs to bytecode, so we need
            # to check consts for code objects.
            # TODO(T190611021): Handle methods and nested functions.
            for _, obj in graph.consts:
                if inspect.iscode(obj) and obj.co_name == fn_name:
                    # pyre-ignore[16]: `types.CodeType` has no attribute `co_exceptiontable`
                    actual = obj.co_exceptiontable
                    break
            else:
                raise ValueError(f"function {fn_name} not found in code")
        assert actual is not None
        actual_exc = ParsedExceptionTable.from_bytes(actual)
        expected_exc = ParsedExceptionTable.from_dis_output(table)
        self.assertEqual(actual_exc.entries, expected_exc.entries)

    def test_self_empty_script(self) -> None:
        with self.assertRaises(AssertionError):
            self.check_instrs([], [])

    def test_self_match_all(self) -> None:
        self.check_instrs([], [SkipAny()])
        self.check_instrs([Instruction("STORE_NAME", "foo")], [SkipAny()])
        self.check_instrs(
            [Instruction("STORE_NAME", "foo"), Instruction("STORE_NAME", "foo")],
            [SkipAny()],
        )

    def test_self_trailing_instrs(self) -> None:
        with self.assertRaisesRegex(AssertionError, "Extra bytecodes not expected"):
            self.check_instrs(
                [Instruction("STORE_NAME", "foo"), Instruction("STORE_NAME", "foo")],
                [Op("STORE_NAME", "foo")],
            )

    def test_self_bad_oparg(self) -> None:
        return
        with self.assertRaisesRegex(
            AssertionError, "Failed to eval expected oparg: foo"
        ):
            self.check_instrs(
                [Instruction("STORE_NAME", "foo")], [Op("STORE_NAME", "foo")]
            )

    def test_self_trailing_script_wildcard(self) -> None:
        with self.assertRaisesRegex(
            AssertionError, escape("Trailing script elements: [Op(STORE_NAME, 'bar')]")
        ):
            self.check_instrs(
                [Instruction("STORE_NAME", "foo"), Instruction("STORE_NAME", "foo")],
                [SkipAny(), Op("STORE_NAME", "bar")],
            )

    def test_self_trailing_script(self) -> None:
        with self.assertRaisesRegex(
            AssertionError, escape("Trailing script elements: [Op(STORE_NAME, 'bar')]")
        ):
            self.check_instrs(
                [Instruction("STORE_NAME", "foo")],
                [Op("STORE_NAME", "foo"), Op("STORE_NAME", "bar")],
            )

    def test_self_mismatch_oparg(self) -> None:
        with self.assertRaises(AssertionError):
            self.check_instrs(
                [Instruction("STORE_NAME", "foo")], [Op("STORE_NAME", "bar")]
            )

    def test_self_mismatch_opname(self) -> None:
        with self.assertRaises(AssertionError):
            self.check_instrs(
                [Instruction("LOAD_NAME", "foo")], [Op("STORE_NAME", "foo")]
            )

    def test_self_wildcard_oparg(self) -> None:
        self.check_instrs([Instruction("LOAD_NAME", "foo")], [Op("LOAD_NAME", Any)])

    def test_self_wildcard_opname(self) -> None:
        self.check_instrs([Instruction("LOAD_NAME", "foo")], [Op(Any, "foo")])

    def test_self_match_after_wildcard(self) -> None:
        self.check_instrs(
            [Instruction("LOAD_NAME", "foo"), Instruction("CALL_FUNCTION", 0)],
            [SkipAny(), Op("CALL_FUNCTION", 0)],
        )

    def test_self_match_block(self) -> None:
        block = pyassem.Block()
        block.bid = 1
        self.check_instrs(
            [
                Instruction("JUMP_ABSOLUTE", None, target=block),
                Instruction("LOAD_CONST", None),
                Instruction("RETURN_VALUE", 0),
            ],
            [Op("JUMP_ABSOLUTE", Block(1)), Op("LOAD_CONST", None), SkipAny()],
        )

    def test_self_match_wrong_block(self) -> None:
        block = pyassem.Block()
        block.bid = 1
        with self.assertRaisesRegex(
            AssertionError,
            escape(
                "mismatch, expected JUMP_ABSOLUTE Block(2), got JUMP_ABSOLUTE Block(1)"
            ),
        ):
            self.check_instrs(
                [
                    Instruction("JUMP_ABSOLUTE", None, target=block),
                    Instruction("LOAD_CONST", None),
                    Instruction("RETURN_VALUE", 0),
                ],
                [Op("JUMP_ABSOLUTE", Block(2)), Op("LOAD_CONST", None), SkipAny()],
            )

    def test_self_neg_wildcard_match(self) -> None:
        with self.assertRaisesRegex(
            AssertionError, "unexpected match LOAD_NAME foo, got LOAD_NAME 'foo'"
        ):
            self.check_instrs(
                [Instruction("LOAD_NAME", "foo"), Instruction("CALL_FUNCTION", 0)],
                [~Op("LOAD_NAME", "foo")],
            )

    def test_self_neg_wildcard_no_match(self) -> None:
        self.check_instrs(
            [Instruction("LOAD_NAME", "foo"), Instruction("CALL_FUNCTION", 0)],
            [~Op("LOAD_NAME", "bar")],
        )

    def test_self_neg_wildcard_multimatch(self) -> None:
        with self.assertRaisesRegex(
            AssertionError, "unexpected match CALL_FUNCTION 0, got CALL_FUNCTION 0"
        ):
            self.check_instrs(
                [Instruction("LOAD_NAME", "foo"), Instruction("CALL_FUNCTION", 0)],
                [SkipBut(Op("LOAD_NAME", "bar"), Op("CALL_FUNCTION", 0))],
            )

    def test_self_neg_wildcard_no_multimatch(self) -> None:
        self.check_instrs(
            [Instruction("LOAD_NAME", "foo"), Instruction("CALL_FUNCTION", 0)],
            [SkipBut(Op("LOAD_NAME", "bar"), Op("CALL_FUNCTION", 1))],
        )


def add_test(modname: str, fname: str) -> None:
    version = sys.version_info[:2]
    if "/cinder/" in fname and "cinder" not in sys.version:
        return
    elif "/3.10/" in fname and version != (3, 10):
        return
    elif "/3.12/" in fname and version != (3, 12):
        return
    if fname.endswith("/__init__.py"):
        return

    def test_code(self: CodeTests):
        test = TestFile(fname)
        graph = self.to_graph(test.code)

        fixme = "fixup to be a minimal repro and check it in"
        if test.expected is None:
            test.write_default_expected(graph_instrs(graph))
            self.fail(f"test script not present, script generated, {fixme}")
        elif DEFAULT_MARKER in test.expected:
            self.fail(f"generated default script marker present, {fixme}")

        # Parse expected bytecode
        script = eval(test.expected, globals(), SCRIPT_CONTEXT)
        transformed = [SkipAny() if value == ... else value for value in script]
        self.check_instrs(graph_instrs(graph), transformed)

        # Parse expected exception table
        exc_table = test.exc_table
        if exc_table is not None:
            if version < (3, 11):
                self.fail("No exception table in python 3.10")
            exc = eval(exc_table)
            for fn, table in exc.items():
                self.check_exc_table(graph, fn, table)

    # pyre-ignore[16]: Callable has no attribute __name__
    test_code.__name__ = "test_" + modname.replace("/", "_").replace(".", "_")[:-3]
    setattr(CodeTests, test_code.__name__, test_code)


glob_test(
    os.path.join(os.path.dirname(__file__), "sbs_code_tests"), "**/*.py", add_test
)


class Matcher:
    def is_match(
        self,
        test: CodeTests,
        instrs: Sequence[Instruction],
        cur_instr: int,
        script: Sequence[Matcher],
        cur_scr: int,
    ) -> tuple[int, str | None]:
        raise NotImplementedError()

    def end(self, test, script: Sequence[Matcher], cur_scr) -> None:
        test.fail(f"Trailing script elements: {script[cur_scr:]}")

    def __invert__(self):
        return SkipBut(self)


class Op(Matcher):
    """Matches a single opcode.  Can be used as:
    OPNAME([oparg])       - OPNAME is injected into eval namespace as a callable for all known opcodes.
    ANY([oparg])          - Match any opcode that matches the given oparg.
    Op(OPNAME[, oparg])   - Op will recognize the callable of a known opcode.
    Op('OPNAME'[, oparg]) - Explicitly provide opcode name as str (can match opcodes not defined in dis)
    Op(Any[, oparg])      - Match any opcode with the given oparg.

    If oparg is not provided then the oparg will not be checked."""

    def __init__(self, opname: str | _AnyType = Any, oparg=_AnyType | object) -> None:
        if isinstance(opname, partial) and issubclass(opname.func, Op):
            # Op(LOAD_NAME, "foo") form instead of
            # LOAD_NAME("foo")
            opname = opname.args[0]
        self.opname = opname
        self.oparg = oparg

    def is_match(
        self,
        test: CodeTests,
        instrs: Sequence[Instruction],
        cur_instr: int,
        script: Sequence[Matcher],
        cur_scr: int,
    ) -> tuple[int, str | None]:
        instr = instrs[cur_instr]

        if is_instr_match(test, self.opname, self.oparg, instr):
            return 1, None

        return (
            0,
            f"mismatch, expected {self.opname} {self.oparg}, got {instr.opname} {format_oparg(instr)}",
        )

    def __repr__(self) -> str:
        if self.oparg == Any:
            return f"Op({self.opname})"
        return f"Op({self.opname}, {self.oparg!r})"

    def __str__(self) -> str:
        return f"{self.opname} {self.oparg}"


CODE_START = partial(Op, SCRIPT_OPCODE_CODE)


class Block:
    """Matches an oparg with block target"""

    def __init__(self, bid: int, label: str | None = None) -> None:
        self.bid = bid
        self.label = label

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, pyassem.Block):
            return False

        return other.bid == self.bid

    def __repr__(self) -> str:
        if self.label:
            return f"Block({self.bid}, label={self.label})"
        return f"Block({self.bid})"


class Code:
    def __init__(self, loc) -> None:
        if not isinstance(loc, (str, tuple, int)):
            raise TypeError("expected code name, line/offset tuple, or line no")
        self.loc = loc

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, CodeGenerator):
            return False

        if isinstance(self.loc, str):
            return self.loc == other.name
        elif isinstance(self.loc, tuple):
            # pyre-fixme[16]: `AST` has no attribute `lineno`.
            # pyre-fixme[16]: `AST` has no attribute `col_offset`.
            return self.loc == (other.tree.lineno, other.tree.col_offset)
        elif isinstance(self.loc, int):
            return self.loc == other.tree.lineno
        return False

    def __repr__(self) -> str:
        return f"Code({self.loc})"


class SkipAny(Matcher):
    """Skips any number of instructions until the next instruction matches. Can
    be used explicitly via SkipAny() or short hand usage of ... is supported.

    Skipping terminates when the next instrution matches."""

    def is_match(
        self,
        test: CodeTests,
        instrs: Sequence[Instruction],
        cur_instr: int,
        script: Sequence[Matcher],
        cur_scr: int,
    ) -> tuple[int, str | None]:
        if cur_scr + 1 != len(script):
            next = script[cur_scr + 1]
            inc, error = next.is_match(test, instrs, cur_instr, script, cur_scr + 1)
            if not error:
                # skip the ... and the match
                return inc + 1, None
            return inc, None
        return 0, None

    def end(self, test, script, cur_scr) -> None:
        if cur_scr != len(script) - 1:
            script[cur_scr + 1].end(test, script, cur_scr + 1)

    def __repr__(self) -> str:
        return "SkipAny()"


class SkipBut(SkipAny):
    """Skips instructions making sure there are no matches against 1 or more
    instructions along the way.  Can be used as:
        ~OPNAME()           - Asserts one opcode isn't present
        SkipBut(
            OPNAME(...),
            OPNAME(...)
        )                   - Asserts multiple instrutions aren't present.

    Skipping terminates when the next instruction matches."""

    def __init__(self, *args) -> None:
        self.args = args

    def is_match(
        self,
        test: CodeTests,
        instrs: Sequence[Instruction],
        cur_instr: int,
        script: Sequence[Matcher],
        cur_scr: int,
    ) -> tuple[int, str | None]:
        instr = instrs[cur_instr]
        for arg in self.args:
            inc, err = arg.is_match(test, instrs, cur_instr, script, cur_scr)
            if not err:
                return (
                    0,
                    f"unexpected match {arg}, got {instr.opname} {format_oparg(instr)}",
                )

        return super().is_match(test, instrs, cur_instr, script, cur_scr)

    def __repr__(self) -> str:
        return f"SkipBut(*{self.args!r})"


class BlockStart(Matcher):
    def __init__(self, name: str) -> None:
        self.name = name

    def __repr__(self) -> str:
        return f"BLOCK_START({self.name!r})"

    def is_match(
        self,
        test: CodeTests,
        instrs: Sequence[Instruction],
        cur_instr: int,
        script: Sequence[Matcher],
        cur_scr: int,
    ) -> tuple[int, str | None]:
        instr = instrs[cur_instr]
        if instr.opname == BLOCK_START_CODE:
            label, _, bid = self.name.rpartition(": ")
            target = instr.target
            assert target is not None
            if target.label == label and target.bid == int(bid):
                return 1, None

        return (
            0,
            f"mismatch, expected BLOCK_START({self.name!r}), got {instr.opname} {format_oparg(instr)}",
        )


SCRIPT_CONTEXT = {
    "ANY": partial(Op, Any),
    BLOCK_START_CODE: BlockStart,
    **{opname: partial(Op, opname) for opname in dis.opmap},
}

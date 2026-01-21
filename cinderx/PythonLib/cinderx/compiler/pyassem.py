# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

"""A flow graph representation for Python bytecode"""

from __future__ import annotations

import sys
from ast import AST
from collections import defaultdict
from contextlib import contextmanager, redirect_stdout
from dataclasses import dataclass
from enum import IntEnum, IntFlag

try:
    # pyre-ignore[21]: No _inline_cache_entries
    from opcode import _inline_cache_entries
except ImportError:
    _inline_cache_entries = None
from types import CodeType
from typing import (
    Callable,
    ClassVar,
    Generator,
    Iterable,
    Iterator,
    Optional,
    Sequence,
    TextIO,
    TypeAlias,
    TypeVar,
)

from .consts import (
    CO_ASYNC_GENERATOR,
    CO_COROUTINE,
    CO_GENERATOR,
    CO_NEWLOCALS,
    CO_OPTIMIZED,
    CO_SUPPRESS_JIT,
)
from .debug import dump_graph
from .flow_graph_optimizer import (
    FlowGraphConstOptimizer314,
    FlowGraphOptimizer,
    FlowGraphOptimizer310,
    FlowGraphOptimizer312,
    FlowGraphOptimizer314,
)
from .opcode_cinder import opcode as cinder_opcode
from .opcodebase import Opcode
from .opcodes import opcode as opcodes_opcode, STATIC_OPCODES
from .symbols import ClassScope, Scope


MAX_COPY_SIZE = 4


class ResumeOparg(IntFlag):
    ScopeEntry = 0
    Yield = 1
    YieldFrom = 2
    Await = 3

    LocationMask = 0x03
    Depth1Mask = 0x04


def sign(a: float) -> float:
    if not isinstance(a, float):
        raise TypeError(f"Must be a real number, not {type(a)}")
    if a != a:
        return 1.0  # NaN case
    return 1.0 if str(a)[0] != "-" else -1.0


def cast_signed_byte_to_unsigned(i: int) -> int:
    if i < 0:
        i = 255 + i + 1
    return i


FVC_MASK = 0x3
FVC_NONE = 0x0
FVC_STR = 0x1
FVC_REPR = 0x2
FVC_ASCII = 0x3
FVS_MASK = 0x4
FVS_HAVE_SPEC = 0x4

NO_INPUT_INSTRS = {
    "FORMAT_SIMPLE",
    "GET_ANEXT",
    "GET_LEN",
    "GET_YIELD_FROM_ITER",
    "IMPORT_FROM",
    "MATCH_KEYS",
    "MATCH_MAPPING",
    "MATCH_SEQUENCE",
    "WITH_EXCEPT_START",
}
if sys.version_info >= (3, 15):
    NO_INPUT_INSTRS.add("GET_ITER")

UNCONDITIONAL_JUMP_OPCODES = (
    "JUMP_ABSOLUTE",
    "JUMP_FORWARD",
    "JUMP",
    "JUMP_BACKWARD",
    "JUMP_NO_INTERRUPT",
    "JUMP_BACKWARD_NO_INTERRUPT",
)


# TASK(T128853358): The RETURN_PRIMITIVE logic should live in the Static flow graph.
RETURN_OPCODES = ("RETURN_VALUE", "RETURN_CONST", "RETURN_PRIMITIVE")


# TASK(T128853358): The RETURN_PRIMITIVE logic should live in the Static flow graph.
SCOPE_EXIT_OPCODES = (
    "RETURN_VALUE",
    "RETURN_CONST",
    "RETURN_PRIMITIVE",
    "RAISE_VARARGS",
    "RERAISE",
)

SETUP_OPCODES = ("SETUP_ASYNC_WITH", "SETUP_FINALLY", "SETUP_WITH", "SETUP_CLEANUP")


@dataclass(frozen=True, slots=True)
class SrcLocation:
    lineno: int
    end_lineno: int
    col_offset: int
    end_col_offset: int

    def __repr__(self) -> str:
        return f"SrcLocation({self.lineno}, {self.end_lineno}, {self.col_offset}, {self.end_col_offset})"


NO_LOCATION = SrcLocation(-1, -1, -1, -1)
NEXT_LOCATION = SrcLocation(-2, -2, -2, -2)


class Instruction:
    __slots__ = ("opname", "oparg", "target", "ioparg", "loc", "exc_handler")

    def __init__(
        self,
        opname: str,
        oparg: object,
        ioparg: int = 0,
        loc: AST | SrcLocation = NO_LOCATION,
        target: Block | None = None,
        exc_handler: Block | None = None,
    ) -> None:
        self.opname = opname
        self.oparg = oparg
        self.loc = loc
        self.ioparg = ioparg
        self.target = target
        self.exc_handler = exc_handler

    @property
    def lineno(self) -> int:
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
        return self.loc.lineno

    def __repr__(self) -> str:
        args = [
            f"{self.opname!r}",
            f"{self.oparg!r}",
            f"{self.ioparg!r}",
            f"{self.loc!r}",
        ]
        if self.target is not None:
            args.append(f"{self.target!r}")
        if self.exc_handler is not None:
            args.append(f"{self.exc_handler!r}")

        return f"Instruction({', '.join(args)})"

    def is_jump(self, opcode: Opcode) -> bool:
        op = opcode.opmap[self.opname]
        return opcode.has_jump(op)

    def set_to_nop(self) -> None:
        self.opname = "NOP"
        self.oparg = self.ioparg = 0
        self.target = None

    def set_to_nop_no_loc(self) -> None:
        self.opname = "NOP"
        self.oparg = self.ioparg = 0
        self.target = None
        self.loc = NO_LOCATION

    @property
    def stores_to(self) -> str | None:
        if self.opname == "STORE_FAST" or self.opname == "STORE_FAST_MAYBE_NULL":
            assert isinstance(self.oparg, str)
            return self.oparg
        return None

    def copy(self) -> Instruction:
        return Instruction(
            self.opname,
            self.oparg,
            self.ioparg,
            self.loc,
            self.target,
            self.exc_handler,
        )


class CompileScope:
    START_MARKER = "compile-scope-start-marker"
    __slots__ = "blocks"

    def __init__(self, blocks: list[Block]) -> None:
        self.blocks: list[Block] = blocks


class FlowGraph:
    def __init__(self) -> None:
        self.next_block_id = 0
        # List of blocks in the order they should be output for linear
        # code. As we deal with structured code, this order corresponds
        # to the order of source level constructs. (The original
        # implementation from Python2 used a complex ordering algorithm
        # which more buggy and erratic than useful.)
        self.ordered_blocks: list[Block] = []
        # Current block being filled in with instructions.
        self.current: Block | None = None
        self.entry = Block("entry")
        self.startBlock(self.entry)

        # Source line number to use for next instruction.
        self.loc: AST | SrcLocation = SrcLocation(0, 0, 0, 0)
        # First line of this code block. This field is expected to be set
        # externally and serve as a reference for generating all other
        # line numbers in the code block. (If it's not set, it will be
        # deduced).
        self.firstline = 0
        # Line number of first instruction output. Used to deduce .firstline
        # if it's not set explicitly.
        self.first_inst_lineno = 0
        # If non-zero, do not emit bytecode
        self.do_not_emit_bytecode = 0
        self.annotations_block: Block | None = None

    def fetch_current(self) -> Block:
        current = self.current
        assert current
        return current

    def get_new_block_id(self) -> int:
        ret = self.next_block_id
        self.next_block_id += 1
        return ret

    def blocks_in_reverse_allocation_order(self) -> Iterable[Block]:
        yield from sorted(self.ordered_blocks, key=lambda b: b.alloc_id, reverse=True)

    @contextmanager
    def new_compile_scope(self) -> Generator[CompileScope, None, None]:
        prev_current = self.current
        prev_ordered_blocks = self.ordered_blocks
        prev_line_no = self.first_inst_lineno
        try:
            self.ordered_blocks = []
            self.current = self.newBlock(CompileScope.START_MARKER)
            yield CompileScope(self.ordered_blocks)
        finally:
            self.current = prev_current
            self.ordered_blocks = prev_ordered_blocks
            self.first_inst_lineno = prev_line_no

    def apply_from_scope(self, scope: CompileScope) -> None:
        # link current block with the block from out of order result
        block: Block = scope.blocks[0]
        assert block.prev is not None
        assert block.prev.label == CompileScope.START_MARKER
        block.prev = None

        self.fetch_current().connect_next(block)
        self.ordered_blocks.extend(scope.blocks)
        self.current = scope.blocks[-1]

    def startBlock(self, block: Block) -> None:
        """Add `block` to ordered_blocks and set it as the current block."""
        if self._debug:
            current = self.current
            if current:
                print("end", repr(current))
                print("    next", current.next)
                print("    prev", current.prev)
                print("   ", current.get_outgoing())
            print(repr(block))
        block.bid = self.get_new_block_id()
        assert block not in self.ordered_blocks
        self.ordered_blocks.append(block)
        self.current = block

    def nextBlock(self, block: Block | None = None, label: str = "") -> None:
        """Connect `block` as current.next, then set it as the new `current`

        Create a new block if needed.
        """
        if self.do_not_emit_bytecode:
            return
        # XXX think we need to specify when there is implicit transfer
        # from one block to the next.  might be better to represent this
        # with explicit JUMP_ABSOLUTE instructions that are optimized
        # out when they are unnecessary.
        #
        # I think this strategy works: each block has a child
        # designated as "next" which is returned as the last of the
        # children.  because the nodes in a graph are emitted in
        # reverse post order, the "next" block will always be emitted
        # immediately after its parent.
        # Worry: maintaining this invariant could be tricky
        if block is None:
            block = self.newBlock(label=label)

        # Note: If the current block ends with an unconditional control
        # transfer, then it is technically incorrect to add an implicit
        # transfer to the block graph. Doing so results in code generation
        # for unreachable blocks.  That doesn't appear to be very common
        # with Python code and since the built-in compiler doesn't optimize
        # it out we don't either.
        self.fetch_current().connect_next(block)
        self.startBlock(block)

    def newBlock(self, label: str = "") -> Block:
        """Creates a new Block object, but does not add it to the graph."""
        return Block(label)

    _debug = 0

    def _enable_debug(self) -> None:
        self._debug = 1

    def _disable_debug(self) -> None:
        self._debug = 0

    def emit_with_loc(self, opcode: str, oparg: object, loc: AST | SrcLocation) -> None:
        if isinstance(oparg, Block):
            if not self.do_not_emit_bytecode:
                current = self.fetch_current()
                current.add_out_edge(oparg)
                current.emit(Instruction(opcode, 0, 0, loc, target=oparg))
            oparg.is_jump_target = True
            return

        ioparg = self.convertArg(opcode, oparg)

        if not self.do_not_emit_bytecode:
            self.fetch_current().emit(Instruction(opcode, oparg, ioparg, loc))

    def emit(self, opcode: str, oparg: object = 0) -> None:
        self.emit_with_loc(opcode, oparg, self.loc)

    def emit_noline(self, opcode: str, oparg: object = 0) -> None:
        self.emit_with_loc(opcode, oparg, NO_LOCATION)

    def emitWithBlock(self, opcode: str, oparg: object, target: Block) -> None:
        if not self.do_not_emit_bytecode:
            current = self.fetch_current()
            current.add_out_edge(target)
            current.emit(Instruction(opcode, oparg, target=target))

    def set_pos(self, node: AST | SrcLocation) -> None:
        if not self.first_inst_lineno:
            # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
            #  `lineno`.
            self.first_inst_lineno = node.lineno
        self.loc = node

    def convertArg(self, opcode: str, oparg: object) -> int:
        if isinstance(oparg, int):
            return oparg
        raise ValueError(f"invalid oparg {oparg!r} for {opcode!r}")

    def getBlocksInOrder(self) -> list[Block]:
        """Return the blocks in the order they should be output."""
        return self.ordered_blocks

    def getBlocks(self) -> list[Block]:
        return self.ordered_blocks

    def getRoot(self) -> Block:
        """Return nodes appropriate for use with dominator"""
        return self.entry

    def getContainedGraphs(self) -> list[PyFlowGraph]:
        result = []
        for b in self.getBlocks():
            result.extend(b.getContainedGraphs())
        return result


class Block:
    allocated_block_count: ClassVar[int] = 0

    def __init__(self, label: str = "") -> None:
        self.insts: list[Instruction] = []
        self.out_edges: set[Block] = set()
        self.label: str = label
        self.is_jump_target = False
        self.bid: int | None = None  # corresponds to b_label.id in cpython
        self.next: Block | None = None
        self.prev: Block | None = None
        self.returns: bool = False
        self.offset: int = 0
        self.is_exc_handler: bool = False  # used in reachability computations
        self.preserve_lasti: bool = False  # used if block is an exception handler
        self.seen: bool = False  # visited during stack depth calculation
        self.startdepth: int = -1
        self.is_exit: bool = False
        self.has_fallthrough: bool = True
        self.num_predecessors: int = 0
        self.alloc_id: int = Block.allocated_block_count
        # for 3.12
        self.except_stack: list[Block] = []

        Block.allocated_block_count += 1

    def __repr__(self) -> str:
        data = []
        data.append(f"id={self.bid}")
        data.append(f"startdepth={self.startdepth}")
        if self.next:
            data.append(f"next={self.next.bid}")
        if self.is_exc_handler:
            data.append("EH")
        extras = ", ".join(data)
        if self.label:
            return f"<block {self.label} {extras}>"
        else:
            return f"<block {extras}>"

    def __str__(self) -> str:
        insts = map(str, self.insts)
        insts = "\n".join(insts)
        return f"<block label={self.label} bid={self.bid} startdepth={self.startdepth}: {insts}>"

    def emit(self, instr: Instruction) -> None:
        if instr.opname in RETURN_OPCODES:
            self.returns = True

        self.insts.append(instr)

    def getInstructions(self) -> list[Instruction]:
        return self.insts

    def add_out_edge(self, block: Block) -> None:
        self.out_edges.add(block)

    def connect_next(self, block: Block) -> None:
        """Connect `block` as self.next."""
        assert self.next is None, self.next
        self.next = block
        assert block.prev is None, block.prev
        block.prev = self

    def insert_next(self, block: Block) -> None:
        """Insert `block` as self.next in the linked list."""
        assert block.prev is None, block.prev
        block.next = self.next
        block.prev = self
        self.next = block

    def get_outgoing(self) -> list[Block]:
        """Get the list of blocks this block can transfer control to."""
        return list(self.out_edges) + ([self.next] if self.next is not None else [])

    def getContainedGraphs(self) -> list[PyFlowGraph]:
        """Return all graphs contained within this block.

        For example, a MAKE_FUNCTION block will contain a reference to
        the graph for the function body.
        """
        contained = []
        for inst in self.insts:
            # pyre-fixme[6] It is not clear what the type of inst is.
            if len(inst) == 1:
                continue

            # pyre-fixme[16] It is not clear what the type of inst is.
            op = inst[1]
            if hasattr(op, "graph"):
                contained.append(op.graph)
        return contained

    def append_instr(
        self,
        opname: str,
        oparg: object,
        ioparg: int = 0,
        loc: AST | SrcLocation = NO_LOCATION,
        target: Block | None = None,
        exc_handler: Block | None = None,
    ) -> None:
        self.insts.append(Instruction(opname, oparg, ioparg, loc, target, exc_handler))

    def has_no_lineno(self) -> bool:
        for inst in self.insts:
            if inst.lineno >= 0:
                return False
        return True

    @property
    def exits(self) -> bool:
        return len(self.insts) > 0 and self.insts[-1].opname in SCOPE_EXIT_OPCODES

    def copy(self) -> Block:
        # Cannot copy block if it has fallthrough, since a block can have only one
        # fallthrough predecessor
        assert not self.has_fallthrough
        result = Block()
        result.insts = [instr.copy() for instr in self.insts]
        result.is_exit = self.is_exit
        result.has_fallthrough = False
        return result


# flags for code objects

# the FlowGraph is transformed in place; it exists in one of these states
ACTIVE = "ACTIVE"  # accepting calls to .emit()
CLOSED = "CLOSED"  # closed to new instructions
CONSTS_CLOSED = "CONSTS_CLOSED"  # closed to new consts
OPTIMIZED = "OPTIMIZED"  # optimizations have been run
ORDERED = "ORDERED"  # basic block ordering is set
FINAL = "FINAL"  # all optimization and normalization of flow graph is done
FLAT = "FLAT"  # flattened
DONE = "DONE"


class IndexedSet:
    """Container that behaves like a `set` that assigns stable dense indexes
    to each element. Put another way: This behaves like a `list` where you
    check `x in <list>` before doing any insertion to avoid duplicates. But
    contrary to the list this does not require an O(n) member check."""

    __delitem__: None = None

    def __init__(self, iterable: Iterable[str] = ()) -> None:
        self.keys: dict[str, int] = {}
        for item in iterable:
            self.get_index(item)

    def __add__(self, iterable: Iterable[str]) -> IndexedSet:
        result = IndexedSet()
        for item in self.keys.keys():
            result.get_index(item)
        for item in iterable:
            result.get_index(item)
        return result

    def __contains__(self, item: str) -> bool:
        return item in self.keys

    def __iter__(self) -> Iterator[str]:
        # This relies on `dict` maintaining insertion order.
        return iter(self.keys.keys())

    def __len__(self) -> int:
        return len(self.keys)

    def get_index(self, item: str) -> int:
        """Return index of name in collection, appending if necessary"""
        assert type(item) is str
        idx = self.keys.get(item)
        if idx is not None:
            return idx
        idx = len(self.keys)
        self.keys[item] = idx
        return idx

    def index(self, item: str) -> int:
        assert type(item) is str
        idx = self.keys.get(item)
        if idx is not None:
            return idx
        raise ValueError()

    def update(self, iterable: Iterable[str]) -> None:
        for item in iterable:
            self.get_index(item)


TConstValue = TypeVar("TConstValue", bound=object)
TConstKey: TypeAlias = (
    tuple[type[TConstValue], TConstValue] | tuple[type[TConstValue], TConstValue, ...]
)


class PyFlowGraph(FlowGraph):
    super_init: Callable[[FlowGraph], None] = FlowGraph.__init__
    flow_graph_optimizer: type[FlowGraphOptimizer] = FlowGraphOptimizer
    opcode: Opcode = opcodes_opcode

    def __init__(
        self,
        name: str,
        filename: str,
        scope: Scope | None,
        flags: int = 0,
        args: Sequence[str] = (),
        kwonlyargs: Sequence[str] = (),
        starargs: Sequence[str] = (),
        optimized: int = 0,
        klass: bool = False,
        docstring: str | None = None,
        firstline: int = 0,
        posonlyargs: int = 0,
        suppress_default_const: bool = False,
    ) -> None:
        self.super_init()
        self.name = name
        self.filename = filename
        self.scope = scope
        self.docstring = None
        self.args = args
        self.kwonlyargs = kwonlyargs
        self.posonlyargs = posonlyargs
        self.starargs = starargs
        self.klass = klass
        self.stacksize = 0
        self.docstring = docstring
        self.flags = flags
        if optimized:
            self.setFlag(CO_OPTIMIZED | CO_NEWLOCALS)
        self.consts: dict[TConstKey, int] = {}
        self.names = IndexedSet()
        # Free variables found by the symbol table scan, including
        # variables used only in nested scopes, are included here.
        if scope is not None:
            self.freevars: IndexedSet = IndexedSet(scope.get_free_vars())
            self.cellvars: IndexedSet = IndexedSet(scope.get_cell_vars())
        else:
            self.freevars = IndexedSet([])
            self.cellvars = IndexedSet([])
        # The closure list is used to track the order of cell
        # variables and free variables in the resulting code object.
        # The offsets used by LOAD_CLOSURE/LOAD_DEREF refer to both
        # kinds of variables.
        self.closure: IndexedSet = self.cellvars + self.freevars
        varnames = IndexedSet()
        varnames.update(args)
        varnames.update(kwonlyargs)
        varnames.update(starargs)
        self.varnames: IndexedSet = varnames
        self.stage: str = ACTIVE
        self.firstline = firstline
        self.first_inst_lineno = 0
        # Add any extra consts that were requested to the const pool
        self.extra_consts: list[object] = []
        if not suppress_default_const:
            self.initializeConsts()
        self.fast_vars: set[object] = set()
        self.gen_kind: int | None = None
        self.insts: list[Instruction] = []
        if flags & CO_COROUTINE:
            self.gen_kind = 1
        elif flags & CO_ASYNC_GENERATOR:
            self.gen_kind = 2
        elif flags & CO_GENERATOR:
            self.gen_kind = 0

    def emit_gen_start(self) -> None:
        if self.gen_kind is not None:
            self.emit_noline("GEN_START", self.gen_kind)

    # These are here rather than in CodeGenerator because we need to emit jumps
    # while doing block operations in the flowgraph
    def emit_jump_forward(self, target: Block) -> None:
        raise NotImplementedError()

    def emit_jump_forward_noline(self, target: Block) -> None:
        raise NotImplementedError()

    def emit_call_one_arg(self) -> None:
        raise NotImplementedError()

    def emit_super_call(self, name: str, is_zero: bool) -> None:
        raise NotImplementedError()

    def emit_load_method(self, name: str) -> None:
        raise NotImplementedError()

    def emit_call_method(self, argcnt: int) -> None:
        raise NotImplementedError()

    def emit_prologue(self) -> None:
        raise NotImplementedError()

    def emit_format_value(self, format: int = -1) -> None:
        if format == -1:
            format = FVC_NONE
        self.emit("FORMAT_VALUE", format)

    def setFlag(self, flag: int) -> None:
        self.flags |= flag

    def checkFlag(self, flag: int) -> int | None:
        if self.flags & flag:
            return 1

    def initializeConsts(self) -> None:
        # Docstring is first entry in co_consts for normal functions
        # (Other types of code objects deal with docstrings in different
        # manner, e.g. lambdas and comprehensions don't have docstrings,
        # classes store them as __doc__ attribute.
        if self.name == "<lambda>":
            self.consts[self.get_const_key(None)] = 0
        elif not self.name.startswith("<") and not self.klass:
            if self.docstring is not None:
                self.consts[self.get_const_key(self.docstring)] = 0
            else:
                self.consts[self.get_const_key(None)] = 0

    def convertArg(self, opcode: str, oparg: object) -> int:
        assert self.stage in {ACTIVE, CLOSED}, self.stage

        if self.do_not_emit_bytecode and opcode in self._quiet_opcodes:
            # return -1 so this errors if it ever ends up in non-dead-code due
            # to a bug.
            return -1

        conv = self._converters.get(opcode)
        if conv is not None:
            return conv(self, oparg)

        return super().convertArg(opcode, oparg)

    def finalize(self) -> None:
        """Perform final optimizations and normalization of flow graph."""
        assert self.stage == ACTIVE, self.stage
        self.stage = CLOSED

        for block in self.ordered_blocks:
            self.normalize_basic_block(block)

        self.optimizeCFG()

        self.stage = CONSTS_CLOSED
        self.trim_unused_consts()
        self.duplicate_exits_without_lineno()
        self.propagate_line_numbers()
        self.firstline = self.firstline or self.first_inst_lineno or 1
        self.guarantee_lineno_for_exits()

        self.stage = ORDERED
        self.normalize_jumps()
        self.stage = FINAL

    def assemble_final_code(self) -> None:
        """Finish assembling code object components from the final graph."""
        raise NotImplementedError()

    def getCode(self) -> CodeType:
        """Get a Python code object"""
        raise NotImplementedError()

    def dump(self, io: TextIO | None = None, stack_effect: bool = False) -> None:
        if io:
            with redirect_stdout(io):
                dump_graph(self, stack_effect)
        else:
            dump_graph(self, stack_effect)

    def push_block(self, worklist: list[Block], block: Block, depth: int) -> None:
        assert block.startdepth < 0 or block.startdepth >= depth, (
            f"{block!r}: {block.startdepth} vs {depth}"
        )
        if block.startdepth < depth:
            block.startdepth = depth
            worklist.append(block)

    def get_stack_effects(self, opname: str, oparg: object, jump: bool) -> int:
        return self.opcode.stack_effect_raw(opname, oparg, jump)

    @property
    def initial_stack_depth(self) -> int:
        return 0 if self.gen_kind is None else 1

    def stackdepth_walk(self, block: Block) -> int:
        # see flowgraph.c :: _PyCfg_Stackdepth()
        maxdepth = 0
        worklist = []
        self.push_block(worklist, block, self.initial_stack_depth)
        while worklist:
            block = worklist.pop()
            next = block.next
            depth = block.startdepth
            assert depth >= 0

            for instr in block.getInstructions():
                delta = self.get_stack_effects(instr.opname, instr.oparg, False)
                new_depth = depth + delta
                if new_depth > maxdepth:
                    maxdepth = new_depth

                assert new_depth >= 0, (instr, self.dump())

                op = self.opcode.opmap[instr.opname]
                if (
                    self.opcode.has_jump(op) or instr.opname in SETUP_OPCODES
                ) and instr.opname != "END_ASYNC_FOR":
                    delta = self.get_stack_effects(instr.opname, instr.oparg, True)

                    target_depth = depth + delta
                    if target_depth > maxdepth:
                        maxdepth = target_depth

                    assert target_depth >= 0

                    self.push_block(worklist, instr.target, target_depth)

                depth = new_depth

                if (
                    instr.opname in SCOPE_EXIT_OPCODES
                    or instr.opname in UNCONDITIONAL_JUMP_OPCODES
                ):
                    # Remaining code is dead
                    next = None
                    break

            # Consider saving the delta we came up with here and reapplying it on
            # subsequent walks rather than having to walk all of the instructions again.
            if next:
                self.push_block(worklist, next, depth)

        return maxdepth

    def compute_stack_depth(self) -> None:
        """Compute the max stack depth.

        Find the flow path that needs the largest stack.  We assume that
        cycles in the flow graph have no net effect on the stack depth.
        """
        assert self.stage == FINAL, self.stage
        for block in self.getBlocksInOrder():
            # We need to get to the first block which actually has instructions
            if block.getInstructions():
                self.stacksize = self.stackdepth_walk(block)
                break

    def instrsize(self, instr: Instruction, oparg: int) -> int:
        if oparg <= 0xFF:
            return 1
        elif oparg <= 0xFFFF:
            return 2
        elif oparg <= 0xFFFFFF:
            return 3
        else:
            return 4

    def flatten_jump(self, inst: Instruction, pc: int) -> int:
        target = inst.target
        assert target is not None

        offset = target.offset
        if self.opcode.opmap[inst.opname] in self.opcode.hasjrel:
            offset -= pc

        return abs(offset)

    def flatten_graph(self) -> None:
        """Arrange the blocks in order and resolve jumps"""
        assert self.stage == FINAL, self.stage
        # This is an awful hack that could hurt performance, but
        # on the bright side it should work until we come up
        # with a better solution.
        #
        # The issue is that in the first loop blocksize() is called
        # which calls instrsize() which requires i_oparg be set
        # appropriately. There is a bootstrap problem because
        # i_oparg is calculated in the second loop.
        #
        # So we loop until we stop seeing new EXTENDED_ARGs.
        # The only EXTENDED_ARGs that could be popping up are
        # ones in jump instructions.  So this should converge
        # fairly quickly.
        extended_arg_recompile = True
        while extended_arg_recompile:
            extended_arg_recompile = False
            self.insts = insts = []
            pc = 0
            for b in self.getBlocksInOrder():
                b.offset = pc

                for inst in b.getInstructions():
                    insts.append(inst)
                    pc += self.instrsize(inst, inst.ioparg)

            pc = 0
            for inst in insts:
                pc += self.instrsize(inst, inst.ioparg)
                op = self.opcode.opmap[inst.opname]
                if self.opcode.has_jump(op):
                    offset = self.flatten_jump(inst, pc)

                    if self.instrsize(inst, inst.ioparg) != self.instrsize(
                        inst, offset
                    ):
                        extended_arg_recompile = True

                    inst.ioparg = offset

        self.stage = FLAT

    # TASK(T128853358): pull out all converters for static opcodes into
    # StaticPyFlowGraph

    def _convert_LOAD_CONST(self, arg: object) -> int:
        getCode = getattr(arg, "getCode", None)
        if getCode is not None:
            arg = getCode()
        key = self.get_const_key(arg)
        res = self.consts.get(key, self)
        if res is self:
            res = self.consts[key] = len(self.consts)
        return res

    def get_const_key(self, value: TConstValue) -> TConstKey:
        if isinstance(value, float):
            return type(value), value, sign(value)
        elif isinstance(value, complex):
            return type(value), value, sign(value.real), sign(value.imag)
        elif isinstance(value, (tuple, frozenset)):
            return (
                type(value),
                value,
                type(value)(self.get_const_key(const) for const in value),
            )
        elif isinstance(value, slice):
            return (
                type(value),
                value,
                type(value.start),
                type(value.step),
                type(value.stop),
            )

        return type(value), value

    def _convert_LOAD_FAST(self, arg: object) -> int:
        self.fast_vars.add(arg)
        if isinstance(arg, int):
            return arg

        assert isinstance(arg, str)
        return self.varnames.get_index(arg)

    def _convert_LOAD_LOCAL(self, arg: object) -> int:
        self.fast_vars.add(arg)
        assert isinstance(arg, tuple), "invalid oparg {arg!r}"
        return self._convert_LOAD_CONST((self.varnames.get_index(arg[0]), arg[1]))

    def _convert_NAME(self, arg: object) -> int:
        assert isinstance(arg, str)
        return self.names.get_index(arg)

    def _convert_LOAD_SUPER(self, arg: object) -> int:
        assert isinstance(arg, tuple), "invalid oparg {arg!r}"
        return self._convert_LOAD_CONST((self._convert_NAME(arg[0]), arg[1]))

    def _convert_LOAD_SUPER_ATTR(self, arg: object) -> int:
        assert isinstance(arg, tuple), "invalid oparg {arg!r}"
        op, name, zero_args = arg
        name_idx = self._convert_NAME(name)
        mask = {
            "LOAD_SUPER_ATTR": 2,
            "LOAD_ZERO_SUPER_ATTR": 0,
            "LOAD_SUPER_METHOD": 3,
            "LOAD_ZERO_SUPER_METHOD": 1,
        }
        return (name_idx << 2) | ((not zero_args) << 1) | mask[op]

    def _convert_DEREF(self, arg: object) -> int:
        # Sometimes, both cellvars and freevars may contain the same var
        # (e.g., for class' __class__). In this case, prefer freevars.
        assert isinstance(arg, str)
        if arg in self.freevars:
            return self.freevars.get_index(arg) + len(self.cellvars)
        return self.closure.get_index(arg)

    # similarly for other opcodes...
    _converters: dict[str, Callable[[PyFlowGraph, object], int]] = {
        "LOAD_CLASS": _convert_LOAD_CONST,
        "LOAD_CONST": _convert_LOAD_CONST,
        "INVOKE_FUNCTION": _convert_LOAD_CONST,
        "INVOKE_METHOD": _convert_LOAD_CONST,
        "LOAD_METHOD_STATIC": _convert_LOAD_CONST,
        "INVOKE_NATIVE": _convert_LOAD_CONST,
        "LOAD_FIELD": _convert_LOAD_CONST,
        "STORE_FIELD": _convert_LOAD_CONST,
        "CAST": _convert_LOAD_CONST,
        "TP_ALLOC": _convert_LOAD_CONST,
        "BUILD_CHECKED_MAP": _convert_LOAD_CONST,
        "BUILD_CHECKED_LIST": _convert_LOAD_CONST,
        "PRIMITIVE_LOAD_CONST": _convert_LOAD_CONST,
        "LOAD_FAST": _convert_LOAD_FAST,
        "LOAD_FAST_AND_CLEAR": _convert_LOAD_FAST,
        "STORE_FAST": _convert_LOAD_FAST,
        "STORE_FAST_MAYBE_NULL": _convert_LOAD_FAST,
        "DELETE_FAST": _convert_LOAD_FAST,
        "LOAD_LOCAL": _convert_LOAD_LOCAL,
        "STORE_LOCAL": _convert_LOAD_LOCAL,
        "LOAD_NAME": _convert_NAME,
        "LOAD_FROM_DICT_OR_DEREF": _convert_DEREF,
        "LOAD_FROM_DICT_OR_GLOBALS": _convert_NAME,
        "LOAD_CLOSURE": lambda self, arg: self.closure.get_index(arg),
        "COMPARE_OP": lambda self, arg: self.opcode.CMP_OP.index(arg),
        "LOAD_GLOBAL": _convert_NAME,
        "STORE_GLOBAL": _convert_NAME,
        "DELETE_GLOBAL": _convert_NAME,
        "CONVERT_NAME": _convert_NAME,
        "STORE_NAME": _convert_NAME,
        "STORE_ANNOTATION": _convert_NAME,
        "DELETE_NAME": _convert_NAME,
        "IMPORT_NAME": _convert_NAME,
        "IMPORT_FROM": _convert_NAME,
        "STORE_ATTR": _convert_NAME,
        "LOAD_ATTR": _convert_NAME,
        "DELETE_ATTR": _convert_NAME,
        "LOAD_METHOD": _convert_NAME,
        "LOAD_DEREF": _convert_DEREF,
        "STORE_DEREF": _convert_DEREF,
        "DELETE_DEREF": _convert_DEREF,
        "LOAD_CLASSDEREF": _convert_DEREF,
        "REFINE_TYPE": _convert_LOAD_CONST,
        "LOAD_METHOD_SUPER": _convert_LOAD_SUPER,
        "LOAD_ATTR_SUPER": _convert_LOAD_SUPER,
        "LOAD_SUPER_ATTR": _convert_LOAD_SUPER_ATTR,
        "LOAD_ZERO_SUPER_ATTR": _convert_LOAD_SUPER_ATTR,
        "LOAD_SUPER_METHOD": _convert_LOAD_SUPER_ATTR,
        "LOAD_ZERO_SUPER_METHOD": _convert_LOAD_SUPER_ATTR,
        "LOAD_TYPE": _convert_LOAD_CONST,
    }

    # Converters which add an entry to co_consts
    _const_converters: set[Callable[[PyFlowGraph, object], int]] = {
        _convert_LOAD_CONST,
        _convert_LOAD_LOCAL,
        _convert_LOAD_SUPER,
        _convert_LOAD_SUPER_ATTR,
    }
    # Opcodes which reference an entry in co_consts
    _const_opcodes: set[str] = set()
    for op, converter in _converters.items():
        if converter in _const_converters:
            _const_opcodes.add(op)

    # Opcodes which do not add names to co_consts/co_names/co_varnames in dead code (self.do_not_emit_bytecode)
    _quiet_opcodes = {
        "LOAD_GLOBAL",
        "LOAD_CONST",
        "IMPORT_NAME",
        "STORE_ATTR",
        "LOAD_ATTR",
        "DELETE_ATTR",
        "LOAD_METHOD",
        "STORE_FAST",
        "LOAD_FAST",
    }

    def make_byte_code(self) -> bytes:
        assert self.stage == FLAT, self.stage

        code: bytearray = bytearray()

        def addCode(opcode: int, oparg: int) -> None:
            assert opcode < 256, opcode
            code.append(opcode)
            code.append(oparg)

        for t in self.insts:
            oparg = t.ioparg
            assert 0 <= oparg <= 0xFFFFFFFF, oparg
            if oparg > 0xFFFFFF:
                addCode(self.opcode.EXTENDED_ARG, (oparg >> 24) & 0xFF)
            if oparg > 0xFFFF:
                addCode(self.opcode.EXTENDED_ARG, (oparg >> 16) & 0xFF)
            if oparg > 0xFF:
                addCode(self.opcode.EXTENDED_ARG, (oparg >> 8) & 0xFF)
            addCode(self.opcode.opmap[t.opname], oparg & 0xFF)
            self.emit_inline_cache(t.opname, addCode)

        self.stage = DONE
        return bytes(code)

    def emit_inline_cache(
        self, opcode: str, addCode: Callable[[int, int], None]
    ) -> None:
        pass

    def make_line_table(self) -> bytes:
        lnotab = LineAddrTable()
        lnotab.setFirstLine(self.firstline)

        prev_offset = offset = 0
        for t in self.insts:
            if lnotab.current_line != t.lineno and t.lineno:
                lnotab.nextLine(t.lineno, prev_offset, offset)
                prev_offset = offset

            offset += self.instrsize(t, t.ioparg) * self.opcode.CODEUNIT_SIZE

        # Since the linetable format writes the end offset of bytecodes, we can't commit the
        # last write until all the instructions are iterated over.
        lnotab.emitCurrentLine(prev_offset, offset)
        return lnotab.getTable()

    def getConsts(self) -> tuple[object]:
        """Return a tuple for the const slot of the code object"""
        # Just return the constant value, removing the type portion. Order by const index.
        return tuple(
            const[1] for const, idx in sorted(self.consts.items(), key=lambda o: o[1])
        )

    def propagate_line_numbers(self) -> None:
        """Propagate line numbers to instructions without."""
        for block in self.ordered_blocks:
            if not block.insts:
                continue
            prev_loc = NO_LOCATION
            for instr in block.insts:
                if instr.lineno == NO_LOCATION.lineno:
                    instr.loc = prev_loc
                else:
                    prev_loc = instr.loc
            if block.has_fallthrough:
                next = block.next
                assert next
                if next.num_predecessors == 1 and next.insts:
                    next_instr = next.insts[0]
                    if next_instr.lineno == -1:
                        next_instr.loc = prev_loc
            last_instr = block.insts[-1]
            if (
                last_instr.is_jump(self.opcode)
                and last_instr.opname not in SETUP_OPCODES
            ):
                # Only actual jumps, not exception handlers
                target = last_instr.target
                assert target
                # CPython doesn't check target.insts and instead can write
                # to random memory...
                if target.num_predecessors == 1 and target.insts:
                    next_instr = target.insts[0]
                    if next_instr.lineno == NO_LOCATION.lineno:
                        next_instr.loc = prev_loc

    def guarantee_lineno_for_exits(self) -> None:
        assert self.firstline > 0
        loc = SrcLocation(self.firstline, self.firstline, 0, 0)
        for block in self.ordered_blocks:
            if not block.insts:
                continue
            last_instr = block.insts[-1]
            if last_instr.lineno < 0:
                if last_instr.opname == "RETURN_VALUE":
                    for instr in block.insts:
                        assert instr.loc == NO_LOCATION
                        instr.loc = loc
            else:
                loc = last_instr.loc

    def is_exit_without_line_number(self, target: Block) -> bool:
        raise NotImplementedError()

    def get_duplicate_exit_visitation_order(self) -> Iterable[Block]:
        raise NotImplementedError()

    def get_target_block_for_exit(self, target: Block) -> Block:
        assert target.insts
        return target

    def duplicate_exits_without_lineno(self) -> None:
        """
        PEP 626 mandates that the f_lineno of a frame is correct
        after a frame terminates. It would be prohibitively expensive
        to continuously update the f_lineno field at runtime,
        so we make sure that all exiting instruction (raises and returns)
        have a valid line number, allowing us to compute f_lineno lazily.
        We can do this by duplicating the exit blocks without line number
        so that none have more than one predecessor. We can then safely
        copy the line number from the sole predecessor block.
        """
        # Copy all exit blocks without line number that are targets of a jump.
        append_after = {}
        for block in self.get_duplicate_exit_visitation_order():
            if block.insts and (last := block.insts[-1]).is_jump(self.opcode):
                if last.opname in SETUP_OPCODES:
                    continue
                target = last.target
                assert target
                target = self.get_target_block_for_exit(target)
                if target.insts[0].opname == "SETUP_CLEANUP":
                    # We have wrapped this block in a stopiteration handler.
                    # The SETUP_CLEANUP is a pseudo-op which will be removed in
                    # a later pass, so it does not need a line number,
                    continue

                if (
                    self.is_exit_without_line_number(target)
                    and target.num_predecessors > 1
                ):
                    new_target = target.copy()
                    new_target.bid = self.get_new_block_id()
                    new_target.insts[0].loc = last.loc
                    last.target = new_target
                    target.num_predecessors -= 1
                    new_target.num_predecessors = 1
                    target.insert_next(new_target)
                    append_after.setdefault(target, []).append(new_target)
        for after, to_append in append_after.items():
            idx = self.ordered_blocks.index(after) + 1
            self.ordered_blocks[idx:idx] = reversed(to_append)

        for block in self.ordered_blocks:
            if block.has_fallthrough and block.next and block.insts:
                if self.is_exit_without_line_number(block.next):
                    block.next.insts[0].loc = block.insts[-1].loc

    def normalize_jumps(self) -> None:
        assert self.stage == ORDERED, self.stage

        seen_blocks = set()

        for block in self.ordered_blocks:
            seen_blocks.add(block.bid)
            if not block.insts:
                continue
            last = block.insts[-1]
            target = last.target

            if last.opname == "JUMP_ABSOLUTE":
                assert target
                if target.bid not in seen_blocks:
                    last.opname = "JUMP_FORWARD"
            elif last.opname == "JUMP_FORWARD":
                assert target
                if target.bid in seen_blocks:
                    last.opname = "JUMP_ABSOLUTE"

    def remove_redundant_nops(self, optimizer: FlowGraphOptimizer) -> bool:
        prev_block = None
        cleaned = False
        for block in self.ordered_blocks:
            prev_lineno = -1
            if prev_block and prev_block.insts:
                prev_lineno = prev_block.insts[-1].lineno
            cleaned |= optimizer.clean_basic_block(block, prev_lineno)
            prev_block = block if block.has_fallthrough else None
        return cleaned

    def remove_redundant_jumps(
        self, optimizer: FlowGraphOptimizer, clean: bool = True
    ) -> bool:
        # Delete jump instructions made redundant by previous step. If a non-empty
        # block ends with a jump instruction, check if the next non-empty block
        # reached through normal flow control is the target of that jump. If it
        # is, then the jump instruction is redundant and can be deleted.
        maybe_empty_blocks = False
        for block in self.ordered_blocks:
            if not block.insts:
                continue
            last = block.insts[-1]
            if last.opname not in UNCONDITIONAL_JUMP_OPCODES:
                continue
            if last.target == block.next:
                block.has_fallthrough = True
                last.set_to_nop()
                if clean:
                    optimizer.clean_basic_block(block, -1)
                maybe_empty_blocks = True
        return maybe_empty_blocks

    def optimizeCFG(self) -> None:
        """Optimize a well-formed CFG."""
        raise NotImplementedError()

    def eliminate_empty_basic_blocks(self) -> None:
        for block in self.ordered_blocks:
            next_block = block.next
            if next_block:
                while not next_block.insts and next_block.next:
                    next_block = next_block.next
                block.next = next_block
        for block in self.ordered_blocks:
            if not block.insts:
                continue
            last = block.insts[-1]
            if last.is_jump(self.opcode) and last.opname != "END_ASYNC_FOR":
                target = last.target
                while not target.insts and target.next:
                    target = target.next
                last.target = target
        self.ordered_blocks = [block for block in self.ordered_blocks if block.insts]

    def unlink_unreachable_basic_blocks(self, reachable_blocks: set[int]) -> None:
        self.ordered_blocks = [
            block
            for block in self.ordered_blocks
            if block.bid in reachable_blocks or block.is_exc_handler
        ]
        prev = None
        for block in self.ordered_blocks:
            block.prev = prev
            if prev is not None:
                prev.next = block
            prev = block

    def remove_unreachable_basic_blocks(self) -> None:
        # mark all reachable blocks
        for block in self.getBlocks():
            block.num_predecessors = 0

        reachable_blocks = set()
        worklist = [self.entry]
        self.entry.num_predecessors = 1
        while worklist:
            entry = worklist.pop()
            if entry.bid in reachable_blocks:
                continue
            reachable_blocks.add(entry.bid)
            for instruction in entry.getInstructions():
                target = instruction.target
                if target is not None:
                    worklist.append(target)
                    target.num_predecessors += 1

            if entry.has_fallthrough:
                next = entry.next
                assert next

                worklist.append(next)
                next.num_predecessors += 1

        self.unlink_unreachable_basic_blocks(reachable_blocks)

    def normalize_basic_block(self, block: Block) -> None:
        """Sets the `fallthrough` and `exit` properties of a block, and ensures that the targets of
        any jumps point to non-empty blocks by following the next pointer of empty blocks.
        """
        for instr in block.getInstructions():
            if instr.opname in SCOPE_EXIT_OPCODES:
                block.is_exit = True
                block.has_fallthrough = False
                continue
            elif instr.opname in UNCONDITIONAL_JUMP_OPCODES:
                block.has_fallthrough = False
            elif not instr.is_jump(self.opcode):
                continue

            # pyre-fixme[16] instr.target can be None
            while not instr.target.insts:
                # pyre-fixme[16] instr.target can be None
                instr.target = instr.target.next

    def should_inline_block(self, block: Block) -> bool:
        return block.is_exit and len(block.insts) <= MAX_COPY_SIZE

    def extend_block(self, block: Block) -> bool:
        """If this block ends with an unconditional jump to an exit block,
        then remove the jump and extend this block with the target.
        """
        if len(block.insts) == 0:
            return False
        last = block.insts[-1]
        if last.opname not in UNCONDITIONAL_JUMP_OPCODES:
            return False
        target = last.target
        assert target is not None
        if not self.should_inline_block(target):
            return False
        last = block.insts[-1]
        last.set_to_nop()
        for instr in target.insts:
            block.insts.append(instr.copy())

        block.next = None
        block.is_exit = True
        block.has_fallthrough = False
        return True

    def trim_unused_consts(self) -> None:
        """Remove trailing unused constants."""
        assert self.stage == CONSTS_CLOSED, self.stage

        max_const_index = 0
        for block in self.ordered_blocks:
            for instr in block.insts:
                if (
                    instr.opname in self._const_opcodes
                    and instr.ioparg > max_const_index
                ):
                    max_const_index = instr.ioparg

        self.consts = {
            key: index for key, index in self.consts.items() if index <= max_const_index
        }


class PyFlowGraph310(PyFlowGraph):
    flow_graph_optimizer = FlowGraphOptimizer310

    # pyre-ignore[2] Forwarding all of the arguments up to the super __init__.
    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        # Final assembled code objects
        self.bytecode: bytes | None = None
        self.line_table: bytes | None = None

    def emit_call_one_arg(self) -> None:
        self.emit("CALL_FUNCTION", 1)

    def emit_load_method(self, name: str) -> None:
        self.emit("LOAD_METHOD", name)

    def emit_call_method(self, argcnt: int) -> None:
        self.emit("CALL_METHOD", argcnt)

    def emit_prologue(self) -> None:
        pass

    def is_exit_without_line_number(self, target: Block) -> bool:
        return target.is_exit and target.insts[0].lineno < 0

    def get_duplicate_exit_visitation_order(self) -> Iterable[Block]:
        return self.blocks_in_reverse_allocation_order()

    def emit_jump_forward(self, target: Block) -> None:
        self.emit("JUMP_FORWARD", target)

    def emit_jump_forward_noline(self, target: Block) -> None:
        self.emit_noline("JUMP_FORWARD", target)

    def optimizeCFG(self) -> None:
        """Optimize a well-formed CFG."""
        for block in self.blocks_in_reverse_allocation_order():
            self.extend_block(block)

        assert self.stage == CLOSED, self.stage

        optimizer = self.flow_graph_optimizer(self)
        for block in self.ordered_blocks:
            optimizer.optimize_basic_block(block)
            optimizer.clean_basic_block(block, -1)

        for block in self.blocks_in_reverse_allocation_order():
            self.extend_block(block)

        self.remove_redundant_nops(optimizer)

        self.eliminate_empty_basic_blocks()
        self.remove_unreachable_basic_blocks()

        maybe_empty_blocks = self.remove_redundant_jumps(optimizer)

        if maybe_empty_blocks:
            self.eliminate_empty_basic_blocks()

        self.stage = OPTIMIZED

    def assemble_final_code(self) -> None:
        """Finish assembling code object components from the final graph."""
        self.finalize()
        assert self.stage == FINAL, self.stage

        self.compute_stack_depth()
        self.flatten_graph()

        assert self.stage == FLAT, self.stage
        self.bytecode = self.make_byte_code()
        self.line_table = self.make_line_table()

    def getCode(self) -> CodeType:
        """Get a Python code object"""
        self.assemble_final_code()
        bytecode = self.bytecode
        assert bytecode is not None
        line_table = self.line_table
        assert line_table is not None
        assert self.stage == DONE, self.stage
        return self.new_code_object(bytecode, line_table)

    def new_code_object(self, code: bytes, lnotab: bytes) -> CodeType:
        assert self.stage == DONE, self.stage
        if (self.flags & CO_NEWLOCALS) == 0:
            nlocals = len(self.fast_vars)
        else:
            nlocals = len(self.varnames)

        firstline = self.firstline
        # For module, .firstline is initially not set, and should be first
        # line with actual bytecode instruction (skipping docstring, optimized
        # out instructions, etc.)
        if not firstline:
            firstline = self.first_inst_lineno
        # If no real instruction, fallback to 1
        if not firstline:
            firstline = 1

        consts = self.getConsts()
        consts = consts + tuple(self.extra_consts)
        return self.make_code(nlocals, code, consts, firstline, lnotab)

    def make_code(
        self,
        nlocals: int,
        code: bytes,
        consts: tuple[object, ...],
        firstline: int,
        lnotab: bytes,
    ) -> CodeType:
        return CodeType(
            len(self.args),
            self.posonlyargs,
            len(self.kwonlyargs),
            nlocals,
            self.stacksize,
            self.flags,
            code,
            consts,
            tuple(self.names),
            tuple(self.varnames),
            self.filename,
            self.name,
            # pyre-fixme[6]: For 13th argument expected `str` but got `int`.
            firstline,
            # pyre-fixme[6]: For 14th argument expected `int` but got `bytes`.
            lnotab,
            # pyre-fixme[6]: For 15th argument expected `bytes` but got `tuple[str,
            #  ...]`.
            tuple(self.freevars),
            # pyre-fixme[6]: For 16th argument expected `bytes` but got `tuple[str,
            #  ...]`.
            tuple(self.cellvars),
        )


class PyFlowGraphCinderMixin(PyFlowGraph):
    opcode: Opcode = cinder_opcode

    def emit_super_call(self, name: str, is_zero: bool) -> None:
        self.emit("LOAD_METHOD_SUPER", (name, is_zero))

    def make_code(
        self,
        nlocals: int,
        code: bytes,
        consts: tuple[object, ...],
        firstline: int,
        lnotab: bytes,
    ) -> CodeType:
        if self.scope is not None and self.scope.suppress_jit:
            self.setFlag(CO_SUPPRESS_JIT)
        # pyre-ignore[16]: `PyFlowGraph` has no attribute `make_code`
        return super().make_code(nlocals, code, consts, firstline, lnotab)


class PyFlowGraphCinder310(PyFlowGraphCinderMixin, PyFlowGraph310):
    pass


class PyFlowGraph312(PyFlowGraph):
    flow_graph_optimizer = FlowGraphOptimizer312

    def __init__(
        self,
        name: str,
        filename: str,
        scope: Scope | None,
        flags: int = 0,
        args: Sequence[str] = (),
        kwonlyargs: Sequence[str] = (),
        starargs: Sequence[str] = (),
        optimized: int = 0,
        klass: bool = False,
        docstring: Optional[str] = None,
        firstline: int = 0,
        posonlyargs: int = 0,
        qualname: Optional[str] = None,
        suppress_default_const: bool = False,
    ) -> None:
        super().__init__(
            name,
            filename,
            scope,
            flags,
            args,
            kwonlyargs,
            starargs,
            optimized,
            klass,
            docstring,
            firstline,
            posonlyargs,
            suppress_default_const,
        )
        self.qualname: str = qualname or name
        # Final assembled code objects
        self.bytecode: bytes | None = None
        self.line_table: bytes | None = None
        self.exception_table: bytes | None = None

    def emit_call_one_arg(self) -> None:
        self.emit("CALL", 0)

    def emit_load_method(self, name: str) -> None:
        self.emit("LOAD_ATTR", (name, 1))

    def emit_call_method(self, argcnt: int) -> None:
        self.emit("CALL", argcnt)

    def emit_super_call(self, name: str, is_zero: bool) -> None:
        op = "LOAD_ZERO_SUPER_METHOD" if is_zero else "LOAD_SUPER_METHOD"
        self.emit("LOAD_SUPER_ATTR", (op, name, is_zero))

    def emit_prologue(self) -> None:
        self.emit("RESUME", 0)

    def is_exit_without_line_number(self, target: Block) -> bool:
        if not target.is_exit:
            return False

        for inst in target.insts:
            if inst.lineno >= 0:
                return False

        return True

    def get_duplicate_exit_visitation_order(self) -> Iterable[Block]:
        return self.ordered_blocks

    def emit_gen_start(self) -> None:
        # This is handled with the prefix instructions in finalize
        pass

    def emit_jump_forward(self, target: Block) -> None:
        self.emit("JUMP", target)

    def emit_jump_forward_noline(self, target: Block) -> None:
        self.emit_noline("JUMP", target)

    def flatten_jump(self, inst: Instruction, pc: int) -> int:
        target = inst.target
        assert target is not None, inst.opname

        offset = target.offset - pc

        return abs(offset)

    def push_except_block(self, except_stack: list[Block], instr: Instruction) -> Block:
        target = instr.target
        assert target is not None, instr
        if instr.opname in ("SETUP_WITH", "SETUP_CLEANUP"):
            target.preserve_lasti = True
        except_stack.append(target)
        return target

    def label_exception_targets(self) -> None:
        def push_todo_block(block: Block) -> None:
            todo_stack.append(block)
            visited.add(block)

        todo_stack: list[Block] = [self.entry]
        visited: set[Block] = {self.entry}
        except_stack = []
        self.entry.except_stack = except_stack

        while todo_stack:
            block = todo_stack.pop()
            assert block in visited
            except_stack = block.except_stack
            block.except_stack = []
            handler = except_stack[-1] if except_stack else None
            for instr in block.insts:
                if instr.opname in SETUP_OPCODES:
                    target = instr.target
                    assert target, instr
                    if target not in visited:
                        # Copy the current except stack into the target's except stack
                        target.except_stack = list(except_stack)
                        push_todo_block(target)
                    handler = self.push_except_block(except_stack, instr)
                elif instr.opname == "POP_BLOCK":
                    except_stack.pop()
                    handler = except_stack[-1] if except_stack else None
                elif instr.is_jump(self.opcode):
                    instr.exc_handler = handler
                    if instr.target not in visited:
                        target = instr.target
                        assert target
                        if block.has_fallthrough:
                            # Copy the current except stack into the block's except stack
                            target.except_stack = list(except_stack)
                        else:
                            # Move the current except stack to the block and start a new one
                            target.except_stack = except_stack
                            except_stack = []
                        push_todo_block(target)
                else:
                    if instr.opname == "YIELD_VALUE":
                        assert except_stack is not None
                        instr.ioparg = len(except_stack)
                    instr.exc_handler = handler

            if block.has_fallthrough and block.next and block.next not in visited:
                assert except_stack is not None
                block.next.except_stack = except_stack
                push_todo_block(block.next)

    def compute_except_handlers(self) -> set[Block]:
        except_handlers: set[Block] = set()
        for block in self.ordered_blocks:
            for instr in block.insts:
                if instr.opname in SETUP_OPCODES:
                    target = instr.target
                    assert target is not None, "SETUP_* opcodes all have targets"
                    except_handlers.add(target)
                    break

        return except_handlers

    def make_explicit_jump_block(self) -> Block:
        return self.newBlock("explicit_jump")

    def remove_redundant_nops_and_jumps(self, optimizer: FlowGraphOptimizer) -> None:
        self.remove_redundant_jumps(optimizer, False)

    def push_cold_blocks_to_end(
        self, except_handlers: set[Block], optimizer: FlowGraphOptimizer
    ) -> None:
        warm = self.compute_warm()

        # If we have a cold block with fallthrough to a warm block, add
        # an explicit jump instead of fallthrough
        for block in list(self.ordered_blocks):
            if block not in warm and block.has_fallthrough and block.next in warm:
                explicit_jump = self.make_explicit_jump_block()
                explicit_jump.bid = self.get_new_block_id()
                self.current = explicit_jump

                next_block = block.next
                assert next_block is not None

                self.emit_jump_forward_noline(next_block)
                self.ordered_blocks.insert(
                    self.ordered_blocks.index(block) + 1, explicit_jump
                )

                explicit_jump.next = block.next
                explicit_jump.has_fallthrough = False
                block.next = explicit_jump

        new_ordered = []
        to_end = []
        prev = None
        for block in self.ordered_blocks:
            if block in warm:
                new_ordered.append(block)
                if prev is not None:
                    prev.next = block
                block.prev = prev
                prev = block
            else:
                to_end.append(block)

        for block in to_end:
            prev.next = block
            block.prev = prev
            prev = block

        block.next = None

        self.ordered_blocks = new_ordered + to_end
        if to_end:
            self.remove_redundant_nops_and_jumps(optimizer)

    def compute_warm(self) -> set[Block]:
        """Compute the set of 'warm' blocks, which are blocks that are reachable
        through normal control flow. 'Cold' blocks are those that are not
        directly reachable and may be moved to the end of the block list
        for optimization purposes."""

        stack = [self.entry]
        visited = set(stack)
        warm: set[Block] = set()

        while stack:
            cur = stack.pop()
            warm.add(cur)
            next = cur.next
            if next is not None and cur.has_fallthrough and next not in visited:
                stack.append(next)
                visited.add(next)

            for instr in cur.insts:
                target = instr.target
                if (
                    target is not None
                    and instr.is_jump(self.opcode)
                    and target not in visited
                ):
                    stack.append(target)
                    visited.add(target)
        return warm

    def remove_unused_consts(self) -> None:
        nconsts = len(self.consts)
        if not nconsts:
            return

        index_map = [-1] * nconsts
        index_map[0] = 0  # The first constant may be docstring; keep it always.

        # mark used consts
        for block in self.ordered_blocks:
            for instr in block.insts:
                if instr.opname in self.opcode.hasconst:
                    index_map[instr.ioparg] = instr.ioparg

        used_consts = [x for x in index_map if x > -1]
        if len(used_consts) == nconsts:
            return

        reverse_index_mapping = {
            old_index: index for index, old_index in enumerate(used_consts)
        }

        # generate the updated consts mapping and the mapping from
        # the old index to the new index
        new_consts = {
            key: reverse_index_mapping[old_index]
            for key, old_index in self.consts.items()
            if old_index in reverse_index_mapping
        }
        self.consts = new_consts

        # now update the existing opargs
        for block in self.ordered_blocks:
            for instr in block.insts:
                if instr.opname in self.opcode.hasconst:
                    instr.ioparg = reverse_index_mapping[instr.ioparg]

    def add_checks_for_loads_of_uninitialized_variables(self) -> None:
        UninitializedVariableChecker(self).check()

    def inline_small_exit_blocks(self) -> None:
        for block in self.ordered_blocks:
            self.extend_block(block)

    def is_redundant_pair(
        self, prev_instr: Instruction | None, instr: Instruction
    ) -> bool:
        if prev_instr is not None and instr.opname == "POP_TOP":
            if (
                prev_instr.opname == "LOAD_CONST"
                or prev_instr.opname == "LOAD_SMALL_INT"
            ):
                return True
            elif prev_instr.opname == "COPY" and prev_instr.oparg == 1:
                return True
        return False

    def remove_redundant_nops_and_pairs(self, optimizer: FlowGraphOptimizer) -> None:
        done = False
        while not done:
            done = True
            instr: Instruction | None = None
            for block in self.ordered_blocks:
                optimizer.clean_basic_block(block, -1)
                if block.is_jump_target:
                    instr = None

                for cur_instr in block.insts:
                    prev_instr = instr
                    instr = cur_instr

                    if self.is_redundant_pair(prev_instr, instr):
                        instr.set_to_nop()
                        assert prev_instr is not None
                        prev_instr.set_to_nop()
                        done = False

                if (instr and instr.is_jump(self.opcode)) or not block.has_fallthrough:
                    instr = None

    def optimizeCFG(self) -> None:
        """Optimize a well-formed CFG."""
        except_handlers = self.compute_except_handlers()

        self.label_exception_targets()

        assert self.stage == CLOSED, self.stage

        self.eliminate_empty_basic_blocks()

        self.inline_small_exit_blocks()

        optimizer = self.flow_graph_optimizer(self)
        for block in self.ordered_blocks:
            optimizer.optimize_basic_block(block)

        self.remove_redundant_nops_and_pairs(optimizer)

        self.inline_small_exit_blocks()

        for block in self.ordered_blocks:
            # remove redundant nops
            optimizer.clean_basic_block(block, -1)

        self.remove_unreachable_basic_blocks()
        self.eliminate_empty_basic_blocks()

        self.remove_redundant_jumps(optimizer, False)

        self.stage = OPTIMIZED

        self.remove_unused_consts()
        self.add_checks_for_loads_of_uninitialized_variables()
        self.insert_superinstructions()
        self.push_cold_blocks_to_end(except_handlers, optimizer)

    def insert_superinstructions(self) -> None:
        # No super instructions on 3.12
        pass

    def build_cell_fixed_offsets(self) -> list[int]:
        nlocals = len(self.varnames)
        ncellvars = len(self.cellvars)
        nfreevars = len(self.freevars)

        noffsets = ncellvars + nfreevars
        fixed = [nlocals + i for i in range(noffsets)]

        for varname in self.cellvars:
            cellindex = self.cellvars.get_index(varname)
            if varname in self.varnames:
                varindex = self.varnames.index(varname)
                fixed[cellindex] = varindex

        return fixed

    def get_generator_prefix(self) -> list[Instruction]:
        firstline = self.firstline or self.first_inst_lineno or 1
        loc = SrcLocation(firstline, firstline, -1, -1)
        return [
            Instruction("RETURN_GENERATOR", 0, loc=loc),
            Instruction("POP_TOP", 0),
        ]

    def insert_prefix_instructions(self, fixed_map: list[int]) -> None:
        to_insert = []

        if self.freevars:
            n_freevars = len(self.freevars)
            to_insert.append(
                Instruction("COPY_FREE_VARS", n_freevars, ioparg=n_freevars)
            )

        if self.cellvars:
            # self.cellvars has the cells out of order so we sort them before
            # adding the MAKE_CELL instructions.  Note that we adjust for arg
            # cells, which come first.
            nvars = len(self.cellvars) + len(self.varnames)
            sorted = [0] * nvars
            for i in range(len(self.cellvars)):
                sorted[fixed_map[i]] = i + 1

            # varnames come first
            n_used = i = 0
            while n_used < len(self.cellvars):
                index = sorted[i] - 1
                if index != -1:
                    to_insert.append(Instruction("MAKE_CELL", index, ioparg=index))
                    n_used += 1
                i += 1

        if self.gen_kind is not None:
            to_insert.extend(self.get_generator_prefix())

        if to_insert:
            self.entry.insts[0:0] = to_insert

    def fix_cell_offsets(self, fixed_map: list[int]) -> int:
        nlocals = len(self.varnames)
        ncellvars = len(self.cellvars)
        nfreevars = len(self.freevars)
        noffsets = ncellvars + nfreevars

        # First deal with duplicates (arg cells).
        num_dropped = 0
        for i in range(noffsets):
            if fixed_map[i] == i + nlocals:
                fixed_map[i] -= num_dropped
            else:
                # It was a duplicate (cell/arg).
                num_dropped += 1

        # Then update offsets, either relative to locals or by cell2arg.
        for b in self.ordered_blocks:
            for instr in b.insts:
                if instr.opname in (
                    "MAKE_CELL",
                    "LOAD_CLOSURE",
                    "LOAD_DEREF",
                    "STORE_DEREF",
                    "DELETE_DEREF",
                    "LOAD_FROM_DICT_OR_DEREF",
                ):
                    oldoffset = instr.ioparg
                    assert oldoffset >= 0
                    assert oldoffset < noffsets, (
                        f"{instr.opname} {self.name} {self.firstline} {instr.oparg}"
                    )
                    assert fixed_map[oldoffset] >= 0
                    if isinstance(instr.oparg, int):
                        # Only update oparg here if it's already in the int-form
                        # so that we can assert on the string form in the sbs
                        # tests.
                        instr.oparg = fixed_map[oldoffset]
                    instr.ioparg = fixed_map[oldoffset]
        return num_dropped

    def prepare_localsplus(self) -> int:
        nlocals = len(self.varnames)
        ncellvars = len(self.cellvars)
        nfreevars = len(self.freevars)
        nlocalsplus = nlocals + ncellvars + nfreevars
        fixed_map = self.build_cell_fixed_offsets()
        # This must be called before fix_cell_offsets().
        self.insert_prefix_instructions(fixed_map)
        num_dropped = self.fix_cell_offsets(fixed_map)
        assert num_dropped >= 0
        nlocalsplus -= num_dropped
        assert nlocalsplus >= 0
        return nlocalsplus

    def instrsize(self, instr: Instruction, oparg: int) -> int:
        opname = instr.opname
        opcode_index = opcodes_opcode.opmap[opname]
        if opcode_index >= len(_inline_cache_entries):
            # T190611021: This should never happen as we should remove pseudo
            # instructions, but we are still missing some functionality
            # like zero-cost exceptions so we emit things like END_FINALLY
            base_size = 0
        else:
            base_size = _inline_cache_entries[opcode_index]
        if oparg <= 0xFF:
            return 1 + base_size
        elif oparg <= 0xFFFF:
            return 2 + base_size
        elif oparg <= 0xFFFFFF:
            return 3 + base_size
        else:
            return 4 + base_size

    _reversed_jumps: dict[str, str] = {
        "POP_JUMP_IF_NOT_NONE": "POP_JUMP_IF_NONE",
        "POP_JUMP_IF_NONE": "POP_JUMP_IF_NOT_NONE",
        "POP_JUMP_IF_FALSE": "POP_JUMP_IF_TRUE",
        "POP_JUMP_IF_TRUE": "POP_JUMP_IF_FALSE",
        "POP_JUMP_IF_NONZERO": "POP_JUMP_IF_ZERO",
        "POP_JUMP_IF_ZERO": "POP_JUMP_IF_NONZERO",
        "JUMP_IF_TRUE": "JUMP_IF_FALSE",
        "JUMP_IF_FALSE": "JUMP_IF_TRUE",
    }

    def normalize_jumps_in_block(
        self, block: Block, seen_blocks: set[Block]
    ) -> Block | None:
        last = block.insts[-1]
        if not last.is_jump(self.opcode):
            return
        target = last.target
        assert target is not None
        is_forward = target.bid not in seen_blocks
        assert last.opname != "JUMP_FOWARD"
        if last.opname == "JUMP":
            last.opname = "JUMP_FORWARD" if is_forward else "JUMP_BACKWARD"
            return
        elif last.opname == "JUMP_NO_INTERRUPT":
            last.opname = "JUMP_FORWARD" if is_forward else "JUMP_BACKWARD_NO_INTERRUPT"
            return

        if is_forward:
            return

        # transform 'conditional jump T' to
        # 'reversed_jump b_next' followed by 'jump_backwards T'
        backwards_jump = self.newBlock("backwards_jump")
        backwards_jump.bid = self.get_new_block_id()
        self.current = backwards_jump
        # cpython has `JUMP(target)` here, but it will always get inserted as
        # the next block in the loop and then transformed to JUMP_BACKWARD by
        # the above code, since is_forward(target) won't have changed.
        self.emit_with_loc("JUMP_BACKWARD", target, last.loc)
        last.opname = self._reversed_jumps[last.opname]
        last.target = block.next
        block.insert_next(backwards_jump)
        return backwards_jump

    def normalize_jumps(self) -> None:
        seen_blocks = set()
        new_blocks = {}
        for block in self.ordered_blocks:
            seen_blocks.add(block.bid)
            if block.insts:
                ret = self.normalize_jumps_in_block(block, seen_blocks)
                new_blocks[block] = ret
        new_ordered = []
        for block in self.ordered_blocks:
            new_ordered.append(block)
            if to_add := new_blocks.get(block):
                new_ordered.append(to_add)
        self.ordered_blocks = new_ordered

    def emit_inline_cache(
        self, opcode: str, addCode: Callable[[int, int], None]
    ) -> None:
        opcode_index = opcodes_opcode.opmap[opcode]
        if opcode_index < len(_inline_cache_entries):
            base_size = _inline_cache_entries[opcode_index]
        else:
            base_size = 0
        for _i in range(base_size):
            addCode(0, 0)

    def make_line_table(self) -> bytes:
        lpostab = LinePositionTable(self.firstline)

        loc = NO_LOCATION
        size = 0
        prev = None
        for t in reversed(self.insts):
            if t.loc is NEXT_LOCATION:
                if t.is_jump(self.opcode) or t.opname in SCOPE_EXIT_OPCODES:
                    t.loc = NO_LOCATION
                else:
                    assert prev is not None
                    t.loc = prev.loc
            prev = t

        for t in self.insts:
            if t.loc != loc:
                lpostab.emit_location(loc, size)
                loc = t.loc
                size = 0

            # The size is in terms of code units
            size += self.instrsize(t, t.ioparg)

        # Since the linetable format writes the end offset of bytecodes, we can't commit the
        # last write until all the instructions are iterated over.
        lpostab.emit_location(loc, size)
        return lpostab.getTable()

    def make_exception_table(self) -> bytes:
        exception_table = ExceptionTable()
        ioffset = 0
        handler = None
        start = -1
        for instr in self.insts:
            if handler != instr.exc_handler:
                # We have hit a boundary where instr.exc_handler has changed
                if handler:
                    exception_table.emit_entry(start, ioffset, handler)
                start = ioffset
                handler = instr.exc_handler
            ioffset += self.instrsize(instr, instr.ioparg)
        if handler:
            exception_table.emit_entry(start, ioffset, handler)
        return exception_table.getTable()

    def convert_pseudo_ops(self) -> None:
        # TASK(T190611021): The graph is actually in the FINAL stage, which
        # seems wrong because we're still modifying the opcodes.
        # assert self.stage == ACTIVE, self.stage

        for block in self.ordered_blocks:
            for instr in block.insts:
                if instr.opname in SETUP_OPCODES or instr.opname == "POP_BLOCK":
                    instr.set_to_nop()
                elif instr.opname == "STORE_FAST_MAYBE_NULL":
                    instr.opname = "STORE_FAST"

        # Remove redundant NOPs added in the previous pass
        optimizer = self.flow_graph_optimizer(self)
        for block in self.ordered_blocks:
            optimizer.clean_basic_block(block, -1)

    def assemble_final_code(self) -> None:
        """Finish assembling code object components from the final graph."""
        # see compile.c :: optimize_and_assemble_code_unit()
        self.finalize()
        assert self.stage == FINAL, self.stage

        # TASK(T206903352): We need to pass the return value to make_code at
        # some point.
        self.prepare_localsplus()
        self.compute_stack_depth()
        self.convert_pseudo_ops()

        self.flatten_graph()
        assert self.stage == FLAT, self.stage
        # see assemble.c :: _PyAssemble_MakeCodeObject()
        self.bytecode = self.make_byte_code()
        self.line_table = self.make_line_table()
        self.exception_table = self.make_exception_table()

    def getCode(self) -> CodeType:
        """Get a Python code object"""
        self.assemble_final_code()
        bytecode = self.bytecode
        assert bytecode is not None
        line_table = self.line_table
        assert line_table is not None
        exception_table = self.exception_table
        assert exception_table is not None
        assert self.stage == DONE, self.stage
        return self.new_code_object(bytecode, line_table, exception_table)

    def new_code_object(
        self, code: bytes, lnotab: bytes, exception_table: bytes
    ) -> CodeType:
        assert self.stage == DONE, self.stage

        nlocals = len(self.varnames)
        firstline = self.firstline
        # For module, .firstline is initially not set, and should be first
        # line with actual bytecode instruction (skipping docstring, optimized
        # out instructions, etc.)
        if not firstline:
            firstline = self.first_inst_lineno
        # If no real instruction, fallback to 1
        if not firstline:
            firstline = 1

        consts = self.getConsts()
        consts = consts + tuple(self.extra_consts)
        return self.make_code(nlocals, code, consts, firstline, lnotab, exception_table)

    def make_code(
        self,
        nlocals: int,
        code: bytes,
        consts: tuple[object, ...],
        firstline: int,
        lnotab: bytes,
        exception_table: bytes,
    ) -> CodeType:
        # pyre-ignore[19]: Too many arguments (this is right for 3.12)
        return CodeType(
            len(self.args),
            self.posonlyargs,
            len(self.kwonlyargs),
            nlocals,
            self.stacksize,
            self.flags,
            code,
            consts,
            tuple(self.names),
            tuple(self.varnames),
            self.filename,
            self.name,
            self.qualname,
            firstline,
            lnotab,
            exception_table,
            tuple(self.freevars),
            tuple(self.cellvars),
        )

    def _convert_LOAD_ATTR(self: PyFlowGraph, arg: object) -> int:
        # 3.12 uses the low-bit to indicate that the LOAD_ATTR is
        # part of a LOAD_ATTR/CALL sequence which loads two values,
        # the first being NULL or the object instance and the 2nd
        # being the method to be called.
        if isinstance(arg, tuple):
            return (self.names.get_index(arg[0]) << 1) | arg[1]

        assert isinstance(arg, str)
        return self.names.get_index(arg) << 1

    def _convert_LOAD_GLOBAL(self: PyFlowGraph, arg: object) -> int:
        assert isinstance(arg, str)
        return self.names.get_index(arg) << 1

    COMPARISON_UNORDERED = 1
    COMPARISON_LESS_THAN = 2
    COMPARISON_GREATER_THAN = 4
    COMPARISON_EQUALS = 8
    COMPARISON_NOT_EQUALS: int = (
        COMPARISON_UNORDERED | COMPARISON_LESS_THAN | COMPARISON_GREATER_THAN
    )

    COMPARE_MASKS: dict[str, int] = {
        "<": COMPARISON_LESS_THAN,
        "<=": COMPARISON_LESS_THAN | COMPARISON_EQUALS,
        "==": COMPARISON_EQUALS,
        "!=": COMPARISON_NOT_EQUALS,
        ">": COMPARISON_GREATER_THAN,
        ">=": COMPARISON_GREATER_THAN | COMPARISON_EQUALS,
    }

    def _convert_COMAPRE_OP(self: PyFlowGraph, arg: object) -> int:
        assert isinstance(arg, str)
        return self.opcode.CMP_OP.index(arg) << 4 | PyFlowGraph312.COMPARE_MASKS[arg]

    def _convert_LOAD_CLOSURE(self: PyFlowGraph, oparg: object) -> int:
        # __class__ and __classdict__ are special cased to be cell vars in classes
        # in get_ref_type in compile.c
        if isinstance(self.scope, ClassScope) and oparg in (
            "__class__",
            "__classdict__",
        ):
            return self.closure.get_index(oparg)

        assert isinstance(oparg, str)
        if oparg in self.freevars:
            return self.freevars.get_index(oparg) + len(self.cellvars)
        return self.closure.get_index(oparg)

    _converters: dict[str, Callable[[PyFlowGraph, object], int]] = {
        **PyFlowGraph._converters,
        "LOAD_ATTR": _convert_LOAD_ATTR,
        "LOAD_GLOBAL": _convert_LOAD_GLOBAL,
        "COMPARE_OP": _convert_COMAPRE_OP,
        "KW_NAMES": PyFlowGraph._convert_LOAD_CONST,
        "EAGER_IMPORT_NAME": PyFlowGraph._convert_NAME,
        "LOAD_CLOSURE": _convert_LOAD_CLOSURE,
    }

    _const_opcodes: set[str] = set(PyFlowGraph._const_opcodes)
    _const_opcodes.add("RETURN_CONST")
    _const_opcodes.add("KW_NAMES")


class PyFlowGraphCinder312(PyFlowGraphCinderMixin, PyFlowGraph312):
    pass


class PyFlowGraph314(PyFlowGraph312):
    flow_graph_optimizer = FlowGraphOptimizer314
    _constant_idx: dict[object, int] = {
        AssertionError: 0,
        NotImplementedError: 1,
        tuple: 2,
        all: 3,
        any: 4,
        list: 5,
        set: 6,
    }
    _load_special_idx = {
        "__enter__": 0,
        "__exit__": 1,
        "__aenter__": 2,
        "__aexit__": 3,
    }

    END_SEND_OFFSET = 5

    def emit_format_value(self, format: int = -1) -> None:
        if format != -1:
            self.emit("CONVERT_VALUE", format)
        self.emit("FORMAT_SIMPLE")

    def get_stack_effects(self, opname: str, oparg: object, jump: bool) -> int:
        res = self.opcode.stack_effect_raw(opname, oparg, jump)
        if opname in SETUP_OPCODES and not jump:
            return 0

        return res

    def flatten_jump(self, inst: Instruction, pc: int) -> int:
        res = super().flatten_jump(inst, pc)
        if inst.opname == "END_ASYNC_FOR":
            # sys.monitoring needs to be able to find the matching END_SEND
            # but the target is the SEND, so we adjust it here.
            res -= self.END_SEND_OFFSET

        return res

    def mark_except_handlers(self) -> None:
        for block in self.ordered_blocks:
            for instr in block.insts:
                if instr.opname in SETUP_OPCODES:
                    target = instr.target
                    assert target is not None, "SETUP_* opcodes all have targets"
                    target.is_exc_handler = True
                    break

    def push_cold_blocks_to_end(
        self, except_handlers: set[Block] | None, optimizer: FlowGraphOptimizer
    ) -> None:
        warm = self.compute_warm()
        cold = self.compute_cold(warm)

        # If we have a cold block with fallthrough to a warm block, add
        # an explicit jump instead of fallthrough
        for block in list(self.ordered_blocks):
            if block in cold and block.has_fallthrough and block.next in warm:
                explicit_jump = self.make_explicit_jump_block()
                explicit_jump.bid = self.get_new_block_id()
                explicit_jump.num_predecessors = 1
                cold.add(explicit_jump)
                self.current = explicit_jump

                next_block = block.next
                assert next_block is not None

                self.emit_jump_forward_noline(next_block)
                self.ordered_blocks.insert(
                    self.ordered_blocks.index(block) + 1, explicit_jump
                )

                explicit_jump.next = block.next
                explicit_jump.has_fallthrough = False
                block.next = explicit_jump

        new_ordered = []
        to_end = []
        prev = None
        for block in self.ordered_blocks:
            if block in warm:
                new_ordered.append(block)
                if prev is not None:
                    prev.next = block
                block.prev = prev
                prev = block
            else:
                to_end.append(block)

        for block in to_end:
            prev.next = block
            block.prev = prev
            prev = block

        block.next = None

        self.ordered_blocks = new_ordered + to_end
        if to_end:
            self.remove_redundant_nops_and_jumps(optimizer)

    def has_fallthrough(self, block: Block) -> bool:
        if not block.insts:
            return True

        last = block.insts[-1]
        return (
            last.opname not in UNCONDITIONAL_JUMP_OPCODES
            and last.opname not in SCOPE_EXIT_OPCODES
        )

    def compute_cold(self, warm: set[Block]) -> set[Block]:
        stack = []
        cold = set()
        visited = set()

        for block in self.ordered_blocks:
            if block.is_exc_handler:
                assert block not in warm
                stack.append(block)

        for block in stack:
            cold.add(block)
            next = block.next
            if next is not None and self.has_fallthrough(block):
                if next not in warm and next not in visited:
                    stack.append(next)
                    visited.add(next)

            for instr in block.insts:
                if instr.is_jump(self.opcode):
                    target = instr.target
                    if target not in warm and target not in visited:
                        stack.append(target)
                        visited.add(target)
        return cold

    def flatten_graph(self) -> None:
        # Init the ioparg of jump instructions to their index into
        # the instruction sequence. This matches CPython's behavior
        # in resolve_jump_offsets where i_target is initialized from
        # i_oparg but i_oparg is not reset to zero.
        offset = 0
        label_map: dict[Block, int] = {}
        for b in self.getBlocksInOrder():
            label_map[b] = offset
            offset += len(b.insts)

        for b in self.getBlocksInOrder():
            for inst in b.getInstructions():
                if inst.is_jump(self.opcode):
                    assert inst.target is not None, inst
                    inst.ioparg = label_map[inst.target]

        super().flatten_graph()

    # The following bits are chosen so that the value of
    # COMPARSION_BIT(left, right)
    # masked by the values below will be non-zero if the
    # comparison is true, and zero if it is false

    # This is for values that are unordered, ie. NaN, not types that are unordered, e.g. sets
    COMPARISON_UNORDERED = 1

    COMPARISON_LESS_THAN = 2
    COMPARISON_GREATER_THAN = 4
    COMPARISON_EQUALS = 8

    COMPARISON_NOT_EQUALS: int = (
        COMPARISON_UNORDERED | COMPARISON_LESS_THAN | COMPARISON_GREATER_THAN
    )

    COMPARE_MASKS: dict[str, int] = {
        "<": COMPARISON_LESS_THAN,
        "<=": COMPARISON_LESS_THAN | COMPARISON_EQUALS,
        "==": COMPARISON_EQUALS,
        "!=": COMPARISON_NOT_EQUALS,
        ">": COMPARISON_GREATER_THAN,
        ">=": COMPARISON_GREATER_THAN | COMPARISON_EQUALS,
    }

    def propagate_line_numbers(self) -> None:
        self.duplicate_exits_without_lineno()
        super().propagate_line_numbers()

    def has_eval_break(self, target: Block) -> bool:
        for inst in target.insts:
            if inst.opname in ["JUMP", "RESUME", "CALL", "JUMP_BACKWARD"]:
                return True
        return False

    def is_exit_without_line_number(self, target: Block) -> bool:
        if target.exits or self.has_eval_break(target):
            return target.has_no_lineno()

        return False

    @property
    def initial_stack_depth(self) -> int:
        return 0

    def initializeConsts(self) -> None:
        # Docstring is first entry in co_consts for normal functions
        # (Other types of code objects deal with docstrings in different
        # manner, e.g. lambdas and comprehensions don't have docstrings,
        # classes store them as __doc__ attribute.
        if self.docstring is not None and not self.klass:
            self.consts[self.get_const_key(self.docstring)] = 0

    def _convert_compare_op(self: PyFlowGraph, arg: object) -> int:
        # cmp goes in top three bits of the oparg, while the low four bits are used
        # by quickened versions of this opcode to store the comparison mask. The
        # fifth-lowest bit indicates whether the result should be converted to bool
        # and is set later):
        assert isinstance(arg, str)
        return self.opcode.CMP_OP.index(arg) << 5 | PyFlowGraph314.COMPARE_MASKS[arg]

    _converters: dict[str, Callable[[PyFlowGraph, object], int]] = {
        **PyFlowGraph312._converters,
        "LOAD_COMMON_CONSTANT": lambda self, val: PyFlowGraph314._constant_idx[val],
        "LOAD_SPECIAL": lambda self, val: PyFlowGraph314._load_special_idx[val],
        "COMPARE_OP": _convert_compare_op,
    }

    def get_ext_oparg(self, inst: Instruction) -> int:
        pushed = self.opcode.get_num_pushed(inst.opname, inst.oparg)
        popped = self.opcode.get_num_popped(inst.opname, inst.oparg)
        assert pushed < 4, pushed
        return popped << 2 | pushed

    def instrsize(self, instr: Instruction, oparg: int) -> int:
        opname = instr.opname
        base_size = _inline_cache_entries.get(opname, 0)
        if opname in STATIC_OPCODES:
            # extended opcode
            base_size += 1
            extoparg = self.get_ext_oparg(instr)
            while extoparg >= 256:
                extoparg >>= 8
                base_size += 1

        if oparg <= 0xFF:
            return 1 + base_size
        elif oparg <= 0xFFFF:
            return 2 + base_size
        elif oparg <= 0xFFFFFF:
            return 3 + base_size
        else:
            return 4 + base_size

    def make_byte_code(self) -> bytes:
        assert self.stage == FLAT, self.stage

        code: bytearray = bytearray()

        def addCode(opcode: int, oparg: int) -> None:
            assert opcode < 256, opcode
            assert oparg < 256, oparg
            code.append(opcode)
            code.append(oparg)

        for t in self.insts:
            if t.opname in STATIC_OPCODES:
                extoparg = self.get_ext_oparg(t)

                if extoparg > 0xFFFFFF:
                    addCode(self.opcode.EXTENDED_ARG, (extoparg >> 24) & 0xFF)
                if extoparg > 0xFFFF:
                    addCode(self.opcode.EXTENDED_ARG, (extoparg >> 16) & 0xFF)
                if extoparg > 0xFF:
                    addCode(self.opcode.EXTENDED_ARG, (extoparg >> 8) & 0xFF)
                addCode(self.opcode.EXTENDED_OPCODE, extoparg & 0xFF)

            oparg = t.ioparg
            assert 0 <= oparg <= 0xFFFFFFFF, oparg
            if oparg > 0xFFFFFF:
                addCode(self.opcode.EXTENDED_ARG, (oparg >> 24) & 0xFF)
            if oparg > 0xFFFF:
                addCode(self.opcode.EXTENDED_ARG, (oparg >> 16) & 0xFF)
            if oparg > 0xFF:
                addCode(self.opcode.EXTENDED_ARG, (oparg >> 8) & 0xFF)
            addCode(self.opcode.opmap[t.opname], oparg & 0xFF)
            self.emit_inline_cache(t.opname, addCode)

        self.stage = DONE
        return bytes(code)

    def emit_inline_cache(
        self, opcode: str, addCode: Callable[[int, int], None]
    ) -> None:
        base_size = _inline_cache_entries.get(opcode, 0)
        for _i in range(base_size):
            addCode(0, 0)

    def convert_pseudo_conditional_jumps(self) -> None:
        for block in self.getBlocksInOrder():
            for i, inst in enumerate(block.insts):
                if inst.opname == "JUMP_IF_FALSE" or inst.opname == "JUMP_IF_TRUE":
                    inst.opname = (
                        "POP_JUMP_IF_FALSE"
                        if inst.opname == "JUMP_IF_FALSE"
                        else "POP_JUMP_IF_TRUE"
                    )
                    block.insts[i:i] = [
                        Instruction(
                            "COPY", 1, 1, inst.loc, exc_handler=inst.exc_handler
                        ),
                        Instruction(
                            "TO_BOOL", 0, 0, inst.loc, exc_handler=inst.exc_handler
                        ),
                    ]

    def unlink_unreachable_basic_blocks(self, reachable_blocks: set[int]) -> None:
        for block in self.ordered_blocks:
            if block.num_predecessors == 0:
                del block.insts[:]
                block.is_exc_handler = False
                block.has_fallthrough = True

    def get_target_block_for_exit(self, target: Block) -> Block:
        while not target.insts:
            assert target.next is not None
            target = target.next
        return target

    def inline_small_exit_blocks(self) -> None:
        changes = True
        # every change removes a jump, ensuring convergence
        while changes:
            changes = False
            for block in self.ordered_blocks:
                if self.extend_block(block):
                    changes = True

    def should_inline_block(self, block: Block) -> bool:
        if block.exits and len(block.insts) <= MAX_COPY_SIZE:
            return True

        if block.has_fallthrough:
            return False

        # We can only inline blocks with no line numbers.
        for inst in block.insts:
            # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
            if inst.loc.lineno >= 0:
                return False
        return True

    def extend_block(self, block: Block) -> bool:
        """If this block ends with an unconditional jump to an exit block,
        then remove the jump and extend this block with the target.
        """
        if len(block.insts) == 0:
            return False
        last = block.insts[-1]
        if last.opname not in UNCONDITIONAL_JUMP_OPCODES:
            return False
        target = last.target
        assert target is not None
        small_exit_block = target.exits and len(target.insts) <= MAX_COPY_SIZE
        no_line_no_fallthrough = not target.has_fallthrough and target.has_no_lineno()
        if not small_exit_block and not no_line_no_fallthrough:
            return False
        last = block.insts[-1]
        removed = last.opname
        last.set_to_nop()
        for instr in target.insts:
            block.insts.append(instr.copy())

        last = block.insts[-1]
        if last.opname in UNCONDITIONAL_JUMP_OPCODES and removed == "JUMP":
            # Make sure we don't lose eval breaker checks
            last.opname = "JUMP"

        block.next = None
        block.is_exit = True
        block.has_fallthrough = False
        return True

    def finalize(self) -> None:
        """Perform final optimizations and normalization of flow graph."""
        assert self.stage == ACTIVE, self.stage
        self.stage = CLOSED

        self.eliminate_empty_basic_blocks()

        block = self.ordered_blocks[0]
        for i, inst in enumerate(block.insts):
            if inst.opname == "ANNOTATIONS_PLACEHOLDER":
                if self.annotations_block is not None:
                    block.insts[i : i + 1] = self.annotations_block.insts
                else:
                    inst.set_to_nop_no_loc()
                break

        for block in self.ordered_blocks:
            self.normalize_basic_block(block)

        self.optimizeCFG()

        self.stage = CONSTS_CLOSED
        self.trim_unused_consts()
        self.duplicate_exits_without_lineno()
        self.propagate_line_numbers()
        self.firstline = self.firstline or self.first_inst_lineno or 1

        self.stage = ORDERED
        self.stage = FINAL

    def assemble_final_code(self) -> None:
        """Finish assembling code object components from the final graph."""
        # see compile.c :: optimize_and_assemble_code_unit()
        self.finalize()
        assert self.stage == FINAL, self.stage

        self.convert_pseudo_conditional_jumps()
        self.compute_stack_depth()

        # TASK(T206903352): We need to pass the return value to make_code at
        # some point.
        self.prepare_localsplus()
        self.convert_pseudo_ops()

        self.normalize_jumps()

        self.resolve_unconditional_jumps()

        self.optimize_load_fast()

        self.flatten_graph()
        assert self.stage == FLAT, self.stage
        # see assemble.c :: _PyAssemble_MakeCodeObject()
        self.bytecode = self.make_byte_code()
        self.line_table = self.make_line_table()
        self.exception_table = self.make_exception_table()

    def convert_pseudo_ops(self) -> None:
        # TASK(T190611021): The graph is actually in the FINAL stage, which
        # seems wrong because we're still modifying the opcodes.
        # assert self.stage == ACTIVE, self.stage

        for block in self.ordered_blocks:
            for instr in block.insts:
                if instr.opname in SETUP_OPCODES or instr.opname == "POP_BLOCK":
                    instr.set_to_nop()
                elif instr.opname == "STORE_FAST_MAYBE_NULL":
                    instr.opname = "STORE_FAST"
                elif instr.opname == "LOAD_CLOSURE":
                    instr.opname = "LOAD_FAST"

        # Remove redundant NOPs added in the previous pass
        optimizer = self.flow_graph_optimizer(self)
        for block in self.ordered_blocks:
            optimizer.clean_basic_block(block, -1)

    def normalize_jumps_in_block(
        self, block: Block, seen_blocks: set[Block]
    ) -> Block | None:
        last = block.insts[-1]
        if last.opname not in self._reversed_jumps:
            return
        target = last.target
        assert target is not None
        is_forward = target.bid not in seen_blocks
        if is_forward:
            block.insts.append(Instruction("NOT_TAKEN", 0, 0, last.loc))
            return

        # transform 'conditional jump T' to
        # 'reversed_jump b_next' followed by 'jump_backwards T'
        backwards_jump = self.newBlock("backwards_jump")
        backwards_jump.bid = self.get_new_block_id()
        self.current = backwards_jump
        # cpython has `JUMP(target)` here, but it will always get inserted as
        # the next block in the loop and then transformed to JUMP_BACKWARD by
        # the above code, since is_forward(target) won't have changed.
        self.fetch_current().emit(Instruction("NOT_TAKEN", 0, 0, last.loc))
        self.emit_with_loc("JUMP_BACKWARD", target, last.loc)
        backwards_jump.startdepth = target.startdepth

        last.opname = self._reversed_jumps[last.opname]
        last.target = block.next

        block.insert_next(backwards_jump)
        return backwards_jump

    def resolve_unconditional_jumps(self) -> None:
        seen_blocks = set()

        for b in self.getBlocksInOrder():
            seen_blocks.add(b.bid)
            for inst in b.getInstructions():
                if inst.opname == "JUMP":
                    assert inst.target is not None
                    is_forward = inst.target.bid not in seen_blocks
                    inst.opname = "JUMP_FORWARD" if is_forward else "JUMP_BACKWARD"
                elif inst.opname == "JUMP_NO_INTERRUPT":
                    assert inst.target is not None
                    is_forward = inst.target.bid not in seen_blocks
                    inst.opname = (
                        "JUMP_FORWARD" if is_forward else "JUMP_BACKWARD_NO_INTERRUPT"
                    )

    # Helper functions for optimize_load_fast
    def kill_local(
        self, instr_flags: dict[int, int], refs: list[Ref], local: int
    ) -> None:
        """Mark references to a local as SUPPORT_KILLED."""
        for r in refs:
            if r.local == local:
                assert r.instr >= 0
                instr_flags[r.instr] |= SUPPORT_KILLED

    def store_local(
        self, instr_flags: dict[int, int], refs: list[Ref], local: int, r: Ref
    ) -> None:
        """Kill a local and mark a reference as STORED_AS_LOCAL."""
        self.kill_local(instr_flags, refs, local)
        if r.instr != DUMMY_INSTR:
            instr_flags[r.instr] |= STORED_AS_LOCAL

    def load_fast_push_block(
        self, stack: list[Block], target: Block, start_depth: int, visited: set[Block]
    ) -> None:
        """Add a block to the worklist for processing."""
        assert target.startdepth >= 0 and target.startdepth == start_depth, (
            f"{target.startdepth} {start_depth} {self.firstline}"
        )
        if target not in visited:
            visited.add(target)
            stack.append(target)

    def optimize_load_fast(self) -> None:
        """
        Strength reduce LOAD_FAST{_LOAD_FAST} instructions into faster variants that
        load borrowed references onto the operand stack.

        This is only safe when we can prove that the reference in the frame outlives
        the borrowed reference produced by the instruction.
        """
        stack = [self.entry]
        visited: set[Block] = set()

        self.entry.startdepth = 0
        refs: list[Ref] = []
        instr_flags = defaultdict(lambda: 0)  # Maps instruction index to flags
        while stack:
            block = stack.pop()
            visited.add(block)

            # Reset per-block state
            instr_flags.clear()

            # Reset the stack of refs. We don't track references on the stack
            # across basic blocks, but the bytecode will expect their presence.
            # Add dummy references as necessary.
            refs.clear()
            for _ in range(block.startdepth):
                refs.append(Ref(DUMMY_INSTR, NOT_LOCAL))
            for i, instr in enumerate(block.insts):
                opcode = instr.opname
                ioparg = instr.ioparg
                oparg = instr.oparg

                # Process instruction based on opcode
                if opcode == "DELETE_FAST":
                    self.kill_local(instr_flags, refs, ioparg)

                elif opcode == "LOAD_FAST":
                    refs.append(Ref(i, ioparg))

                elif opcode == "LOAD_FAST_AND_CLEAR":
                    self.kill_local(instr_flags, refs, ioparg)
                    refs.append(Ref(i, ioparg))

                elif opcode == "LOAD_FAST_LOAD_FAST":
                    # Extract the two locals from the combined oparg
                    local1 = ioparg >> 4
                    local2 = ioparg & 0xF
                    refs.append(Ref(i, local1))
                    refs.append(Ref(i, local2))

                elif opcode == "STORE_FAST":
                    r = refs.pop()
                    self.store_local(instr_flags, refs, ioparg, r)

                elif opcode == "STORE_FAST_LOAD_FAST":
                    # STORE_FAST
                    r = refs.pop()
                    local1 = ioparg >> 4
                    local2 = ioparg & 15
                    self.store_local(instr_flags, refs, local1, r)
                    # LOAD_FAST
                    refs.append(Ref(i, local2))

                elif opcode == "STORE_FAST_STORE_FAST":
                    # STORE_FAST
                    r = refs.pop()
                    local1 = ioparg >> 4
                    self.store_local(instr_flags, refs, local1, r)
                    # STORE_FAST
                    r = refs.pop()
                    local2 = ioparg & 15
                    self.store_local(instr_flags, refs, local2, r)

                # Handle stack manipulation opcodes
                elif opcode == "COPY":
                    assert ioparg > 0
                    idx = len(refs) - ioparg
                    r = refs[idx]
                    refs.append(Ref(r.instr, r.local))

                elif opcode == "SWAP":
                    assert ioparg >= 2
                    idx = len(refs) - ioparg
                    refs[idx], refs[-1] = refs[-1], refs[idx]

                # We treat opcodes that do not consume all of their inputs on
                # a case by case basis, as we have no generic way of knowing
                # how many inputs should be left on the stack.

                # Opcodes that consume no inputs
                elif opcode in NO_INPUT_INSTRS:
                    delta = self.opcode.stack_effect_raw(opcode, oparg, False)
                    assert delta >= 0
                    for _ in range(delta):
                        refs.append(Ref(i, NOT_LOCAL))

                # Opcodes that consume some inputs and push no new values
                elif opcode in (
                    "DICT_MERGE",
                    "DICT_UPDATE",
                    "LIST_APPEND",
                    "LIST_EXTEND",
                    "MAP_ADD",
                    "RERAISE",
                    "SET_ADD",
                    "SET_UPDATE",
                ):
                    num_popped = self.opcode.get_num_popped(opcode, oparg)
                    num_pushed = self.opcode.get_num_pushed(opcode, oparg)
                    net_popped = num_popped - num_pushed
                    assert net_popped > 0
                    for _ in range(net_popped):
                        refs.pop()

                elif opcode in ("END_SEND", "SET_FUNCTION_ATTRIBUTE"):
                    assert self.opcode.stack_effect_raw(opcode, oparg, False) == -1
                    tos = refs.pop()
                    refs.pop()  # Pop the second item
                    refs.append(Ref(tos.instr, tos.local))  # Push back the top item

                # Handle opcodes that consume some inputs and push new values
                elif opcode == "CHECK_EXC_MATCH":
                    refs.pop()
                    refs.append(Ref(i, NOT_LOCAL))

                elif opcode == "FOR_ITER":
                    if instr.target:
                        self.load_fast_push_block(
                            stack, instr.target, len(refs) + 1, visited
                        )
                    refs.append(Ref(i, NOT_LOCAL))

                elif opcode in ("LOAD_ATTR", "LOAD_SUPER_ATTR"):
                    self_ref = refs.pop()
                    if opcode == "LOAD_SUPER_ATTR":
                        refs.pop()  # Pop super type
                        refs.pop()  # Pop super object
                    refs.append(Ref(i, NOT_LOCAL))
                    if ioparg & 1:
                        # A method call; conservatively assume self is pushed back
                        refs.append(Ref(self_ref.instr, self_ref.local))

                elif opcode in ("LOAD_SPECIAL", "PUSH_EXC_INFO"):
                    tos = refs.pop()
                    refs.append(Ref(i, NOT_LOCAL))
                    refs.append(Ref(tos.instr, tos.local))

                elif opcode == "SEND":
                    assert instr.target
                    self.load_fast_push_block(stack, instr.target, len(refs), visited)
                    refs.pop()
                    refs.append(Ref(i, NOT_LOCAL))

                # Opcodes that consume all of their inputs
                else:
                    num_popped = self.opcode.get_num_popped(opcode, oparg)
                    num_pushed = self.opcode.get_num_pushed(opcode, oparg)

                    if instr.target and self.opcode.has_jump(self.opcode.opmap[opcode]):
                        self.load_fast_push_block(
                            stack,
                            instr.target,
                            len(refs) - num_popped + num_pushed,
                            visited,
                        )

                    if opcode not in SETUP_OPCODES:
                        # Block push opcodes only affect the stack when jumping to the target
                        for _ in range(num_popped):
                            refs.pop()
                        for _ in range(num_pushed):
                            refs.append(Ref(i, NOT_LOCAL))

            # Push fallthrough block
            if block.has_fallthrough and block.next:
                self.load_fast_push_block(stack, block.next, len(refs), visited)

            # Mark instructions that produce values that are on the stack at the end of the basic block
            for r in refs:
                if r.instr != -1:
                    instr_flags[r.instr] = instr_flags.get(r.instr, 0) | REF_UNCONSUMED

            # Optimize instructions
            for i, instr in enumerate(block.insts):
                if i not in instr_flags:
                    if instr.opname == "LOAD_FAST":
                        instr.opname = "LOAD_FAST_BORROW"
                    elif instr.opname == "LOAD_FAST_LOAD_FAST":
                        instr.opname = "LOAD_FAST_BORROW_LOAD_FAST_BORROW"

    def get_generator_prefix(self) -> list[Instruction]:
        firstline = self.firstline or self.first_inst_lineno or 1
        loc = SrcLocation(firstline, firstline, -1, -1)
        return [
            Instruction("RETURN_GENERATOR", 0, loc=loc),
            Instruction("POP_TOP", 0, loc=loc),
        ]

    def emit_jump_forward_noline(self, target: Block) -> None:
        self.emit_noline("JUMP_NO_INTERRUPT", target)

    def make_explicit_jump_block(self) -> Block:
        res = super().make_explicit_jump_block()
        res.num_predecessors = 1
        return res

    def label_exception_targets(self) -> None:
        def push_todo_block(block: Block) -> None:
            todo_stack.append(block)
            visited.add(block)

        todo_stack: list[Block] = [self.entry]
        visited: set[Block] = {self.entry}
        except_stack = []
        self.entry.except_stack = except_stack

        while todo_stack:
            block = todo_stack.pop()
            assert block in visited
            except_stack = block.except_stack
            block.except_stack = []
            handler = except_stack[-1] if except_stack else None
            last_yield_except_depth = -1
            for instr in block.insts:
                if instr.opname in SETUP_OPCODES:
                    target = instr.target
                    assert target, instr
                    if target not in visited:
                        # Copy the current except stack into the target's except stack
                        target.except_stack = list(except_stack)
                        push_todo_block(target)
                    handler = self.push_except_block(except_stack, instr)
                elif instr.opname == "POP_BLOCK":
                    except_stack.pop()
                    handler = except_stack[-1] if except_stack else None
                    instr.set_to_nop()
                elif instr.is_jump(self.opcode) and instr.opname != "END_ASYNC_FOR":
                    instr.exc_handler = handler
                    if instr.target not in visited:
                        target = instr.target
                        assert target
                        if block.has_fallthrough:
                            # Copy the current except stack into the block's except stack
                            target.except_stack = list(except_stack)
                        else:
                            # Move the current except stack to the block and start a new one
                            target.except_stack = except_stack
                            except_stack = []
                        push_todo_block(target)
                elif instr.opname == "YIELD_VALUE":
                    last_yield_except_depth = len(except_stack)
                    instr.exc_handler = handler
                elif instr.opname == "RESUME":
                    instr.exc_handler = handler
                    if instr.oparg != ResumeOparg.ScopeEntry:
                        if last_yield_except_depth == 1:
                            instr.ioparg |= ResumeOparg.Depth1Mask
                        last_yield_except_depth = -1
                elif instr.opname == "RETURN_GENERATOR":
                    instr.exc_handler = None
                else:
                    instr.exc_handler = handler

            if block.has_fallthrough and block.next and block.next not in visited:
                assert except_stack is not None
                block.next.except_stack = except_stack
                push_todo_block(block.next)

    def make_super_instruction(
        self, inst1: Instruction, inst2: Instruction, super_op: str
    ) -> None:
        # pyre-ignore[16]: lineno is maybe not defined on AST
        line1 = inst1.loc.lineno
        # pyre-ignore[16]: lineno is maybe not defined on AST
        line2 = inst2.loc.lineno
        # Skip if instructions are on different lines
        if line1 >= 0 and line2 >= 0 and line1 != line2:
            return

        if inst1.ioparg >= 16 or inst2.ioparg >= 16:
            return

        inst1.opname = super_op
        inst1.ioparg = (inst1.ioparg << 4) | inst2.ioparg
        inst2.set_to_nop()

    def insert_superinstructions(self) -> None:
        for block in self.ordered_blocks:
            for i, instr in enumerate(block.insts):
                if i + 1 == len(block.insts):
                    break

                next_instr = block.insts[i + 1]
                if instr.opname == "LOAD_FAST":
                    if next_instr.opname == "LOAD_FAST":
                        self.make_super_instruction(
                            instr, next_instr, "LOAD_FAST_LOAD_FAST"
                        )
                elif instr.opname == "STORE_FAST":
                    if next_instr.opname == "LOAD_FAST":
                        self.make_super_instruction(
                            instr, next_instr, "STORE_FAST_LOAD_FAST"
                        )
                    elif next_instr.opname == "STORE_FAST":
                        self.make_super_instruction(
                            instr, next_instr, "STORE_FAST_STORE_FAST"
                        )

    def remove_redundant_jumps(
        self, optimizer: FlowGraphOptimizer, clean: bool = True
    ) -> bool:
        # Delete jump instructions made redundant by previous step. If a non-empty
        # block ends with a jump instruction, check if the next non-empty block
        # reached through normal flow control is the target of that jump. If it
        # is, then the jump instruction is redundant and can be deleted.
        maybe_empty_blocks = False
        for block in self.ordered_blocks:
            if not block.insts:
                continue
            last = block.insts[-1]
            if last.opname not in UNCONDITIONAL_JUMP_OPCODES:
                continue
            target = last.target
            while not target.insts:
                target = target.next
            next = block.next
            while next and not next.insts:
                next = next.next
            if target == next:
                block.has_fallthrough = True
                last.set_to_nop()
                maybe_empty_blocks = True
        return maybe_empty_blocks

    def remove_redundant_nops_and_jumps(self, optimizer: FlowGraphOptimizer) -> None:
        while True:
            removed = False
            for block in self.ordered_blocks:
                removed |= optimizer.clean_basic_block(block, -1)

            removed |= self.remove_redundant_jumps(optimizer, False)
            if not removed:
                break

    def optimizeCFG(self) -> None:
        """Optimize a well-formed CFG."""
        self.mark_except_handlers()

        self.label_exception_targets()

        assert self.stage == CLOSED, self.stage

        self.inline_small_exit_blocks()

        self.remove_unreachable_basic_blocks()
        self.propagate_line_numbers()

        const_optimizer = FlowGraphConstOptimizer314(self)
        for block in self.ordered_blocks:
            const_optimizer.optimize_basic_block(block)

        optimizer = self.flow_graph_optimizer(self)
        for block in self.ordered_blocks:
            optimizer.optimize_basic_block(block)

        self.remove_redundant_nops_and_pairs(optimizer)

        for block in self.ordered_blocks:
            # remove redundant nops
            optimizer.clean_basic_block(block, -1)

        self.remove_redundant_nops_and_pairs(optimizer)
        self.remove_unreachable_basic_blocks()
        self.remove_redundant_nops_and_jumps(optimizer)

        self.stage = OPTIMIZED

        self.remove_unused_consts()
        self.add_checks_for_loads_of_uninitialized_variables()
        self.insert_superinstructions()
        self.push_cold_blocks_to_end(None, optimizer)
        self.propagate_line_numbers()

    _const_opcodes: set[str] = set(PyFlowGraph312._const_opcodes) | {"LOAD_SMALL_INT"}


class PyFlowGraph315(PyFlowGraph314):
    def maybe_propagate_location(
        self, instr: Instruction, loc: AST | SrcLocation
    ) -> None:
        if instr.lineno == NO_LOCATION.lineno:
            instr.loc = loc

    def get_generator_prefix(self) -> list[Instruction]:
        return []

    def propagate_line_numbers(self) -> None:
        """Propagate line numbers to instructions without."""
        self.duplicate_exits_without_lineno()
        for block in self.ordered_blocks:
            if not block.insts:
                continue
            prev_loc = NO_LOCATION
            for instr in block.insts:
                self.maybe_propagate_location(instr, prev_loc)
                prev_loc = instr.loc
            if block.has_fallthrough:
                next = block.next
                assert next
                if next.num_predecessors == 1 and next.insts:
                    self.maybe_propagate_location(next.insts[0], prev_loc)
            last_instr = block.insts[-1]
            if (
                last_instr.is_jump(self.opcode)
                and last_instr.opname not in SETUP_OPCODES
            ):
                # Only actual jumps, not exception handlers
                target = last_instr.target
                assert target
                while not target.insts and target.num_predecessors == 1:
                    target = target.next
                    assert target
                # CPython doesn't check target.insts and instead can write
                # to random memory...
                if target.num_predecessors == 1 and target.insts:
                    self.maybe_propagate_location(target.insts[0], prev_loc)


# Constants for reference tracking flags
SUPPORT_KILLED = (
    1  # The loaded reference is still on the stack when the local is killed
)
STORED_AS_LOCAL = 2  # The loaded reference is stored into a local
REF_UNCONSUMED = (
    4  # The loaded reference is still on the stack at the end of the basic block
)

# Special values for reference tracking
NOT_LOCAL = -1  # Reference does not refer to a local
DUMMY_INSTR = -1  # Reference was not produced by an instruction


@dataclass
class Ref:
    """Represents a reference on the stack."""

    instr: int  # Index of instruction that produced the reference or DUMMY_INSTR
    local: int  # The local to which the reference refers or NOT_LOCAL


class UninitializedVariableChecker:
    # Opcodes which may clear a variable
    clear_ops = (
        "DELETE_FAST",
        "LOAD_FAST_AND_CLEAR",
        "STORE_FAST_MAYBE_NULL",
    )
    # Opcodes which guarantee that a variable is stored
    stored_ops = ("STORE_FAST", "LOAD_FAST_CHECK")

    def __init__(self, flow_graph: PyFlowGraph312) -> None:
        self.flow_graph = flow_graph
        self.stack: list[Block] = []
        self.unsafe_locals: dict[Block, int] = {}
        self.visited: set[Block] = set()

    def check(self) -> None:
        nlocals = len(self.flow_graph.varnames)
        nparams = (
            len(self.flow_graph.args)
            + len(self.flow_graph.starargs)
            + len(self.flow_graph.kwonlyargs)
        )

        if nlocals == 0:
            return

        if nlocals > 64:
            # To avoid O(nlocals**2) compilation, locals beyond the first
            # 64 are only analyzed one basicblock at a time: initialization
            # info is not passed between basicblocks.
            self.fast_scan_many_locals(nlocals)
            nlocals = 64

        # First origin of being uninitialized:
        # The non-parameter locals in the entry block.
        start_mask = 0
        for i in range(nparams, nlocals):
            start_mask |= 1 << i

        self.maybe_push(self.flow_graph.entry, start_mask)

        # Second origin of being uninitialized:
        # There could be DELETE_FAST somewhere, so
        # be sure to scan each basicblock at least once.
        for b in self.flow_graph.ordered_blocks:
            self.scan_block_for_locals(b)

        # Now propagate the uncertainty from the origins we found: Use
        # LOAD_FAST_CHECK for any LOAD_FAST where the local could be undefined.
        while self.stack:
            block = self.stack.pop()
            self.visited.remove(block)
            self.scan_block_for_locals(block)

    def scan_block_for_locals(self, block: Block) -> None:
        unsafe_mask = self.unsafe_locals.get(block, 0)
        for instr in block.insts:
            if instr.exc_handler is not None:
                self.maybe_push(instr.exc_handler, unsafe_mask)

            if instr.ioparg >= 64:
                continue

            bit = 1 << instr.ioparg
            if instr.opname in self.clear_ops:
                unsafe_mask |= bit
            elif instr.opname in self.stored_ops:
                unsafe_mask &= ~bit
            elif instr.opname == "LOAD_FAST":
                if unsafe_mask & bit:
                    instr.opname = "LOAD_FAST_CHECK"
                unsafe_mask &= ~bit

        if block.next and block.has_fallthrough:
            self.maybe_push(block.next, unsafe_mask)

        if block.insts and block.insts[-1].is_jump(self.flow_graph.opcode):
            target = block.insts[-1].target
            assert target is not None
            self.maybe_push(target, unsafe_mask)

    def fast_scan_many_locals(self, nlocals: int) -> None:
        states = [0] * (nlocals - 64)
        block_num = 0
        # state[i - 64] == blocknum if local i is guaranteed to
        # be initialized, i.e., if it has had a previous LOAD_FAST or
        # STORE_FAST within that basicblock (not followed by
        # DELETE_FAST/LOAD_FAST_AND_CLEAR/STORE_FAST_MAYBE_NULL).
        for block in self.flow_graph.ordered_blocks:
            block_num += 1
            for instr in block.insts:
                if instr.ioparg < 64:
                    continue

                arg = instr.ioparg - 64
                if instr.opname in self.clear_ops:
                    states[arg] = block_num - 1
                elif instr.opname == "STORE_FAST":
                    states[arg] = block_num
                elif instr.opname == "LOAD_FAST":
                    if states[arg] != block_num:
                        instr.opname = "LOAD_FAST_CHECK"
                    states[arg] = block_num

    def maybe_push(self, block: Block, unsafe_mask: int) -> None:
        block_unsafe = self.unsafe_locals.get(block, 0)
        both = block_unsafe | unsafe_mask
        if block_unsafe != both:
            self.unsafe_locals[block] = both
            if block not in self.visited:
                self.stack.append(block)
                self.visited.add(block)


class LineAddrTable:
    """linetable / lnotab

    This class builds the linetable, which is documented in
    Objects/lnotab_notes.txt. Here's a brief recap:

    For each new lineno after the first one, two bytes are added to the
    linetable.  (In some cases, multiple two-byte entries are added.)  The first
    byte is the distance in bytes between the instruction for the current lineno
    and the next lineno.  The second byte is offset in line numbers.  If either
    offset is greater than 255, multiple two-byte entries are added -- see
    lnotab_notes.txt for the delicate details.

    """

    def __init__(self) -> None:
        self.current_line = 0
        self.prev_line = 0
        self.linetable: list[int] = []

    def setFirstLine(self, lineno: int) -> None:
        self.current_line = lineno
        self.prev_line = lineno

    def nextLine(self, lineno: int, start: int, end: int) -> None:
        assert lineno
        self.emitCurrentLine(start, end)

        if self.current_line >= 0:
            self.prev_line = self.current_line
        self.current_line = lineno

    def emitCurrentLine(self, start: int, end: int) -> None:
        # compute deltas
        addr_delta = end - start
        if not addr_delta:
            return
        if self.current_line < 0:
            line_delta = -128
        else:
            line_delta = self.current_line - self.prev_line
            while line_delta < -127 or 127 < line_delta:
                if line_delta < 0:
                    k = -127
                else:
                    k = 127
                self.push_entry(0, k)
                line_delta -= k

        while addr_delta > 254:
            self.push_entry(254, line_delta)
            line_delta = -128 if self.current_line < 0 else 0
            addr_delta -= 254

        assert -128 <= line_delta and line_delta <= 127
        self.push_entry(addr_delta, line_delta)

    def getTable(self) -> bytes:
        return bytes(self.linetable)

    def push_entry(self, addr_delta: int, line_delta: int) -> None:
        self.linetable.append(addr_delta)
        self.linetable.append(cast_signed_byte_to_unsigned(line_delta))


class CodeLocationInfoKind(IntEnum):
    SHORT0 = 0
    ONE_LINE0 = 10
    ONE_LINE1 = 11
    ONE_LINE2 = 12
    NO_COLUMNS = 13
    LONG = 14
    NONE = 15


class LinePositionTable:
    """Generates the Python 3.12 and later position table which tracks
    line numbers as well as column information."""

    def __init__(self, firstline: int) -> None:
        self.linetable = bytearray()
        self.lineno = firstline

    # https://github.com/python/cpython/blob/3.12/Python/assemble.c#L170
    # https://github.com/python/cpython/blob/3.12/Objects/locations.md
    def emit_location(self, loc: AST | SrcLocation, size: int) -> None:
        if size == 0:
            return

        while size > 8:
            self.write_entry(loc, 8)
            size -= 8

        self.write_entry(loc, size)

    def write_entry(self, loc: AST | SrcLocation, size: int) -> None:
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
        if loc.lineno < 0:
            return self.write_entry_no_location(size)

        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
        line_delta = loc.lineno - self.lineno
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
        #  `col_offset`.
        column = loc.col_offset
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
        #  `end_col_offset`.
        end_column = loc.end_col_offset
        assert isinstance(end_column, int)
        assert column >= -1
        assert end_column >= -1
        if column < 0 or end_column < 0:
            # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
            #  `end_lineno`.
            # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
            #  `lineno`.
            if loc.end_lineno == loc.lineno or loc.end_lineno == -1:
                self.write_no_column(size, line_delta)
                # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
                #  `lineno`.
                self.lineno = loc.lineno
                return
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
        #  `end_lineno`.
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
        elif loc.end_lineno == loc.lineno:
            if (
                line_delta == 0
                and column < 80
                and end_column - column < 16
                and end_column >= column
            ):
                return self.write_short_form(size, column, end_column)
            if 0 <= line_delta < 3 and column < 128 and end_column < 128:
                self.write_one_line_form(size, line_delta, column, end_column)
                # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
                #  `lineno`.
                self.lineno = loc.lineno
                return

        self.write_long_form(loc, size)
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
        self.lineno = loc.lineno

    def write_short_form(self, size: int, column: int, end_column: int) -> None:
        assert size > 0 and size <= 8
        column_low_bits = column & 7
        column_group = column >> 3
        assert column < 80
        assert end_column >= column
        assert end_column - column < 16
        self.write_first_byte(CodeLocationInfoKind.SHORT0 + column_group, size)
        # Start column / end column
        self.write_byte((column_low_bits << 4) | (end_column - column))

    def write_one_line_form(
        self, size: int, line_delta: int, column: int, end_column: int
    ) -> None:
        assert size > 0 and size <= 8
        assert line_delta >= 0 and line_delta < 3
        assert column < 128
        assert end_column < 128
        # Start line delta
        self.write_first_byte(CodeLocationInfoKind.ONE_LINE0 + line_delta, size)
        self.write_byte(column)  # Start column
        self.write_byte(end_column)  # End column

    def write_no_column(self, size: int, line_delta: int) -> None:
        self.write_first_byte(CodeLocationInfoKind.NO_COLUMNS, size)
        self.write_signed_varint(line_delta)  # Start line delta

    def write_long_form(self, loc: AST | SrcLocation, size: int) -> None:
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
        #  `end_lineno`.
        end_lineno = loc.end_lineno
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
        #  `end_col_offset`.
        end_col_offset = loc.end_col_offset

        assert size > 0 and size <= 8
        assert end_lineno is not None and end_col_offset is not None
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
        assert end_lineno >= loc.lineno

        self.write_first_byte(CodeLocationInfoKind.LONG, size)
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
        self.write_signed_varint(loc.lineno - self.lineno)  # Start line delta
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute `lineno`.
        self.write_varint(end_lineno - loc.lineno)  # End line delta
        # pyre-fixme[16]: Item `AST` of `AST | SrcLocation` has no attribute
        #  `col_offset`.
        self.write_varint(loc.col_offset + 1)  # Start column
        self.write_varint(end_col_offset + 1)  # End column

    def write_entry_no_location(self, size: int) -> None:
        self.write_first_byte(CodeLocationInfoKind.NONE, size)

    def write_varint(self, value: int) -> None:
        while value >= 64:
            self.linetable.append(0x40 | (value & 0x3F))
            value >>= 6

        self.linetable.append(value)

    def write_signed_varint(self, value: int) -> None:
        if value < 0:
            uval = ((-value) << 1) | 1
        else:
            uval = value << 1

        self.write_varint(uval)

    def write_first_byte(self, code: int, length: int) -> None:
        assert code & 0x0F == code
        self.linetable.append(0x80 | (code << 3) | (length - 1))

    def write_byte(self, code: int) -> None:
        self.linetable.append(code & 0xFF)

    def getTable(self) -> bytes:
        return bytes(self.linetable)


class ExceptionTable:
    """Generates the Python 3.12+ exception table."""

    # https://github.com/python/cpython/blob/3.12/Objects/exception_handling_notes.txt

    def __init__(self) -> None:
        self.exception_table: bytearray = bytearray()

    def getTable(self) -> bytes:
        return bytes(self.exception_table)

    def write_byte(self, byte: int) -> None:
        self.exception_table.append(byte)

    def emit_item(self, value: int, msb: int) -> None:
        assert (msb | 128) == 128
        assert value >= 0 and value < (1 << 30)
        CONTINUATION_BIT = 64

        if value > (1 << 24):
            self.write_byte((value >> 24) | CONTINUATION_BIT | msb)
            msb = 0
        for i in (18, 12, 6):
            if value >= (1 << i):
                v = (value >> i) & 0x3F
                self.write_byte(v | CONTINUATION_BIT | msb)
                msb = 0
        self.write_byte((value & 0x3F) | msb)

    def emit_entry(self, start: int, end: int, handler: Block) -> None:
        size = end - start
        assert end > start
        target = handler.offset
        depth = handler.startdepth - 1
        if handler.preserve_lasti:
            depth = depth - 1
        assert depth >= 0
        depth_lasti = (depth << 1) | int(handler.preserve_lasti)
        self.emit_item(start, (1 << 7))
        self.emit_item(size, 0)
        self.emit_item(target, 0)
        self.emit_item(depth_lasti, 0)

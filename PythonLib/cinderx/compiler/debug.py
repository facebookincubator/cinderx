"""Debugging output for various internal datatypes."""

# Marked pyre-unsafe so we don't need to import pyassem.py for the typedefs
# pyre-unsafe

from .opcodes import opcode


def str_of_oparg(instr) -> str:
    if instr.target is None:
        # This is not a block
        return str(instr.oparg)
    elif instr.target.label:
        # This is a labelled block
        return f"{instr.target.bid} ({instr.target.label})"
    else:
        # This is an unlabelled block
        return str(instr.target.bid)


def str_of_instr(instr, pc: int = 0) -> str:
    oparg = str_of_oparg(instr)
    opname = instr.opname
    return f"{pc:3} {instr.lineno} {opname} {oparg}"


def str_of_block_header(block) -> str:
    return repr(block)


def dump_instr(instr, pc: int = 0) -> None:
    print("    ", str_of_instr(instr, pc))


def dump_block(block, pc: int = 0) -> int:
    print(str_of_block_header(block))
    for instr in block.getInstructions():
        dump_instr(instr, pc)
        pc += opcode.CODEUNIT_SIZE
    return pc


def dump_graph(graph) -> None:
    pc = 0
    for block in graph.getBlocks():
        pc = dump_block(block, pc)

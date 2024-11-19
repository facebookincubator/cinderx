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


def str_of_instr(instr) -> str:
    oparg = str_of_oparg(instr)
    opname = instr.opname
    return f"{instr.lineno} {opname} {oparg}"


def str_of_block_header(block) -> str:
    return repr(block)


def str_of_stack_effect(instr) -> str:
    d1 = opcode.stack_effect_raw(instr.opname, instr.oparg, False)
    d2 = opcode.stack_effect_raw(instr.opname, instr.oparg, True)
    if d1 != d2:
        delta = f"{d1}/{d2}"
    else:
        delta = str(d1)
    return delta


def str_of_block_instr(instr, pc: int = 0, stack_effect: bool = False) -> str:
    delta = str_of_stack_effect(instr) if stack_effect else ""
    return f"{delta:>6} | {pc:3} {str_of_instr(instr)}"


def dump_block(block, pc: int = 0, stack_effect: bool = False) -> int:
    print(str_of_block_header(block))
    for instr in block.getInstructions():
        print("    ", str_of_block_instr(instr, pc, stack_effect))
        pc += opcode.CODEUNIT_SIZE
    return pc


def dump_graph(graph, stack_effect: bool = False) -> None:
    pc = 0
    for block in graph.getBlocks():
        pc = dump_block(block, pc, stack_effect)

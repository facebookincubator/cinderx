# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict
"""Debugging output for various internal datatypes."""

from __future__ import annotations

from typing import TYPE_CHECKING

from .opcodes import opcode

if TYPE_CHECKING:
    from .pyassem import Block, Instruction, PyFlowGraph


def str_of_oparg(instr: Instruction) -> str:
    if instr.target is None:
        # This is not a block
        return str(instr.oparg)
    elif instr.target.label:
        # This is a labelled block
        return f"{instr.target.bid} ({instr.target.label})"
    else:
        # This is an unlabelled block
        return str(instr.target.bid)


def str_of_instr(instr: Instruction) -> str:
    oparg = str_of_oparg(instr)
    opname = instr.opname
    return f"{instr.lineno} {opname} {oparg}"


def str_of_block_header(block: Block) -> str:
    return repr(block)


def str_of_stack_effect(instr: Instruction) -> str:
    d1 = opcode.stack_effect_raw(instr.opname, instr.oparg, False)
    d2 = opcode.stack_effect_raw(instr.opname, instr.oparg, True)
    if d1 != d2:
        delta = f"{d1}/{d2}"
    else:
        delta = str(d1)
    return delta


def str_of_block_instr(
    instr: Instruction, pc: int = 0, stack_effect: bool = False
) -> str:
    delta = str_of_stack_effect(instr) if stack_effect else ""
    eh = f" EH: {instr.exc_handler.bid}" if instr.exc_handler is not None else ""

    return f"{delta:>6} | {pc:3} {str_of_instr(instr)}{eh}"


def dump_block(
    graph: PyFlowGraph, block: Block, pc: int = 0, stack_effect: bool = False
) -> int:
    print(str_of_block_header(block))
    for instr in block.getInstructions():
        print("    ", str_of_block_instr(instr, pc, stack_effect))
        pc += graph.instrsize(instr, instr.ioparg) * opcode.CODEUNIT_SIZE
    return pc


def dump_graph(graph: PyFlowGraph, stack_effect: bool = False) -> None:
    pc = 0
    for block in graph.getBlocks():
        pc = dump_block(graph, block, pc, stack_effect)

# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from .opcodebase import Opcode
from .opcodes import opcode as base_opcode


opcode: Opcode = base_opcode.copy()
opcode.def_op("LOAD_METHOD_SUPER", 198)
opcode.hasconst.add("LOAD_SUPER_METHOD")
opcode.def_op("LOAD_ATTR_SUPER", 199)
opcode.hasconst.add("LOAD_ATTR_SUPER")


opcode.stack_effects.update(
    LOAD_METHOD_SUPER=-1,
    LOAD_ATTR_SUPER=-2,
)

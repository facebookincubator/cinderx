# Portions copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import sys

from opcode import (
    hascompare,
    hasconst,
    hasfree,
    hasjabs,
    hasjrel,
    haslocal,
    hasname,
    opmap,
)

from .opcodebase import Opcode

opcode: Opcode = Opcode()
for opname, opnum in opmap.items():
    if opnum in hasname:
        opcode.name_op(opname, opnum)
    elif opnum in hasjrel:
        opcode.jrel_op(opname, opnum)
    elif opnum in hasjabs:
        opcode.jabs_op(opname, opnum)
    else:
        opcode.def_op(opname, opnum)
        if opnum in hasconst:
            opcode.hasconst.add(opnum)
        elif opnum in hasfree:
            opcode.hasfree.add(opnum)
        elif opnum in haslocal:
            opcode.haslocal.add(opnum)
        elif opnum in hascompare:
            opcode.hascompare.add(opnum)


FVC_MASK = 0x3
FVC_NONE = 0x0
FVC_STR = 0x1
FVC_REPR = 0x2
FVC_ASCII = 0x3
FVS_MASK = 0x4
FVS_HAVE_SPEC = 0x4


opcode.stack_effects.update(
    NOP=0,
    POP_TOP=-1,
    ROT_TWO=0,
    ROT_THREE=0,
    DUP_TOP=1,
    DUP_TOP_TWO=2,
    ROT_FOUR=0,
    UNARY_POSITIVE=0,
    UNARY_NEGATIVE=0,
    UNARY_NOT=0,
    UNARY_INVERT=0,
    BINARY_MATRIX_MULTIPLY=-1,
    INPLACE_MATRIX_MULTIPLY=-1,
    BINARY_POWER=-1,
    BINARY_MULTIPLY=-1,
    BINARY_MODULO=-1,
    BINARY_ADD=-1,
    BINARY_SUBTRACT=-1,
    BINARY_SUBSCR=-1,
    BINARY_FLOOR_DIVIDE=-1,
    BINARY_TRUE_DIVIDE=-1,
    INPLACE_FLOOR_DIVIDE=-1,
    INPLACE_TRUE_DIVIDE=-1,
    GET_LEN=1,
    MATCH_MAPPING=1,
    MATCH_SEQUENCE=1,
    MATCH_KEYS=2,
    COPY_DICT_WITHOUT_KEYS=0,
    WITH_EXCEPT_START=1,
    GET_AITER=0,
    GET_ANEXT=1,
    BEFORE_ASYNC_WITH=1,
    END_ASYNC_FOR=-7,
    INPLACE_ADD=-1,
    INPLACE_SUBTRACT=-1,
    INPLACE_MULTIPLY=-1,
    INPLACE_MODULO=-1,
    STORE_SUBSCR=-3,
    DELETE_SUBSCR=-2,
    BINARY_LSHIFT=-1,
    BINARY_RSHIFT=-1,
    BINARY_AND=-1,
    BINARY_XOR=-1,
    BINARY_OR=-1,
    INPLACE_POWER=-1,
    GET_ITER=0,
    GET_YIELD_FROM_ITER=0,
    PRINT_EXPR=-1,
    LOAD_BUILD_CLASS=1,
    YIELD_FROM=-1,
    GET_AWAITABLE=0,
    LOAD_ASSERTION_ERROR=1,
    INPLACE_LSHIFT=-1,
    INPLACE_RSHIFT=-1,
    INPLACE_AND=-1,
    INPLACE_XOR=-1,
    INPLACE_OR=-1,
    LIST_TO_TUPLE=0,
    RETURN_VALUE=-1,
    IMPORT_STAR=-1,
    SETUP_ANNOTATIONS=0,
    YIELD_VALUE=0,
    POP_BLOCK=0,
    POP_EXCEPT=-3,
    STORE_NAME=-1,
    DELETE_NAME=0,
    UNPACK_SEQUENCE=lambda oparg, jmp=0: oparg - 1,
    FOR_ITER=lambda oparg, jmp=0: -1 if jmp > 0 else 1,
    UNPACK_EX=lambda oparg, jmp=0: (oparg & 0xFF) + (oparg >> 8),
    STORE_ATTR=-2,
    DELETE_ATTR=-1,
    STORE_GLOBAL=-1,
    DELETE_GLOBAL=0,
    ROT_N=0,
    LOAD_CONST=1,
    LOAD_NAME=1,
    BUILD_TUPLE=lambda oparg, jmp=0: 1 - oparg,
    BUILD_LIST=lambda oparg, jmp=0: 1 - oparg,
    BUILD_SET=lambda oparg, jmp=0: 1 - oparg,
    BUILD_MAP=lambda oparg, jmp=0: 1 - 2 * oparg,
    LOAD_ATTR=0,
    COMPARE_OP=-1,
    IMPORT_NAME=-1,
    IMPORT_FROM=1,
    JUMP_FORWARD=0,
    JUMP_IF_FALSE_OR_POP=lambda oparg, jmp=0: 0 if jmp else -1,
    JUMP_IF_TRUE_OR_POP=lambda oparg, jmp=0: 0 if jmp else -1,
    JUMP_ABSOLUTE=0,
    POP_JUMP_IF_FALSE=-1,
    POP_JUMP_IF_TRUE=-1,
    LOAD_GLOBAL=1,
    IS_OP=-1,
    CONTAINS_OP=-1,
    RERAISE=-3,
    JUMP_IF_NOT_EXC_MATCH=-2,
    SETUP_FINALLY=lambda oparg, jmp: 6 if jmp else 0,
    LOAD_FAST=1,
    STORE_FAST=-1,
    DELETE_FAST=0,
    GEN_START=-1,
    RAISE_VARARGS=lambda oparg, jmp=0: -oparg,
    CALL_FUNCTION=lambda oparg, jmp=0: -oparg,
    MAKE_FUNCTION=lambda oparg, jmp=0: -1
    - ((oparg & 0x01) != 0)
    - ((oparg & 0x02) != 0)
    - ((oparg & 0x04) != 0)
    - ((oparg & 0x08) != 0),
    BUILD_SLICE=lambda oparg, jmp=0: -2 if oparg == 3 else -1,
    LOAD_CLOSURE=1,
    LOAD_DEREF=1,
    STORE_DEREF=-1,
    DELETE_DEREF=0,
    CALL_FUNCTION_KW=lambda oparg, jmp=0: -oparg - 1,
    CALL_FUNCTION_EX=lambda oparg, jmp=0: -1 - ((oparg & 0x01) != 0),
    SETUP_WITH=lambda oparg, jmp=0: 6 if jmp else 1,
    EXTENDED_ARG=0,
    LIST_APPEND=-1,
    SET_ADD=-1,
    MAP_ADD=-2,
    LOAD_CLASSDEREF=1,
    MATCH_CLASS=-1,
    SETUP_ASYNC_WITH=lambda oparg, jmp: (-1 + 6) if jmp else 0,
    # If there's a fmt_spec on the stack, we go from 2->1,
    # else 1->1.
    FORMAT_VALUE=lambda oparg, jmp=0: -1 if (oparg & FVS_MASK) == FVS_HAVE_SPEC else 0,
    BUILD_CONST_KEY_MAP=lambda oparg, jmp=0: -oparg,
    BUILD_STRING=lambda oparg, jmp=0: 1 - oparg,
    INVOKE_METHOD=lambda oparg, jmp: -oparg - 1,
    LOAD_METHOD=1,
    CALL_METHOD=lambda oparg, jmp: -oparg - 1,
    LIST_EXTEND=-1,
    SET_UPDATE=-1,
    DICT_MERGE=-1,
    DICT_UPDATE=-1,
)

if sys.version_info >= (3, 12):
    opcode.stack_effects.update(
        CALL=lambda oparg, jmp=0: -oparg,
        JUMP=0,
        JUMP_BACKWARD=0,
        PUSH_NULL=1,
        RETURN_CONST=0,
        SWAP=0,
        COPY=1,
        CALL_INTRINSIC_1=0,
        CALL_INTRINSIC_2=-1,
        LOAD_FROM_DICT_OR_DEREF=0,
        LOAD_LOCALS=1,
        MAKE_FUNCTION=lambda oparg, jmp=0: oparg.bit_count(),
        LOAD_FROM_DICT_OR_GLOBALS=-1,
        LOAD_SUPER_ATTR=-1,
    )

    from opcode import (
        _intrinsic_1_descs as INTRINSIC_1,
        _intrinsic_2_descs as INTRINSIC_2,
        _nb_ops as NB_OPS,
    )

else:
    NB_OPS: list[tuple[str, str]] = []
    INTRINSIC_1: list[str] = []
    INTRINSIC_2: list[str] = []

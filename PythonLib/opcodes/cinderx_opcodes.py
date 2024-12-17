# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

# Opcodes defined by cinderx

# Cinderx defines its own set of opcodes, which need to be added to the cpython
# ones. Since cpython does not have a stable map of opcode numbers to opcodes,
# we have to assign numbers to these opcodes on a per-version basis. This file
# serves as the source of truth for the cinderx opcodes and their attributes,
# and should be combined with the opcode map for each python version to
# allocate opcode numbers for that version.


# Bitflags for opcode attributes.
NONE = 0
NAME = 1
JREL = 2
JABS = 4
CONST = 8
IMPLEMENTED_IN_INTERPRETER = 16


CINDER_OPS = {
    "INVOKE_METHOD": CONST | IMPLEMENTED_IN_INTERPRETER,
    "LOAD_FIELD": CONST | IMPLEMENTED_IN_INTERPRETER,
    "STORE_FIELD": CONST | IMPLEMENTED_IN_INTERPRETER,
    "BUILD_CHECKED_LIST": CONST,
    "LOAD_TYPE": CONST,
    "CAST": CONST | IMPLEMENTED_IN_INTERPRETER,
    "LOAD_LOCAL": CONST | IMPLEMENTED_IN_INTERPRETER,
    "STORE_LOCAL": CONST | IMPLEMENTED_IN_INTERPRETER,
    "PRIMITIVE_BOX": NONE,
    "POP_JUMP_IF_ZERO": JABS,
    "POP_JUMP_IF_NONZERO": JABS,
    "PRIMITIVE_UNBOX": NONE | IMPLEMENTED_IN_INTERPRETER,
    "PRIMITIVE_BINARY_OP": NONE | IMPLEMENTED_IN_INTERPRETER,
    "PRIMITIVE_UNARY_OP": NONE | IMPLEMENTED_IN_INTERPRETER,
    "PRIMITIVE_COMPARE_OP": NONE | IMPLEMENTED_IN_INTERPRETER,
    "LOAD_ITERABLE_ARG": NONE | IMPLEMENTED_IN_INTERPRETER,
    "LOAD_MAPPING_ARG": NONE,
    "INVOKE_FUNCTION": CONST | IMPLEMENTED_IN_INTERPRETER,
    "JUMP_IF_ZERO_OR_POP": JABS,
    "JUMP_IF_NONZERO_OR_POP": JABS,
    "FAST_LEN": NONE,
    "CONVERT_PRIMITIVE": NONE,
    "INVOKE_NATIVE": CONST | IMPLEMENTED_IN_INTERPRETER,
    "LOAD_CLASS": CONST,
    "BUILD_CHECKED_MAP": CONST | IMPLEMENTED_IN_INTERPRETER,
    "SEQUENCE_GET": NONE,
    "SEQUENCE_SET": NONE,
    "LIST_DEL": NONE,
    "REFINE_TYPE": CONST,
    "PRIMITIVE_LOAD_CONST": CONST | IMPLEMENTED_IN_INTERPRETER,
    "RETURN_PRIMITIVE": NONE | IMPLEMENTED_IN_INTERPRETER,
    "TP_ALLOC": CONST,
}

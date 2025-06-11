# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

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
ARG = 16
IMPLEMENTED_IN_INTERPRETER = 32


class Family:
    def __init__(
        self, flags: int, cache_format: dict[str, int], *specializations: str
    ) -> None:
        self.flags = flags
        self.cache_format = cache_format
        self.specializations: tuple[str, ...] = specializations


CINDER_OPS: dict[str, int | Family] = {
    "INVOKE_METHOD": CONST | IMPLEMENTED_IN_INTERPRETER,
    "LOAD_FIELD": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER,
        {
            "cache": 2,
        },
        "LOAD_OBJ_FIELD",
        "LOAD_PRIMITIVE_FIELD",
    ),
    "STORE_FIELD": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER,
        {
            "cache": 2,
        },
        "STORE_OBJ_FIELD",
        "STORE_PRIMITIVE_FIELD",
    ),
    "BUILD_CHECKED_LIST": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER,
        {
            "cache": 2,
        },
        "BUILD_CHECKED_LIST_CACHED",
    ),
    "LOAD_TYPE": CONST | IMPLEMENTED_IN_INTERPRETER,
    "CAST": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER,
        {
            "cache": 2,
        },
        "CAST_CACHED",
    ),
    "LOAD_LOCAL": CONST | IMPLEMENTED_IN_INTERPRETER,
    "STORE_LOCAL": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER,
        {
            "cache": 1,
        },
        "STORE_LOCAL_CACHED",
    ),
    "PRIMITIVE_BOX": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "POP_JUMP_IF_ZERO": JREL | ARG | IMPLEMENTED_IN_INTERPRETER,
    "POP_JUMP_IF_NONZERO": JREL | ARG | IMPLEMENTED_IN_INTERPRETER,
    "PRIMITIVE_UNBOX": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "PRIMITIVE_BINARY_OP": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "PRIMITIVE_UNARY_OP": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "PRIMITIVE_COMPARE_OP": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "LOAD_ITERABLE_ARG": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "LOAD_MAPPING_ARG": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "INVOKE_FUNCTION": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER,
        {
            "cache": 4,
        },
        "INVOKE_FUNCTION_CACHED",
        "INVOKE_INDIRECT_CACHED",
    ),
    "JUMP_IF_ZERO_OR_POP": JABS,
    "JUMP_IF_NONZERO_OR_POP": JABS,
    "FAST_LEN": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "CONVERT_PRIMITIVE": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "INVOKE_NATIVE": CONST | IMPLEMENTED_IN_INTERPRETER,
    "LOAD_CLASS": CONST | IMPLEMENTED_IN_INTERPRETER,
    "BUILD_CHECKED_MAP": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER,
        {
            "cache": 2,
        },
        "BUILD_CHECKED_MAP_CACHED",
    ),
    "SEQUENCE_GET": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "SEQUENCE_SET": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "LIST_DEL": NONE | IMPLEMENTED_IN_INTERPRETER,
    "REFINE_TYPE": CONST | IMPLEMENTED_IN_INTERPRETER,
    "PRIMITIVE_LOAD_CONST": CONST | IMPLEMENTED_IN_INTERPRETER,
    "RETURN_PRIMITIVE": NONE | IMPLEMENTED_IN_INTERPRETER | ARG,
    "TP_ALLOC": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER,
        {
            "cache": 2,
        },
        "TP_ALLOC_CACHED",
    ),
    "LOAD_METHOD_STATIC": Family(
        CONST | IMPLEMENTED_IN_INTERPRETER, {"cache": 2}, "LOAD_METHOD_STATIC_CACHED"
    ),
}

# Copyright (c) Meta Platforms, Inc. and affiliates.

# Generated via assign_opcode_numbers.py, do not edit.

# This is an addition to python/3.12/Lib/opcode.py, and is intended to be run
# via `exec` in generate_opcode_h.py with the globals dict obtained from
# running Lib/opcode.py.

# flake8: noqa


# Lib/opcode.py deletes these functions so we need to define them again here.
# We also need to update opname when we call def_op().
def init(opname, opmap, hasname, hasjrel, hasjabs, hasconst, interp_only):
    def def_op(name, op):
        opmap[name] = op
        opname[op] = name

    def name_op(name, op):
        def_op(name, op)
        hasname.append(op)

    def jrel_op(name, op):
        def_op(name, op)
        hasjrel.append(op)

    def jabs_op(name, op):
        def_op(name, op)
        hasjabs.append(op)

    def_op("INVOKE_METHOD", 185)
    hasconst.append(185)
    def_op("LOAD_FIELD", 186)
    hasconst.append(186)
    def_op("STORE_FIELD", 187)
    hasconst.append(187)
    if not interp_only:
        def_op("BUILD_CHECKED_LIST", 188)
        hasconst.append(188)
    if not interp_only:
        def_op("LOAD_TYPE", 189)
        hasconst.append(189)
    def_op("CAST", 190)
    hasconst.append(190)
    def_op("LOAD_LOCAL", 191)
    hasconst.append(191)
    def_op("STORE_LOCAL", 192)
    hasconst.append(192)
    if not interp_only:
        def_op("PRIMITIVE_BOX", 193)
    if not interp_only:
        jabs_op("POP_JUMP_IF_ZERO", 194)
    if not interp_only:
        jabs_op("POP_JUMP_IF_NONZERO", 195)
    def_op("PRIMITIVE_UNBOX", 196)
    def_op("PRIMITIVE_BINARY_OP", 197)
    def_op("PRIMITIVE_UNARY_OP", 198)
    def_op("PRIMITIVE_COMPARE_OP", 199)
    def_op("LOAD_ITERABLE_ARG", 200)
    if not interp_only:
        def_op("LOAD_MAPPING_ARG", 201)
    def_op("INVOKE_FUNCTION", 202)
    hasconst.append(202)
    if not interp_only:
        jabs_op("JUMP_IF_ZERO_OR_POP", 203)
    if not interp_only:
        jabs_op("JUMP_IF_NONZERO_OR_POP", 204)
    def_op("FAST_LEN", 205)
    def_op("CONVERT_PRIMITIVE", 206)
    def_op("INVOKE_NATIVE", 207)
    hasconst.append(207)
    if not interp_only:
        def_op("LOAD_CLASS", 208)
        hasconst.append(208)
    def_op("BUILD_CHECKED_MAP", 209)
    hasconst.append(209)
    def_op("SEQUENCE_GET", 210)
    def_op("SEQUENCE_SET", 211)
    def_op("LIST_DEL", 212)
    def_op("REFINE_TYPE", 213)
    hasconst.append(213)
    def_op("PRIMITIVE_LOAD_CONST", 214)
    hasconst.append(214)
    def_op("RETURN_PRIMITIVE", 215)
    if not interp_only:
        def_op("TP_ALLOC", 216)
        hasconst.append(216)

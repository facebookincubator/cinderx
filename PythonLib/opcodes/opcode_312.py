# Copyright (c) Meta Platforms, Inc. and affiliates.

# Generated via assign_opcode_numbers.py, do not edit.

# This is an addition to python/3.12/Lib/opcode.py, and is intended to be run
# via `exec` in generate_opcode_h.py with the globals dict obtained from
# running Lib/opcode.py.

# flake8: noqa


# Lib/opcode.py deletes these functions so we need to define them again here.
# We also need to update opname when we call def_op().
def init(opname, opmap, hasname, hasjrel, hasjabs, hasconst, hasarg, interp_only=False):
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
    hasarg.append(185)
    def_op("LOAD_FIELD", 186)
    hasconst.append(186)
    hasarg.append(186)
    def_op("STORE_FIELD", 187)
    hasconst.append(187)
    hasarg.append(187)
    def_op("BUILD_CHECKED_LIST", 188)
    hasconst.append(188)
    hasarg.append(188)
    def_op("LOAD_TYPE", 189)
    hasconst.append(189)
    hasarg.append(189)
    def_op("CAST", 190)
    hasconst.append(190)
    hasarg.append(190)
    def_op("LOAD_LOCAL", 191)
    hasconst.append(191)
    hasarg.append(191)
    def_op("STORE_LOCAL", 192)
    hasconst.append(192)
    hasarg.append(192)
    def_op("PRIMITIVE_BOX", 193)
    hasarg.append(193)
    jrel_op("POP_JUMP_IF_ZERO", 194)
    hasarg.append(194)
    jrel_op("POP_JUMP_IF_NONZERO", 195)
    hasarg.append(195)
    def_op("PRIMITIVE_UNBOX", 196)
    hasarg.append(196)
    def_op("PRIMITIVE_BINARY_OP", 197)
    hasarg.append(197)
    def_op("PRIMITIVE_UNARY_OP", 198)
    hasarg.append(198)
    def_op("PRIMITIVE_COMPARE_OP", 199)
    hasarg.append(199)
    def_op("LOAD_ITERABLE_ARG", 200)
    hasarg.append(200)
    def_op("LOAD_MAPPING_ARG", 201)
    hasarg.append(201)
    def_op("INVOKE_FUNCTION", 202)
    hasconst.append(202)
    hasarg.append(202)
    if not interp_only:
        jabs_op("JUMP_IF_ZERO_OR_POP", 203)
    if not interp_only:
        jabs_op("JUMP_IF_NONZERO_OR_POP", 204)
    def_op("FAST_LEN", 205)
    hasarg.append(205)
    def_op("CONVERT_PRIMITIVE", 206)
    hasarg.append(206)
    def_op("INVOKE_NATIVE", 207)
    hasconst.append(207)
    hasarg.append(207)
    def_op("LOAD_CLASS", 208)
    hasconst.append(208)
    hasarg.append(208)
    def_op("BUILD_CHECKED_MAP", 209)
    hasconst.append(209)
    hasarg.append(209)
    def_op("SEQUENCE_GET", 210)
    hasarg.append(210)
    def_op("SEQUENCE_SET", 211)
    hasarg.append(211)
    def_op("LIST_DEL", 212)
    def_op("REFINE_TYPE", 213)
    hasconst.append(213)
    hasarg.append(213)
    def_op("PRIMITIVE_LOAD_CONST", 214)
    hasconst.append(214)
    hasarg.append(214)
    def_op("RETURN_PRIMITIVE", 215)
    hasarg.append(215)
    def_op("TP_ALLOC", 216)
    hasconst.append(216)
    hasarg.append(216)

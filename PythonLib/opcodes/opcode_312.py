# Copyright (c) Meta Platforms, Inc. and affiliates.

# Generated via assign_opcode_numbers.py, do not edit.

# This is an addition to python/3.12/Lib/opcode.py, and is intended to be run
# via `exec` in generate_opcode_h.py with the globals dict obtained from
# running Lib/opcode.py.

# flake8: noqa


# Lib/opcode.py deletes these functions so we need to define them again here.
# We also need to update opname when we call def_op().
def init(
    opname,
    opmap,
    hasname,
    hasjrel,
    hasjabs,
    hasconst,
    hasarg,
    cache_format,
    specializations,
    inline_cache_entries,
    interp_only=False,
):
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
    cache_format["INVOKE_FUNCTION"] = "{'cache': 4}"
    inline_cache_entries[202] = 4
    def_op("INVOKE_FUNCTION_CACHED", 203)
    hasconst.append(203)
    hasarg.append(203)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_FUNCTION_CACHED")
    inline_cache_entries[203] = 4
    def_op("INVOKE_INDIRECT_CACHED", 204)
    hasconst.append(204)
    hasarg.append(204)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_INDIRECT_CACHED")
    inline_cache_entries[204] = 4
    if not interp_only:
        jabs_op("JUMP_IF_ZERO_OR_POP", 205)
    if not interp_only:
        jabs_op("JUMP_IF_NONZERO_OR_POP", 206)
    def_op("FAST_LEN", 207)
    hasarg.append(207)
    def_op("CONVERT_PRIMITIVE", 208)
    hasarg.append(208)
    def_op("INVOKE_NATIVE", 209)
    hasconst.append(209)
    hasarg.append(209)
    def_op("LOAD_CLASS", 210)
    hasconst.append(210)
    hasarg.append(210)
    def_op("BUILD_CHECKED_MAP", 211)
    hasconst.append(211)
    hasarg.append(211)
    def_op("SEQUENCE_GET", 212)
    hasarg.append(212)
    def_op("SEQUENCE_SET", 213)
    hasarg.append(213)
    def_op("LIST_DEL", 214)
    def_op("REFINE_TYPE", 215)
    hasconst.append(215)
    hasarg.append(215)
    def_op("PRIMITIVE_LOAD_CONST", 216)
    hasconst.append(216)
    hasarg.append(216)
    def_op("RETURN_PRIMITIVE", 217)
    hasarg.append(217)
    def_op("TP_ALLOC", 218)
    hasconst.append(218)
    hasarg.append(218)
    cache_format["TP_ALLOC"] = "{'cache': 2}"
    inline_cache_entries[218] = 2
    def_op("TP_ALLOC_CACHED", 219)
    hasconst.append(219)
    hasarg.append(219)
    if "TP_ALLOC" not in specializations:
        specializations["TP_ALLOC"] = []
    specializations["TP_ALLOC"].append("TP_ALLOC_CACHED")
    inline_cache_entries[219] = 2
    def_op("LOAD_METHOD_STATIC", 220)
    hasconst.append(220)
    hasarg.append(220)
    cache_format["LOAD_METHOD_STATIC"] = "{'cache': 2}"
    inline_cache_entries[220] = 2
    def_op("LOAD_METHOD_STATIC_CACHED", 221)
    hasconst.append(221)
    hasarg.append(221)
    if "LOAD_METHOD_STATIC" not in specializations:
        specializations["LOAD_METHOD_STATIC"] = []
    specializations["LOAD_METHOD_STATIC"].append("LOAD_METHOD_STATIC_CACHED")
    inline_cache_entries[221] = 2

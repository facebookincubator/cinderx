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
    cache_format["LOAD_FIELD"] = "{'cache': 2}"
    inline_cache_entries[186] = 2
    def_op("LOAD_OBJ_FIELD", 187)
    hasconst.append(187)
    hasarg.append(187)
    if "LOAD_FIELD" not in specializations:
        specializations["LOAD_FIELD"] = []
    specializations["LOAD_FIELD"].append("LOAD_OBJ_FIELD")
    inline_cache_entries[187] = 2
    def_op("LOAD_PRIMITIVE_FIELD", 188)
    hasconst.append(188)
    hasarg.append(188)
    if "LOAD_FIELD" not in specializations:
        specializations["LOAD_FIELD"] = []
    specializations["LOAD_FIELD"].append("LOAD_PRIMITIVE_FIELD")
    inline_cache_entries[188] = 2
    def_op("STORE_FIELD", 189)
    hasconst.append(189)
    hasarg.append(189)
    def_op("BUILD_CHECKED_LIST", 190)
    hasconst.append(190)
    hasarg.append(190)
    def_op("LOAD_TYPE", 191)
    hasconst.append(191)
    hasarg.append(191)
    def_op("CAST", 192)
    hasconst.append(192)
    hasarg.append(192)
    def_op("LOAD_LOCAL", 193)
    hasconst.append(193)
    hasarg.append(193)
    def_op("STORE_LOCAL", 194)
    hasconst.append(194)
    hasarg.append(194)
    cache_format["STORE_LOCAL"] = "{'cache': 1}"
    inline_cache_entries[194] = 1
    def_op("STORE_LOCAL_CACHED", 195)
    hasconst.append(195)
    hasarg.append(195)
    if "STORE_LOCAL" not in specializations:
        specializations["STORE_LOCAL"] = []
    specializations["STORE_LOCAL"].append("STORE_LOCAL_CACHED")
    inline_cache_entries[195] = 1
    def_op("PRIMITIVE_BOX", 196)
    hasarg.append(196)
    jrel_op("POP_JUMP_IF_ZERO", 197)
    hasarg.append(197)
    jrel_op("POP_JUMP_IF_NONZERO", 198)
    hasarg.append(198)
    def_op("PRIMITIVE_UNBOX", 199)
    hasarg.append(199)
    def_op("PRIMITIVE_BINARY_OP", 200)
    hasarg.append(200)
    def_op("PRIMITIVE_UNARY_OP", 201)
    hasarg.append(201)
    def_op("PRIMITIVE_COMPARE_OP", 202)
    hasarg.append(202)
    def_op("LOAD_ITERABLE_ARG", 203)
    hasarg.append(203)
    def_op("LOAD_MAPPING_ARG", 204)
    hasarg.append(204)
    def_op("INVOKE_FUNCTION", 205)
    hasconst.append(205)
    hasarg.append(205)
    cache_format["INVOKE_FUNCTION"] = "{'cache': 4}"
    inline_cache_entries[205] = 4
    def_op("INVOKE_FUNCTION_CACHED", 206)
    hasconst.append(206)
    hasarg.append(206)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_FUNCTION_CACHED")
    inline_cache_entries[206] = 4
    def_op("INVOKE_INDIRECT_CACHED", 207)
    hasconst.append(207)
    hasarg.append(207)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_INDIRECT_CACHED")
    inline_cache_entries[207] = 4
    if not interp_only:
        jabs_op("JUMP_IF_ZERO_OR_POP", 208)
    if not interp_only:
        jabs_op("JUMP_IF_NONZERO_OR_POP", 209)
    def_op("FAST_LEN", 210)
    hasarg.append(210)
    def_op("CONVERT_PRIMITIVE", 211)
    hasarg.append(211)
    def_op("INVOKE_NATIVE", 212)
    hasconst.append(212)
    hasarg.append(212)
    def_op("LOAD_CLASS", 213)
    hasconst.append(213)
    hasarg.append(213)
    def_op("BUILD_CHECKED_MAP", 214)
    hasconst.append(214)
    hasarg.append(214)
    def_op("SEQUENCE_GET", 215)
    hasarg.append(215)
    def_op("SEQUENCE_SET", 216)
    hasarg.append(216)
    def_op("LIST_DEL", 217)
    def_op("REFINE_TYPE", 218)
    hasconst.append(218)
    hasarg.append(218)
    def_op("PRIMITIVE_LOAD_CONST", 219)
    hasconst.append(219)
    hasarg.append(219)
    def_op("RETURN_PRIMITIVE", 220)
    hasarg.append(220)
    def_op("TP_ALLOC", 221)
    hasconst.append(221)
    hasarg.append(221)
    cache_format["TP_ALLOC"] = "{'cache': 2}"
    inline_cache_entries[221] = 2
    def_op("TP_ALLOC_CACHED", 222)
    hasconst.append(222)
    hasarg.append(222)
    if "TP_ALLOC" not in specializations:
        specializations["TP_ALLOC"] = []
    specializations["TP_ALLOC"].append("TP_ALLOC_CACHED")
    inline_cache_entries[222] = 2
    def_op("LOAD_METHOD_STATIC", 223)
    hasconst.append(223)
    hasarg.append(223)
    cache_format["LOAD_METHOD_STATIC"] = "{'cache': 2}"
    inline_cache_entries[223] = 2
    def_op("LOAD_METHOD_STATIC_CACHED", 224)
    hasconst.append(224)
    hasarg.append(224)
    if "LOAD_METHOD_STATIC" not in specializations:
        specializations["LOAD_METHOD_STATIC"] = []
    specializations["LOAD_METHOD_STATIC"].append("LOAD_METHOD_STATIC_CACHED")
    inline_cache_entries[224] = 2

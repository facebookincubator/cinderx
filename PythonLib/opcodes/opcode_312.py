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
    cache_format["STORE_FIELD"] = "{'cache': 2}"
    inline_cache_entries[189] = 2
    def_op("STORE_OBJ_FIELD", 190)
    hasconst.append(190)
    hasarg.append(190)
    if "STORE_FIELD" not in specializations:
        specializations["STORE_FIELD"] = []
    specializations["STORE_FIELD"].append("STORE_OBJ_FIELD")
    inline_cache_entries[190] = 2
    def_op("STORE_PRIMITIVE_FIELD", 191)
    hasconst.append(191)
    hasarg.append(191)
    if "STORE_FIELD" not in specializations:
        specializations["STORE_FIELD"] = []
    specializations["STORE_FIELD"].append("STORE_PRIMITIVE_FIELD")
    inline_cache_entries[191] = 2
    def_op("BUILD_CHECKED_LIST", 192)
    hasconst.append(192)
    hasarg.append(192)
    def_op("LOAD_TYPE", 193)
    hasconst.append(193)
    hasarg.append(193)
    def_op("CAST", 194)
    hasconst.append(194)
    hasarg.append(194)
    cache_format["CAST"] = "{'cache': 2}"
    inline_cache_entries[194] = 2
    def_op("CAST_CACHED", 195)
    hasconst.append(195)
    hasarg.append(195)
    if "CAST" not in specializations:
        specializations["CAST"] = []
    specializations["CAST"].append("CAST_CACHED")
    inline_cache_entries[195] = 2
    def_op("LOAD_LOCAL", 196)
    hasconst.append(196)
    hasarg.append(196)
    def_op("STORE_LOCAL", 197)
    hasconst.append(197)
    hasarg.append(197)
    cache_format["STORE_LOCAL"] = "{'cache': 1}"
    inline_cache_entries[197] = 1
    def_op("STORE_LOCAL_CACHED", 198)
    hasconst.append(198)
    hasarg.append(198)
    if "STORE_LOCAL" not in specializations:
        specializations["STORE_LOCAL"] = []
    specializations["STORE_LOCAL"].append("STORE_LOCAL_CACHED")
    inline_cache_entries[198] = 1
    def_op("PRIMITIVE_BOX", 199)
    hasarg.append(199)
    jrel_op("POP_JUMP_IF_ZERO", 200)
    hasarg.append(200)
    jrel_op("POP_JUMP_IF_NONZERO", 201)
    hasarg.append(201)
    def_op("PRIMITIVE_UNBOX", 202)
    hasarg.append(202)
    def_op("PRIMITIVE_BINARY_OP", 203)
    hasarg.append(203)
    def_op("PRIMITIVE_UNARY_OP", 204)
    hasarg.append(204)
    def_op("PRIMITIVE_COMPARE_OP", 205)
    hasarg.append(205)
    def_op("LOAD_ITERABLE_ARG", 206)
    hasarg.append(206)
    def_op("LOAD_MAPPING_ARG", 207)
    hasarg.append(207)
    def_op("INVOKE_FUNCTION", 208)
    hasconst.append(208)
    hasarg.append(208)
    cache_format["INVOKE_FUNCTION"] = "{'cache': 4}"
    inline_cache_entries[208] = 4
    def_op("INVOKE_FUNCTION_CACHED", 209)
    hasconst.append(209)
    hasarg.append(209)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_FUNCTION_CACHED")
    inline_cache_entries[209] = 4
    def_op("INVOKE_INDIRECT_CACHED", 210)
    hasconst.append(210)
    hasarg.append(210)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_INDIRECT_CACHED")
    inline_cache_entries[210] = 4
    if not interp_only:
        jabs_op("JUMP_IF_ZERO_OR_POP", 211)
    if not interp_only:
        jabs_op("JUMP_IF_NONZERO_OR_POP", 212)
    def_op("FAST_LEN", 213)
    hasarg.append(213)
    def_op("CONVERT_PRIMITIVE", 214)
    hasarg.append(214)
    def_op("INVOKE_NATIVE", 215)
    hasconst.append(215)
    hasarg.append(215)
    def_op("LOAD_CLASS", 216)
    hasconst.append(216)
    hasarg.append(216)
    def_op("BUILD_CHECKED_MAP", 217)
    hasconst.append(217)
    hasarg.append(217)
    def_op("SEQUENCE_GET", 218)
    hasarg.append(218)
    def_op("SEQUENCE_SET", 219)
    hasarg.append(219)
    def_op("LIST_DEL", 220)
    def_op("REFINE_TYPE", 221)
    hasconst.append(221)
    hasarg.append(221)
    def_op("PRIMITIVE_LOAD_CONST", 222)
    hasconst.append(222)
    hasarg.append(222)
    def_op("RETURN_PRIMITIVE", 223)
    hasarg.append(223)
    def_op("TP_ALLOC", 224)
    hasconst.append(224)
    hasarg.append(224)
    cache_format["TP_ALLOC"] = "{'cache': 2}"
    inline_cache_entries[224] = 2
    def_op("TP_ALLOC_CACHED", 225)
    hasconst.append(225)
    hasarg.append(225)
    if "TP_ALLOC" not in specializations:
        specializations["TP_ALLOC"] = []
    specializations["TP_ALLOC"].append("TP_ALLOC_CACHED")
    inline_cache_entries[225] = 2
    def_op("LOAD_METHOD_STATIC", 226)
    hasconst.append(226)
    hasarg.append(226)
    cache_format["LOAD_METHOD_STATIC"] = "{'cache': 2}"
    inline_cache_entries[226] = 2
    def_op("LOAD_METHOD_STATIC_CACHED", 227)
    hasconst.append(227)
    hasarg.append(227)
    if "LOAD_METHOD_STATIC" not in specializations:
        specializations["LOAD_METHOD_STATIC"] = []
    specializations["LOAD_METHOD_STATIC"].append("LOAD_METHOD_STATIC_CACHED")
    inline_cache_entries[227] = 2

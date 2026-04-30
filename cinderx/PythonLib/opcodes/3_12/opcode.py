# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

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
    cache_format["BUILD_CHECKED_LIST"] = "{'cache': 2}"
    inline_cache_entries[192] = 2
    def_op("BUILD_CHECKED_LIST_CACHED", 193)
    hasconst.append(193)
    hasarg.append(193)
    if "BUILD_CHECKED_LIST" not in specializations:
        specializations["BUILD_CHECKED_LIST"] = []
    specializations["BUILD_CHECKED_LIST"].append("BUILD_CHECKED_LIST_CACHED")
    inline_cache_entries[193] = 2
    def_op("LOAD_TYPE", 194)
    hasconst.append(194)
    hasarg.append(194)
    def_op("CAST", 195)
    hasconst.append(195)
    hasarg.append(195)
    cache_format["CAST"] = "{'cache': 2}"
    inline_cache_entries[195] = 2
    def_op("CAST_CACHED", 196)
    hasconst.append(196)
    hasarg.append(196)
    if "CAST" not in specializations:
        specializations["CAST"] = []
    specializations["CAST"].append("CAST_CACHED")
    inline_cache_entries[196] = 2
    def_op("LOAD_LOCAL", 197)
    hasconst.append(197)
    hasarg.append(197)
    def_op("STORE_LOCAL", 198)
    hasconst.append(198)
    hasarg.append(198)
    cache_format["STORE_LOCAL"] = "{'cache': 1}"
    inline_cache_entries[198] = 1
    def_op("STORE_LOCAL_CACHED", 199)
    hasconst.append(199)
    hasarg.append(199)
    if "STORE_LOCAL" not in specializations:
        specializations["STORE_LOCAL"] = []
    specializations["STORE_LOCAL"].append("STORE_LOCAL_CACHED")
    inline_cache_entries[199] = 1
    def_op("PRIMITIVE_BOX", 200)
    hasarg.append(200)
    jrel_op("POP_JUMP_IF_ZERO", 201)
    hasarg.append(201)
    jrel_op("POP_JUMP_IF_NONZERO", 202)
    hasarg.append(202)
    def_op("PRIMITIVE_UNBOX", 203)
    hasarg.append(203)
    def_op("PRIMITIVE_BINARY_OP", 204)
    hasarg.append(204)
    def_op("PRIMITIVE_UNARY_OP", 205)
    hasarg.append(205)
    def_op("PRIMITIVE_COMPARE_OP", 206)
    hasarg.append(206)
    def_op("LOAD_ITERABLE_ARG", 207)
    hasarg.append(207)
    def_op("LOAD_MAPPING_ARG", 208)
    hasarg.append(208)
    def_op("INVOKE_FUNCTION", 209)
    hasconst.append(209)
    hasarg.append(209)
    cache_format["INVOKE_FUNCTION"] = "{'cache': 4}"
    inline_cache_entries[209] = 4
    def_op("INVOKE_FUNCTION_CACHED", 210)
    hasconst.append(210)
    hasarg.append(210)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_FUNCTION_CACHED")
    inline_cache_entries[210] = 4
    def_op("INVOKE_INDIRECT_CACHED", 211)
    hasconst.append(211)
    hasarg.append(211)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_INDIRECT_CACHED")
    inline_cache_entries[211] = 4
    if not interp_only:
        jabs_op("JUMP_IF_ZERO_OR_POP", 212)
    if not interp_only:
        jabs_op("JUMP_IF_NONZERO_OR_POP", 213)
    def_op("FAST_LEN", 214)
    hasarg.append(214)
    def_op("CONVERT_PRIMITIVE", 215)
    hasarg.append(215)
    def_op("INVOKE_NATIVE", 216)
    hasconst.append(216)
    hasarg.append(216)
    def_op("LOAD_CLASS", 217)
    hasconst.append(217)
    hasarg.append(217)
    def_op("BUILD_CHECKED_MAP", 218)
    hasconst.append(218)
    hasarg.append(218)
    cache_format["BUILD_CHECKED_MAP"] = "{'cache': 2}"
    inline_cache_entries[218] = 2
    def_op("BUILD_CHECKED_MAP_CACHED", 219)
    hasconst.append(219)
    hasarg.append(219)
    if "BUILD_CHECKED_MAP" not in specializations:
        specializations["BUILD_CHECKED_MAP"] = []
    specializations["BUILD_CHECKED_MAP"].append("BUILD_CHECKED_MAP_CACHED")
    inline_cache_entries[219] = 2
    def_op("SEQUENCE_GET", 220)
    hasarg.append(220)
    def_op("SEQUENCE_SET", 221)
    hasarg.append(221)
    def_op("LIST_DEL", 222)
    def_op("REFINE_TYPE", 223)
    hasconst.append(223)
    hasarg.append(223)
    def_op("PRIMITIVE_LOAD_CONST", 224)
    hasconst.append(224)
    hasarg.append(224)
    def_op("RETURN_PRIMITIVE", 225)
    hasarg.append(225)
    def_op("TP_ALLOC", 226)
    hasconst.append(226)
    hasarg.append(226)
    cache_format["TP_ALLOC"] = "{'cache': 2}"
    inline_cache_entries[226] = 2
    def_op("TP_ALLOC_CACHED", 227)
    hasconst.append(227)
    hasarg.append(227)
    if "TP_ALLOC" not in specializations:
        specializations["TP_ALLOC"] = []
    specializations["TP_ALLOC"].append("TP_ALLOC_CACHED")
    inline_cache_entries[227] = 2
    def_op("LOAD_METHOD_STATIC", 228)
    hasconst.append(228)
    hasarg.append(228)
    cache_format["LOAD_METHOD_STATIC"] = "{'cache': 2}"
    inline_cache_entries[228] = 2
    def_op("LOAD_METHOD_STATIC_CACHED", 230)
    hasconst.append(230)
    hasarg.append(230)
    if "LOAD_METHOD_STATIC" not in specializations:
        specializations["LOAD_METHOD_STATIC"] = []
    specializations["LOAD_METHOD_STATIC"].append("LOAD_METHOD_STATIC_CACHED")
    inline_cache_entries[230] = 2

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

    def_op("INVOKE_METHOD", 1)
    hasconst.append(1)
    hasarg.append(1)
    def_op("LOAD_FIELD", 2)
    hasconst.append(2)
    hasarg.append(2)
    cache_format["LOAD_FIELD"] = "{'cache': 2}"
    def_op("LOAD_OBJ_FIELD", 4)
    hasconst.append(4)
    hasarg.append(4)
    if "LOAD_FIELD" not in specializations:
        specializations["LOAD_FIELD"] = []
    specializations["LOAD_FIELD"].append("LOAD_OBJ_FIELD")
    def_op("LOAD_PRIMITIVE_FIELD", 5)
    hasconst.append(5)
    hasarg.append(5)
    if "LOAD_FIELD" not in specializations:
        specializations["LOAD_FIELD"] = []
    specializations["LOAD_FIELD"].append("LOAD_PRIMITIVE_FIELD")
    def_op("STORE_FIELD", 6)
    hasconst.append(6)
    hasarg.append(6)
    cache_format["STORE_FIELD"] = "{'cache': 2}"
    def_op("STORE_OBJ_FIELD", 7)
    hasconst.append(7)
    hasarg.append(7)
    if "STORE_FIELD" not in specializations:
        specializations["STORE_FIELD"] = []
    specializations["STORE_FIELD"].append("STORE_OBJ_FIELD")
    def_op("STORE_PRIMITIVE_FIELD", 8)
    hasconst.append(8)
    hasarg.append(8)
    if "STORE_FIELD" not in specializations:
        specializations["STORE_FIELD"] = []
    specializations["STORE_FIELD"].append("STORE_PRIMITIVE_FIELD")
    def_op("BUILD_CHECKED_LIST", 9)
    hasconst.append(9)
    hasarg.append(9)
    cache_format["BUILD_CHECKED_LIST"] = "{'cache': 2}"
    def_op("BUILD_CHECKED_LIST_CACHED", 10)
    hasconst.append(10)
    hasarg.append(10)
    if "BUILD_CHECKED_LIST" not in specializations:
        specializations["BUILD_CHECKED_LIST"] = []
    specializations["BUILD_CHECKED_LIST"].append("BUILD_CHECKED_LIST_CACHED")
    def_op("LOAD_TYPE", 11)
    hasconst.append(11)
    hasarg.append(11)
    def_op("CAST", 12)
    hasconst.append(12)
    hasarg.append(12)
    cache_format["CAST"] = "{'cache': 2}"
    def_op("CAST_CACHED", 13)
    hasconst.append(13)
    hasarg.append(13)
    if "CAST" not in specializations:
        specializations["CAST"] = []
    specializations["CAST"].append("CAST_CACHED")
    def_op("LOAD_LOCAL", 14)
    hasconst.append(14)
    hasarg.append(14)
    def_op("STORE_LOCAL", 15)
    hasconst.append(15)
    hasarg.append(15)
    cache_format["STORE_LOCAL"] = "{'cache': 1}"
    def_op("STORE_LOCAL_CACHED", 16)
    hasconst.append(16)
    hasarg.append(16)
    if "STORE_LOCAL" not in specializations:
        specializations["STORE_LOCAL"] = []
    specializations["STORE_LOCAL"].append("STORE_LOCAL_CACHED")
    def_op("PRIMITIVE_BOX", 17)
    hasarg.append(17)
    jrel_op("POP_JUMP_IF_ZERO", 100)
    hasarg.append(100)
    inline_cache_entries["POP_JUMP_IF_ZERO"] = 1
    jrel_op("POP_JUMP_IF_NONZERO", 103)
    hasarg.append(103)
    inline_cache_entries["POP_JUMP_IF_NONZERO"] = 1
    def_op("PRIMITIVE_UNBOX", 18)
    hasarg.append(18)
    def_op("PRIMITIVE_BINARY_OP", 19)
    hasarg.append(19)
    def_op("PRIMITIVE_UNARY_OP", 20)
    hasarg.append(20)
    def_op("PRIMITIVE_COMPARE_OP", 21)
    hasarg.append(21)
    def_op("LOAD_ITERABLE_ARG", 22)
    hasarg.append(22)
    def_op("LOAD_MAPPING_ARG", 23)
    hasarg.append(23)
    def_op("INVOKE_FUNCTION", 24)
    hasconst.append(24)
    hasarg.append(24)
    cache_format["INVOKE_FUNCTION"] = "{'cache': 4}"
    def_op("INVOKE_FUNCTION_CACHED", 25)
    hasconst.append(25)
    hasarg.append(25)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_FUNCTION_CACHED")
    def_op("INVOKE_INDIRECT_CACHED", 26)
    hasconst.append(26)
    hasarg.append(26)
    if "INVOKE_FUNCTION" not in specializations:
        specializations["INVOKE_FUNCTION"] = []
    specializations["INVOKE_FUNCTION"].append("INVOKE_INDIRECT_CACHED")
    def_op("FAST_LEN", 27)
    hasarg.append(27)
    def_op("CONVERT_PRIMITIVE", 28)
    hasarg.append(28)
    def_op("INVOKE_NATIVE", 29)
    hasconst.append(29)
    hasarg.append(29)
    def_op("LOAD_CLASS", 30)
    hasconst.append(30)
    hasarg.append(30)
    def_op("BUILD_CHECKED_MAP", 31)
    hasconst.append(31)
    hasarg.append(31)
    cache_format["BUILD_CHECKED_MAP"] = "{'cache': 2}"
    def_op("BUILD_CHECKED_MAP_CACHED", 32)
    hasconst.append(32)
    hasarg.append(32)
    if "BUILD_CHECKED_MAP" not in specializations:
        specializations["BUILD_CHECKED_MAP"] = []
    specializations["BUILD_CHECKED_MAP"].append("BUILD_CHECKED_MAP_CACHED")
    def_op("SEQUENCE_GET", 33)
    hasarg.append(33)
    def_op("SEQUENCE_SET", 34)
    hasarg.append(34)
    def_op("LIST_DEL", 35)
    def_op("REFINE_TYPE", 36)
    hasconst.append(36)
    hasarg.append(36)
    def_op("PRIMITIVE_LOAD_CONST", 37)
    hasconst.append(37)
    hasarg.append(37)
    def_op("RETURN_PRIMITIVE", 40)
    hasarg.append(40)
    def_op("TP_ALLOC", 41)
    hasconst.append(41)
    hasarg.append(41)
    cache_format["TP_ALLOC"] = "{'cache': 2}"
    def_op("TP_ALLOC_CACHED", 42)
    hasconst.append(42)
    hasarg.append(42)
    if "TP_ALLOC" not in specializations:
        specializations["TP_ALLOC"] = []
    specializations["TP_ALLOC"].append("TP_ALLOC_CACHED")
    def_op("LOAD_METHOD_STATIC", 43)
    hasconst.append(43)
    hasarg.append(43)
    cache_format["LOAD_METHOD_STATIC"] = "{'cache': 2}"
    def_op("LOAD_METHOD_STATIC_CACHED", 45)
    hasconst.append(45)
    hasarg.append(45)
    if "LOAD_METHOD_STATIC" not in specializations:
        specializations["LOAD_METHOD_STATIC"] = []
    specializations["LOAD_METHOD_STATIC"].append("LOAD_METHOD_STATIC_CACHED")

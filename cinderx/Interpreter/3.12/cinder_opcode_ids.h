// Copyright (c) Meta Platforms, Inc. and affiliates.

#ifndef Py_OPCODE_H
#define Py_OPCODE_H
#ifdef __cplusplus
extern "C" {
#endif


/* Instruction opcodes for compiled code */

#define PY_OPCODES(X) \
  X(CACHE,                               0) \
  X(POP_TOP,                             1) \
  X(PUSH_NULL,                           2) \
  X(INTERPRETER_EXIT,                    3) \
  X(END_FOR,                             4) \
  X(END_SEND,                            5) \
  X(NOP,                                 9) \
  X(UNARY_NEGATIVE,                     11) \
  X(UNARY_NOT,                          12) \
  X(UNARY_INVERT,                       15) \
  X(RESERVED,                           17) \
  X(BINARY_SUBSCR,                      25) \
  X(BINARY_SLICE,                       26) \
  X(STORE_SLICE,                        27) \
  X(GET_LEN,                            30) \
  X(MATCH_MAPPING,                      31) \
  X(MATCH_SEQUENCE,                     32) \
  X(MATCH_KEYS,                         33) \
  X(PUSH_EXC_INFO,                      35) \
  X(CHECK_EXC_MATCH,                    36) \
  X(CHECK_EG_MATCH,                     37) \
  X(WITH_EXCEPT_START,                  49) \
  X(GET_AITER,                          50) \
  X(GET_ANEXT,                          51) \
  X(BEFORE_ASYNC_WITH,                  52) \
  X(BEFORE_WITH,                        53) \
  X(END_ASYNC_FOR,                      54) \
  X(CLEANUP_THROW,                      55) \
  X(STORE_SUBSCR,                       60) \
  X(DELETE_SUBSCR,                      61) \
  X(GET_ITER,                           68) \
  X(GET_YIELD_FROM_ITER,                69) \
  X(LOAD_BUILD_CLASS,                   71) \
  X(LOAD_ASSERTION_ERROR,               74) \
  X(RETURN_GENERATOR,                   75) \
  X(RETURN_VALUE,                       83) \
  X(SETUP_ANNOTATIONS,                  85) \
  X(LOAD_LOCALS,                        87) \
  X(POP_EXCEPT,                         89) \
  X(HAVE_ARGUMENT,                      90) \
  X(STORE_NAME,                         90) \
  X(DELETE_NAME,                        91) \
  X(UNPACK_SEQUENCE,                    92) \
  X(FOR_ITER,                           93) \
  X(UNPACK_EX,                          94) \
  X(STORE_ATTR,                         95) \
  X(DELETE_ATTR,                        96) \
  X(STORE_GLOBAL,                       97) \
  X(DELETE_GLOBAL,                      98) \
  X(SWAP,                               99) \
  X(LOAD_CONST,                        100) \
  X(LOAD_NAME,                         101) \
  X(BUILD_TUPLE,                       102) \
  X(BUILD_LIST,                        103) \
  X(BUILD_SET,                         104) \
  X(BUILD_MAP,                         105) \
  X(LOAD_ATTR,                         106) \
  X(COMPARE_OP,                        107) \
  X(EAGER_IMPORT_NAME,                 108) \
  X(IMPORT_FROM,                       109) \
  X(JUMP_FORWARD,                      110) \
  X(POP_JUMP_IF_FALSE,                 114) \
  X(POP_JUMP_IF_TRUE,                  115) \
  X(LOAD_GLOBAL,                       116) \
  X(IS_OP,                             117) \
  X(CONTAINS_OP,                       118) \
  X(RERAISE,                           119) \
  X(COPY,                              120) \
  X(RETURN_CONST,                      121) \
  X(BINARY_OP,                         122) \
  X(SEND,                              123) \
  X(LOAD_FAST,                         124) \
  X(STORE_FAST,                        125) \
  X(DELETE_FAST,                       126) \
  X(LOAD_FAST_CHECK,                   127) \
  X(POP_JUMP_IF_NOT_NONE,              128) \
  X(POP_JUMP_IF_NONE,                  129) \
  X(RAISE_VARARGS,                     130) \
  X(GET_AWAITABLE,                     131) \
  X(MAKE_FUNCTION,                     132) \
  X(BUILD_SLICE,                       133) \
  X(JUMP_BACKWARD_NO_INTERRUPT,        134) \
  X(MAKE_CELL,                         135) \
  X(LOAD_CLOSURE,                      136) \
  X(LOAD_DEREF,                        137) \
  X(STORE_DEREF,                       138) \
  X(DELETE_DEREF,                      139) \
  X(JUMP_BACKWARD,                     140) \
  X(LOAD_SUPER_ATTR,                   141) \
  X(CALL_FUNCTION_EX,                  142) \
  X(LOAD_FAST_AND_CLEAR,               143) \
  X(EXTENDED_ARG,                      144) \
  X(LIST_APPEND,                       145) \
  X(SET_ADD,                           146) \
  X(MAP_ADD,                           147) \
  X(COPY_FREE_VARS,                    149) \
  X(YIELD_VALUE,                       150) \
  X(RESUME,                            151) \
  X(MATCH_CLASS,                       152) \
  X(FORMAT_VALUE,                      155) \
  X(BUILD_CONST_KEY_MAP,               156) \
  X(BUILD_STRING,                      157) \
  X(LIST_EXTEND,                       162) \
  X(SET_UPDATE,                        163) \
  X(DICT_MERGE,                        164) \
  X(DICT_UPDATE,                       165) \
  X(CALL,                              171) \
  X(KW_NAMES,                          172) \
  X(CALL_INTRINSIC_1,                  173) \
  X(CALL_INTRINSIC_2,                  174) \
  X(LOAD_FROM_DICT_OR_GLOBALS,         175) \
  X(LOAD_FROM_DICT_OR_DEREF,           176) \
  X(IMPORT_NAME,                       183) \
  X(INVOKE_METHOD,                     185) \
  X(LOAD_FIELD,                        186) \
  X(LOAD_OBJ_FIELD,                    187) \
  X(LOAD_PRIMITIVE_FIELD,              188) \
  X(STORE_FIELD,                       189) \
  X(STORE_OBJ_FIELD,                   190) \
  X(STORE_PRIMITIVE_FIELD,             191) \
  X(BUILD_CHECKED_LIST,                192) \
  X(BUILD_CHECKED_LIST_CACHED,         193) \
  X(LOAD_TYPE,                         194) \
  X(CAST,                              195) \
  X(CAST_CACHED,                       196) \
  X(LOAD_LOCAL,                        197) \
  X(STORE_LOCAL,                       198) \
  X(STORE_LOCAL_CACHED,                199) \
  X(PRIMITIVE_BOX,                     200) \
  X(POP_JUMP_IF_ZERO,                  201) \
  X(POP_JUMP_IF_NONZERO,               202) \
  X(PRIMITIVE_UNBOX,                   203) \
  X(PRIMITIVE_BINARY_OP,               204) \
  X(PRIMITIVE_UNARY_OP,                205) \
  X(PRIMITIVE_COMPARE_OP,              206) \
  X(LOAD_ITERABLE_ARG,                 207) \
  X(LOAD_MAPPING_ARG,                  208) \
  X(INVOKE_FUNCTION,                   209) \
  X(INVOKE_FUNCTION_CACHED,            210) \
  X(INVOKE_INDIRECT_CACHED,            211) \
  X(JUMP_IF_ZERO_OR_POP,               212) \
  X(JUMP_IF_NONZERO_OR_POP,            213) \
  X(FAST_LEN,                          214) \
  X(CONVERT_PRIMITIVE,                 215) \
  X(INVOKE_NATIVE,                     216) \
  X(LOAD_CLASS,                        217) \
  X(BUILD_CHECKED_MAP,                 218) \
  X(BUILD_CHECKED_MAP_CACHED,          219) \
  X(SEQUENCE_GET,                      220) \
  X(SEQUENCE_SET,                      221) \
  X(LIST_DEL,                          222) \
  X(REFINE_TYPE,                       223) \
  X(PRIMITIVE_LOAD_CONST,              224) \
  X(RETURN_PRIMITIVE,                  225) \
  X(TP_ALLOC,                          226) \
  X(TP_ALLOC_CACHED,                   227) \
  X(LOAD_METHOD_STATIC,                228) \
  X(LOAD_METHOD_STATIC_CACHED,         230) \
  X(MIN_INSTRUMENTED_OPCODE,           237) \
  X(INSTRUMENTED_LOAD_SUPER_ATTR,      237) \
  X(INSTRUMENTED_POP_JUMP_IF_NONE,     238) \
  X(INSTRUMENTED_POP_JUMP_IF_NOT_NONE, 239) \
  X(INSTRUMENTED_RESUME,               240) \
  X(INSTRUMENTED_CALL,                 241) \
  X(INSTRUMENTED_RETURN_VALUE,         242) \
  X(INSTRUMENTED_YIELD_VALUE,          243) \
  X(INSTRUMENTED_CALL_FUNCTION_EX,     244) \
  X(INSTRUMENTED_JUMP_FORWARD,         245) \
  X(INSTRUMENTED_JUMP_BACKWARD,        246) \
  X(INSTRUMENTED_RETURN_CONST,         247) \
  X(INSTRUMENTED_FOR_ITER,             248) \
  X(INSTRUMENTED_POP_JUMP_IF_FALSE,    249) \
  X(INSTRUMENTED_POP_JUMP_IF_TRUE,     250) \
  X(INSTRUMENTED_END_FOR,              251) \
  X(INSTRUMENTED_END_SEND,             252) \
  X(INSTRUMENTED_INSTRUCTION,          253) \
  X(INSTRUMENTED_LINE,                 254) \
  X(BINARY_OP_ADD_FLOAT,                 6) \
  X(BINARY_OP_ADD_INT,                   7) \
  X(BINARY_OP_ADD_UNICODE,               8) \
  X(BINARY_OP_INPLACE_ADD_UNICODE,      10) \
  X(BINARY_OP_MULTIPLY_FLOAT,           13) \
  X(BINARY_OP_MULTIPLY_INT,             14) \
  X(BINARY_OP_SUBTRACT_FLOAT,           16) \
  X(BINARY_OP_SUBTRACT_INT,             18) \
  X(BINARY_SUBSCR_DICT,                 19) \
  X(BINARY_SUBSCR_GETITEM,              20) \
  X(BINARY_SUBSCR_LIST_INT,             21) \
  X(BINARY_SUBSCR_TUPLE_INT,            22) \
  X(CALL_PY_EXACT_ARGS,                 23) \
  X(CALL_PY_WITH_DEFAULTS,              24) \
  X(CALL_BOUND_METHOD_EXACT_ARGS,       28) \
  X(CALL_BUILTIN_CLASS,                 29) \
  X(CALL_BUILTIN_FAST_WITH_KEYWORDS,    34) \
  X(CALL_METHOD_DESCRIPTOR_FAST_WITH_KEYWORDS, 38   ) \
  X(CALL_NO_KW_BUILTIN_FAST,            39) \
  X(CALL_NO_KW_BUILTIN_O,               40) \
  X(CALL_NO_KW_ISINSTANCE,              41) \
  X(CALL_NO_KW_LEN,                     42) \
  X(CALL_NO_KW_LIST_APPEND,             43) \
  X(CALL_NO_KW_METHOD_DESCRIPTOR_FAST,  44) \
  X(CALL_NO_KW_METHOD_DESCRIPTOR_NOARGS, 45) \
  X(CALL_NO_KW_METHOD_DESCRIPTOR_O,     46) \
  X(CALL_NO_KW_STR_1,                   47) \
  X(CALL_NO_KW_TUPLE_1,                 48) \
  X(CALL_NO_KW_TYPE_1,                  56) \
  X(COMPARE_OP_FLOAT,                   57) \
  X(COMPARE_OP_INT,                     58) \
  X(COMPARE_OP_STR,                     59) \
  X(FOR_ITER_LIST,                      62) \
  X(FOR_ITER_TUPLE,                     63) \
  X(FOR_ITER_RANGE,                     64) \
  X(FOR_ITER_GEN,                       65) \
  X(LOAD_SUPER_ATTR_ATTR,               66) \
  X(LOAD_SUPER_ATTR_METHOD,             67) \
  X(LOAD_ATTR_CLASS,                    70) \
  X(LOAD_ATTR_GETATTRIBUTE_OVERRIDDEN,  72) \
  X(LOAD_ATTR_INSTANCE_VALUE,           73) \
  X(LOAD_ATTR_MODULE,                   76) \
  X(LOAD_ATTR_PROPERTY,                 77) \
  X(LOAD_ATTR_SLOT,                     78) \
  X(LOAD_ATTR_WITH_HINT,                79) \
  X(LOAD_ATTR_METHOD_LAZY_DICT,         80) \
  X(LOAD_ATTR_METHOD_NO_DICT,           81) \
  X(LOAD_ATTR_METHOD_WITH_VALUES,       82) \
  X(LOAD_CONST__LOAD_FAST,              84) \
  X(LOAD_FAST__LOAD_CONST,              86) \
  X(LOAD_FAST__LOAD_FAST,               88) \
  X(LOAD_GLOBAL_BUILTIN,               111) \
  X(LOAD_GLOBAL_MODULE,                112) \
  X(STORE_ATTR_INSTANCE_VALUE,         113) \
  X(STORE_ATTR_SLOT,                   148) \
  X(STORE_ATTR_WITH_HINT,              153) \
  X(STORE_FAST__LOAD_FAST,             154) \
  X(STORE_FAST__STORE_FAST,            158) \
  X(STORE_SUBSCR_DICT,                 159) \
  X(STORE_SUBSCR_LIST_INT,             160) \
  X(UNPACK_SEQUENCE_LIST,              161) \
  X(UNPACK_SEQUENCE_TUPLE,             166) \
  X(UNPACK_SEQUENCE_TWO_TUPLE,         167) \
  X(SEND_GEN,                          168)

#define MIN_PSEUDO_OPCODE                      256
#define SETUP_FINALLY                          256
#define SETUP_CLEANUP                          257
#define SETUP_WITH                             258
#define POP_BLOCK                              259
#define JUMP                                   260
#define JUMP_NO_INTERRUPT                      261
#define LOAD_METHOD                            262
#define LOAD_SUPER_METHOD                      263
#define LOAD_ZERO_SUPER_METHOD                 264
#define LOAD_ZERO_SUPER_ATTR                   265
#define STORE_FAST_MAYBE_NULL                  266
#define MAX_PSEUDO_OPCODE                      266

#define HAS_ARG(op) ((((op) >= HAVE_ARGUMENT) && (!IS_PSEUDO_OPCODE(op)))\
    || ((op) == JUMP) \
    || ((op) == JUMP_NO_INTERRUPT) \
    || ((op) == LOAD_METHOD) \
    || ((op) == LOAD_SUPER_METHOD) \
    || ((op) == LOAD_ZERO_SUPER_METHOD) \
    || ((op) == LOAD_ZERO_SUPER_ATTR) \
    || ((op) == STORE_FAST_MAYBE_NULL) \
    )

#define HAS_CONST(op) (false\
    || ((op) == LOAD_CONST) \
    || ((op) == RETURN_CONST) \
    || ((op) == KW_NAMES) \
    || ((op) == INVOKE_METHOD) \
    || ((op) == LOAD_FIELD) \
    || ((op) == LOAD_OBJ_FIELD) \
    || ((op) == LOAD_PRIMITIVE_FIELD) \
    || ((op) == STORE_FIELD) \
    || ((op) == STORE_OBJ_FIELD) \
    || ((op) == STORE_PRIMITIVE_FIELD) \
    || ((op) == BUILD_CHECKED_LIST) \
    || ((op) == BUILD_CHECKED_LIST_CACHED) \
    || ((op) == LOAD_TYPE) \
    || ((op) == CAST) \
    || ((op) == CAST_CACHED) \
    || ((op) == LOAD_LOCAL) \
    || ((op) == STORE_LOCAL) \
    || ((op) == STORE_LOCAL_CACHED) \
    || ((op) == INVOKE_FUNCTION) \
    || ((op) == INVOKE_FUNCTION_CACHED) \
    || ((op) == INVOKE_INDIRECT_CACHED) \
    || ((op) == INVOKE_NATIVE) \
    || ((op) == LOAD_CLASS) \
    || ((op) == BUILD_CHECKED_MAP) \
    || ((op) == BUILD_CHECKED_MAP_CACHED) \
    || ((op) == REFINE_TYPE) \
    || ((op) == PRIMITIVE_LOAD_CONST) \
    || ((op) == TP_ALLOC) \
    || ((op) == TP_ALLOC_CACHED) \
    || ((op) == LOAD_METHOD_STATIC) \
    || ((op) == LOAD_METHOD_STATIC_CACHED) \
    )

#define NB_ADD                                   0
#define NB_AND                                   1
#define NB_FLOOR_DIVIDE                          2
#define NB_LSHIFT                                3
#define NB_MATRIX_MULTIPLY                       4
#define NB_MULTIPLY                              5
#define NB_REMAINDER                             6
#define NB_OR                                    7
#define NB_POWER                                 8
#define NB_RSHIFT                                9
#define NB_SUBTRACT                             10
#define NB_TRUE_DIVIDE                          11
#define NB_XOR                                  12
#define NB_INPLACE_ADD                          13
#define NB_INPLACE_AND                          14
#define NB_INPLACE_FLOOR_DIVIDE                 15
#define NB_INPLACE_LSHIFT                       16
#define NB_INPLACE_MATRIX_MULTIPLY              17
#define NB_INPLACE_MULTIPLY                     18
#define NB_INPLACE_REMAINDER                    19
#define NB_INPLACE_OR                           20
#define NB_INPLACE_POWER                        21
#define NB_INPLACE_RSHIFT                       22
#define NB_INPLACE_SUBTRACT                     23
#define NB_INPLACE_TRUE_DIVIDE                  24
#define NB_INPLACE_XOR                          25

/* Defined in Lib/opcode.py */
#define ENABLE_SPECIALIZATION 1
enum {
#define OP(op, value) op = value,
PY_OPCODES(OP)
#undef OP
};

#define IS_PSEUDO_OPCODE(op) (((op) >= MIN_PSEUDO_OPCODE) && ((op) <= MAX_PSEUDO_OPCODE))

#ifdef __cplusplus
}
#endif
#endif /* !Py_OPCODE_H */

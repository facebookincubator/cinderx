// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

// Define all bytecodes that appeared or were removed between 3.10 and 3.12.
// Bytecodes that don't match the Python version being used for the current
// build will be intentionally defined to out-of-range values (i.e. >255) so as
// to not be reachable by the interpreter, or any utilities that read or write
// to code objects.
//
// Having them defined across all builds means there can be less Python version
// checks in the compiler.

#if PY_VERSION_HEX < 0x030C0000

#define STUB_OPCODE_DEFS(X)            \
  X(BEFORE_WITH)                       \
  X(BINARY_OP)                         \
  X(BINARY_SLICE)                      \
  X(CACHE)                             \
  X(CALL)                              \
  X(CALL_INTRINSIC_1)                  \
  X(CALL_INTRINSIC_2)                  \
  X(CHECK_EG_MATCH)                    \
  X(CHECK_EXC_MATCH)                   \
  X(CLEANUP_THROW)                     \
  X(COPY)                              \
  X(COPY_FREE_VARS)                    \
  X(END_FOR)                           \
  X(END_SEND)                          \
  X(INSTRUMENTED_CALL)                 \
  X(INSTRUMENTED_CALL_FUNCTION_EX)     \
  X(INSTRUMENTED_END_FOR)              \
  X(INSTRUMENTED_END_SEND)             \
  X(INSTRUMENTED_FOR_ITER)             \
  X(INSTRUMENTED_INSTRUCTION)          \
  X(INSTRUMENTED_JUMP_BACKWARD)        \
  X(INSTRUMENTED_JUMP_FORWARD)         \
  X(INSTRUMENTED_LINE)                 \
  X(INSTRUMENTED_LOAD_SUPER_ATTR)      \
  X(INSTRUMENTED_POP_JUMP_IF_FALSE)    \
  X(INSTRUMENTED_POP_JUMP_IF_NONE)     \
  X(INSTRUMENTED_POP_JUMP_IF_NOT_NONE) \
  X(INSTRUMENTED_POP_JUMP_IF_TRUE)     \
  X(INSTRUMENTED_RESUME)               \
  X(INSTRUMENTED_RETURN_CONST)         \
  X(INSTRUMENTED_RETURN_VALUE)         \
  X(INSTRUMENTED_YIELD_VALUE)          \
  X(JUMP_BACKWARD)                     \
  X(JUMP_BACKWARD_NO_INTERRUPT)        \
  X(KW_NAMES)                          \
  X(LOAD_FAST_AND_CLEAR)               \
  X(LOAD_FAST_CHECK)                   \
  X(LOAD_FROM_DICT_OR_DEREF)           \
  X(LOAD_FROM_DICT_OR_GLOBALS)         \
  X(LOAD_LOCALS)                       \
  X(LOAD_SUPER_ATTR)                   \
  X(MAKE_CELL)                         \
  X(POP_JUMP_IF_NONE)                  \
  X(POP_JUMP_IF_NOT_NONE)              \
  X(PUSH_EXC_INFO)                     \
  X(PUSH_NULL)                         \
  X(RESUME)                            \
  X(RETURN_CONST)                      \
  X(RETURN_GENERATOR)                  \
  X(SEND)                              \
  X(STORE_SLICE)                       \
  X(SWAP)

#define STUB_NB_DEFS(X) \
  X(ADD)                \
  X(AND)                \
  X(FLOOR_DIVIDE)       \
  X(LSHIFT)             \
  X(MATRIX_MULTIPLY)    \
  X(MULTIPLY)           \
  X(REMAINDER)          \
  X(OR)                 \
  X(POWER)              \
  X(RSHIFT)             \
  X(SUBTRACT)           \
  X(TRUE_DIVIDE)        \
  X(XOR)

enum {
#define DEFINE_NB(X) NB_##X,
  STUB_NB_DEFS(DEFINE_NB)
#undef DEFINE_NB
#define DEFINE_NB_INPLACE(X) NB_INPLACE_##X,
      STUB_NB_DEFS(DEFINE_NB_INPLACE)
#undef DEFINE_NB_INPLACE
};

#else

#define STUB_OPCODE_DEFS(X)        \
  X(BINARY_ADD)                    \
  X(BINARY_AND)                    \
  X(BINARY_FLOOR_DIVIDE)           \
  X(BINARY_LSHIFT)                 \
  X(BINARY_MATRIX_MULTIPLY)        \
  X(BINARY_MODULO)                 \
  X(BINARY_MULTIPLY)               \
  X(BINARY_OR)                     \
  X(BINARY_POWER)                  \
  X(BINARY_RSHIFT)                 \
  X(BINARY_SUBSCR_DICT_STR)        \
  X(BINARY_SUBSCR_LIST)            \
  X(BINARY_SUBSCR_TUPLE)           \
  X(BINARY_SUBSCR_TUPLE_CONST_INT) \
  X(BINARY_SUBTRACT)               \
  X(BINARY_TRUE_DIVIDE)            \
  X(BINARY_XOR)                    \
  X(CALL_FUNCTION)                 \
  X(CALL_FUNCTION_KW)              \
  X(CALL_METHOD)                   \
  X(COPY_DICT_WITHOUT_KEYS)        \
  X(DUP_TOP)                       \
  X(DUP_TOP_TWO)                   \
  X(GEN_START)                     \
  X(INPLACE_ADD)                   \
  X(INPLACE_AND)                   \
  X(INPLACE_FLOOR_DIVIDE)          \
  X(INPLACE_LSHIFT)                \
  X(INPLACE_MATRIX_MULTIPLY)       \
  X(INPLACE_MODULO)                \
  X(INPLACE_MULTIPLY)              \
  X(INPLACE_OR)                    \
  X(INPLACE_POWER)                 \
  X(INPLACE_RSHIFT)                \
  X(INPLACE_SUBTRACT)              \
  X(INPLACE_TRUE_DIVIDE)           \
  X(INPLACE_XOR)                   \
  X(JUMP_ABSOLUTE)                 \
  X(JUMP_IF_FALSE_OR_POP)          \
  X(JUMP_IF_NOT_EXC_MATCH)         \
  X(JUMP_IF_TRUE_OR_POP)           \
  X(LIST_TO_TUPLE)                 \
  X(LOAD_ATTR_DICT_DESCR)          \
  X(LOAD_ATTR_DICT_NO_DESCR)       \
  X(LOAD_ATTR_NO_DICT_DESCR)       \
  X(LOAD_ATTR_POLYMORPHIC)         \
  X(LOAD_ATTR_SPLIT_DICT)          \
  X(LOAD_ATTR_SPLIT_DICT_DESCR)    \
  X(LOAD_ATTR_SUPER)               \
  X(LOAD_ATTR_S_MODULE)            \
  X(LOAD_ATTR_TYPE)                \
  X(LOAD_ATTR_UNCACHABLE)          \
  X(LOAD_METHOD_DICT_DESCR)        \
  X(LOAD_METHOD_DICT_METHOD)       \
  X(LOAD_METHOD_MODULE)            \
  X(LOAD_METHOD_NO_DICT_DESCR)     \
  X(LOAD_METHOD_NO_DICT_METHOD)    \
  X(LOAD_METHOD_SPLIT_DICT_DESCR)  \
  X(LOAD_METHOD_SPLIT_DICT_METHOD) \
  X(LOAD_METHOD_SUPER)             \
  X(LOAD_METHOD_S_MODULE)          \
  X(LOAD_METHOD_TYPE)              \
  X(LOAD_METHOD_TYPE_METHODLIKE)   \
  X(LOAD_METHOD_UNCACHABLE)        \
  X(LOAD_METHOD_UNSHADOWED_METHOD) \
  X(LOAD_PRIMITIVE_FIELD)          \
  X(MAKE_OPNAME)                   \
  X(ROT_FOUR)                      \
  X(ROT_N)                         \
  X(ROT_THREE)                     \
  X(ROT_TWO)                       \
  X(SETUP_ASYNC_WITH)              \
  X(STORE_ATTR_DESCR)              \
  X(STORE_ATTR_DICT)               \
  X(STORE_ATTR_SPLIT_DICT)         \
  X(STORE_ATTR_UNCACHABLE)         \
  X(STORE_PRIMITIVE_FIELD)         \
  X(UNARY_POSITIVE)                \
  X(YIELD_FROM)
#endif

enum {
  // Uses magic value to put these in a completely bogus range that doesn't fit
  // in one byte.  Fits in two bytes which matches how CPython handles pseudo
  // opcodes.
  STUB_OPCODE_BEGIN = 40000,
#define DEFINE_OPCODE(NAME) NAME,
  STUB_OPCODE_DEFS(DEFINE_OPCODE)
#undef DEFINE_OPCODE
};

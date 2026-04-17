// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

// Define all bytecodes that appeared or were removed from <lowest version of
// Python we support> and <highest version of Python we support>. Bytecodes that
// don't match the Python version being used for the current build will be
// intentionally defined to out-of-range values (i.e. >255) so as to not be
// reachable by the interpreter, or any utilities that read or write to code
// objects.
//
// Having them defined across all builds means there can be less Python version
// checks in the compiler.

#if PY_VERSION_HEX < 0x030E0000

#define STUB_OPCODE_DEFS(X)            \
  X(BINARY_ADD)                        \
  X(BINARY_AND)                        \
  X(BINARY_FLOOR_DIVIDE)               \
  X(BINARY_LSHIFT)                     \
  X(BINARY_MATRIX_MULTIPLY)            \
  X(BINARY_MODULO)                     \
  X(BINARY_MULTIPLY)                   \
  X(BINARY_OR)                         \
  X(BINARY_POWER)                      \
  X(BINARY_RSHIFT)                     \
  X(BINARY_SUBSCR_DICT_STR)            \
  X(BINARY_SUBSCR_LIST)                \
  X(BINARY_SUBSCR_TUPLE)               \
  X(BINARY_SUBSCR_TUPLE_CONST_INT)     \
  X(BINARY_SUBTRACT)                   \
  X(BINARY_TRUE_DIVIDE)                \
  X(BINARY_XOR)                        \
  X(BUILD_INTERPOLATION)               \
  X(BUILD_TEMPLATE)                    \
  X(CALL_FUNCTION)                     \
  X(CALL_FUNCTION_KW)                  \
  X(CALL_KW)                           \
  X(CALL_METHOD)                       \
  X(CONVERT_VALUE)                     \
  X(COPY_DICT_WITHOUT_KEYS)            \
  X(DUP_TOP)                           \
  X(DUP_TOP_TWO)                       \
  X(FORMAT_WITH_SPEC)                  \
  X(FORMAT_SIMPLE)                     \
  X(EXTENDED_OPCODE)                   \
  X(GEN_START)                         \
  X(INPLACE_ADD)                       \
  X(INPLACE_AND)                       \
  X(INPLACE_FLOOR_DIVIDE)              \
  X(INPLACE_LSHIFT)                    \
  X(INPLACE_MATRIX_MULTIPLY)           \
  X(INPLACE_MODULO)                    \
  X(INPLACE_MULTIPLY)                  \
  X(INPLACE_OR)                        \
  X(INPLACE_POWER)                     \
  X(INPLACE_RSHIFT)                    \
  X(INPLACE_SUBTRACT)                  \
  X(INPLACE_TRUE_DIVIDE)               \
  X(INPLACE_XOR)                       \
  X(JUMP_ABSOLUTE)                     \
  X(JUMP_IF_FALSE_OR_POP)              \
  X(JUMP_IF_NOT_EXC_MATCH)             \
  X(JUMP_IF_TRUE_OR_POP)               \
  X(LIST_TO_TUPLE)                     \
  X(LOAD_ATTR_DICT_DESCR)              \
  X(LOAD_ATTR_DICT_NO_DESCR)           \
  X(LOAD_ATTR_NO_DICT_DESCR)           \
  X(LOAD_ATTR_POLYMORPHIC)             \
  X(LOAD_ATTR_SPLIT_DICT)              \
  X(LOAD_ATTR_SPLIT_DICT_DESCR)        \
  X(LOAD_ATTR_SUPER)                   \
  X(LOAD_ATTR_S_MODULE)                \
  X(LOAD_ATTR_TYPE)                    \
  X(LOAD_ATTR_UNCACHABLE)              \
  X(LOAD_COMMON_CONSTANT)              \
  X(LOAD_FAST_BORROW)                  \
  X(LOAD_FAST_BORROW_LOAD_FAST_BORROW) \
  X(LOAD_FAST_LOAD_FAST)               \
  X(LOAD_METHOD_DICT_DESCR)            \
  X(LOAD_METHOD_DICT_METHOD)           \
  X(LOAD_METHOD_MODULE)                \
  X(LOAD_METHOD_NO_DICT_DESCR)         \
  X(LOAD_METHOD_NO_DICT_METHOD)        \
  X(LOAD_METHOD_SPLIT_DICT_DESCR)      \
  X(LOAD_METHOD_SPLIT_DICT_METHOD)     \
  X(LOAD_METHOD_SUPER)                 \
  X(LOAD_METHOD_S_MODULE)              \
  X(LOAD_METHOD_TYPE)                  \
  X(LOAD_METHOD_TYPE_METHODLIKE)       \
  X(LOAD_METHOD_UNCACHABLE)            \
  X(LOAD_METHOD_UNSHADOWED_METHOD)     \
  X(LOAD_SMALL_INT)                    \
  X(LOAD_SPECIAL)                      \
  X(MAKE_OPNAME)                       \
  X(NOT_TAKEN)                         \
  X(POP_ITER)                          \
  X(ROT_FOUR)                          \
  X(ROT_N)                             \
  X(ROT_THREE)                         \
  X(ROT_TWO)                           \
  X(SETUP_ASYNC_WITH)                  \
  X(STORE_ATTR_DESCR)                  \
  X(STORE_ATTR_DICT)                   \
  X(STORE_ATTR_SPLIT_DICT)             \
  X(SET_FUNCTION_ATTRIBUTE)            \
  X(STORE_ATTR_UNCACHABLE)             \
  X(STORE_FAST_STORE_FAST)             \
  X(STORE_FAST_LOAD_FAST)              \
  X(TO_BOOL)                           \
  X(UNARY_POSITIVE)                    \
  X(YIELD_FROM)

#elif PY_VERSION_HEX < 0x030F0000

#define STUB_OPCODE_DEFS(X)        \
  X(BEFORE_ASYNC_WITH)             \
  X(BEFORE_WITH)                   \
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
  X(BINARY_SUBSCR)                 \
  X(BINARY_SUBSCR_DICT_STR)        \
  X(BINARY_SUBSCR_LIST)            \
  X(BINARY_SUBSCR_TUPLE)           \
  X(BINARY_SUBSCR_TUPLE_CONST_INT) \
  X(BINARY_SUBSCR_TUPLE_INT)       \
  X(BINARY_SUBSCR_LIST_INT)        \
  X(BINARY_SUBSCR_DICT)            \
  X(BINARY_SUBTRACT)               \
  X(BINARY_TRUE_DIVIDE)            \
  X(BINARY_XOR)                    \
  X(BUILD_CONST_KEY_MAP)           \
  X(CALL_FUNCTION)                 \
  X(CALL_FUNCTION_KW)              \
  X(CALL_METHOD)                   \
  X(COPY_DICT_WITHOUT_KEYS)        \
  X(DUP_TOP)                       \
  X(DUP_TOP_TWO)                   \
  X(FORMAT_VALUE)                  \
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
  X(JUMP_IF_NONZERO_OR_POP)        \
  X(JUMP_IF_TRUE_OR_POP)           \
  X(JUMP_IF_ZERO_OR_POP)           \
  X(KW_NAMES)                      \
  X(LIST_TO_TUPLE)                 \
  X(LOAD_ASSERTION_ERROR)          \
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
  X(LOAD_METHOD)                   \
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
  X(MAKE_OPNAME)                   \
  X(RETURN_CONST)                  \
  X(ROT_FOUR)                      \
  X(ROT_N)                         \
  X(ROT_THREE)                     \
  X(ROT_TWO)                       \
  X(SETUP_ASYNC_WITH)              \
  X(STORE_ATTR_DESCR)              \
  X(STORE_ATTR_DICT)               \
  X(STORE_ATTR_SPLIT_DICT)         \
  X(STORE_ATTR_UNCACHABLE)         \
  X(UNARY_POSITIVE)                \
  X(YIELD_FROM)

#else

#define STUB_OPCODE_DEFS(X)        \
  X(BEFORE_ASYNC_WITH)             \
  X(BEFORE_WITH)                   \
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
  X(BINARY_SUBSCR)                 \
  X(BINARY_SUBSCR_DICT_STR)        \
  X(BINARY_SUBSCR_LIST)            \
  X(BINARY_SUBSCR_TUPLE)           \
  X(BINARY_SUBSCR_TUPLE_CONST_INT) \
  X(BINARY_SUBSCR_TUPLE_INT)       \
  X(BINARY_SUBSCR_LIST_INT)        \
  X(BINARY_SUBSCR_DICT)            \
  X(BINARY_SUBTRACT)               \
  X(BINARY_TRUE_DIVIDE)            \
  X(BINARY_XOR)                    \
  X(BUILD_CONST_KEY_MAP)           \
  X(CALL_FUNCTION)                 \
  X(CALL_FUNCTION_KW)              \
  X(CALL_METHOD)                   \
  X(COPY_DICT_WITHOUT_KEYS)        \
  X(DUP_TOP)                       \
  X(DUP_TOP_TWO)                   \
  X(FORMAT_VALUE)                  \
  X(GEN_START)                     \
  X(GET_YIELD_FROM_ITER)           \
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
  X(JUMP_IF_NONZERO_OR_POP)        \
  X(JUMP_IF_TRUE_OR_POP)           \
  X(JUMP_IF_ZERO_OR_POP)           \
  X(KW_NAMES)                      \
  X(LIST_TO_TUPLE)                 \
  X(LOAD_ASSERTION_ERROR)          \
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
  X(LOAD_METHOD)                   \
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
  X(MAKE_OPNAME)                   \
  X(RETURN_CONST)                  \
  X(ROT_FOUR)                      \
  X(ROT_N)                         \
  X(ROT_THREE)                     \
  X(ROT_TWO)                       \
  X(SETUP_ASYNC_WITH)              \
  X(STORE_ATTR_DESCR)              \
  X(STORE_ATTR_DICT)               \
  X(STORE_ATTR_SPLIT_DICT)         \
  X(STORE_ATTR_UNCACHABLE)         \
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

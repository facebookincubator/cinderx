// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/bytecode.h"

#include "cinderx/Interpreter/opcode.h"

#include <unordered_set>

namespace jit {

// these must be opcodes whose oparg is a jump target index
const std::unordered_set<int> kBranchOpcodes = {
    FOR_ITER,
    JUMP_ABSOLUTE,
    JUMP_BACKWARD,
    JUMP_BACKWARD_NO_INTERRUPT,
    JUMP_FORWARD,
    JUMP_IF_FALSE_OR_POP,
    JUMP_IF_NONZERO_OR_POP,
    JUMP_IF_NOT_EXC_MATCH,
    JUMP_IF_TRUE_OR_POP,
    JUMP_IF_ZERO_OR_POP,
    POP_JUMP_IF_FALSE,
    POP_JUMP_IF_NONE,
    POP_JUMP_IF_NONZERO,
    POP_JUMP_IF_NOT_NONE,
    POP_JUMP_IF_TRUE,
    POP_JUMP_IF_ZERO,
};

const std::unordered_set<int> kRelBranchOpcodes = {
    FOR_ITER,
    JUMP_BACKWARD,
    JUMP_BACKWARD_NO_INTERRUPT,
    JUMP_FORWARD,
    POP_JUMP_IF_NONE,
    POP_JUMP_IF_NOT_NONE,
    SEND,
    SETUP_FINALLY,

#if PY_VERSION_HEX >= 0x030B0000
    // These instructions switched from absolute to relative in 3.11.
    POP_JUMP_IF_FALSE,
    POP_JUMP_IF_TRUE,
#endif
};

// we always consider branches block terminators; no need to duplicate them here
const std::unordered_set<int> kBlockTerminatorOpcodes = {
    RAISE_VARARGS,
    RERAISE,
    RETURN_CONST,
    RETURN_PRIMITIVE,
    RETURN_VALUE,
};

} // namespace jit

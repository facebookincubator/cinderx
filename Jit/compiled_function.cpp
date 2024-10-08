// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiled_function.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/disassembler.h"
#include "cinderx/Jit/hir/printer.h"

namespace jit {

void CompiledFunction::disassemble() const {
  JIT_ABORT("disassemble() cannot be called in a release build.");
}

void CompiledFunction::printHIR() const {
  JIT_ABORT("printHIR() cannot be called in a release build.");
}

void CompiledFunctionDebug::disassemble() const {
  jit::disassemble(
      reinterpret_cast<const char*>(vectorcallEntry()),
      codeSize(),
      reinterpret_cast<vma_t>(vectorcallEntry()));
}

void CompiledFunctionDebug::printHIR() const {
  jit::hir::HIRPrinter printer;
  printer.Print(*irfunc_.get());
}

} // namespace jit

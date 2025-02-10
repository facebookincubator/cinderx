// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiled_function.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/disassembler.h"
#include "cinderx/Jit/hir/printer.h"

extern "C" {

bool isJitCompiled(const PyFunctionObject* func) {
  return jit::CodeAllocator::exists() &&
      jit::CodeAllocator::get()->contains(
          reinterpret_cast<const void*>(func->vectorcall));
}

} // extern "C"

namespace jit {

void CompiledFunction::disassemble() const {
  jit::disassemble(
      reinterpret_cast<const char*>(vectorcallEntry()),
      codeSize(),
      reinterpret_cast<vma_t>(vectorcallEntry()));
}

void CompiledFunction::printHIR() const {
  JIT_CHECK(
      irfunc_ != nullptr,
      "Can only call CompiledFunction::printHIR() from a debug build");
  jit::hir::HIRPrinter printer;
  printer.Print(*irfunc_);
}

std::chrono::nanoseconds CompiledFunction::compileTime() const {
  return compile_time_;
}

void CompiledFunction::setCompileTime(std::chrono::nanoseconds time) {
  compile_time_ = time;
}

void CompiledFunction::setHirFunc(std::unique_ptr<hir::Function>&& irfunc) {
  irfunc_ = std::move(irfunc);
}

} // namespace jit

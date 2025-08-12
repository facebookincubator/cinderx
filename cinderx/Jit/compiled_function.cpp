// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiled_function.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/disassembler.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/module_state.h"

extern "C" {

bool isJitCompiled(const PyFunctionObject* func) {
  // Possible that this is called during finalization after the module state is
  // destroyed.
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    return false;
  }
  jit::ICodeAllocator* code_allocator = mod_state->codeAllocator();
  return code_allocator != nullptr &&
      code_allocator->contains(reinterpret_cast<const void*>(func->vectorcall));
}

} // extern "C"

namespace jit {

CompiledFunction::~CompiledFunction() {
  if (runtime_ != nullptr) {
    runtime_->releaseReferences();
  }
}

void CompiledFunction::disassemble() const {
  auto start = reinterpret_cast<const char*>(vectorcallEntry());
  Disassembler dis{start, codeSize()};
  dis.disassembleAll(std::cout);
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

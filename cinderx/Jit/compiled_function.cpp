// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiled_function.h"

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Jit/disassembler.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/module_state.h"

#include <iostream>

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
  if (data_.runtime != nullptr) {
    data_.runtime->releaseReferences();
  }

  auto code_allocator = cinderx::getModuleState()->codeAllocator();
  code_allocator->releaseCode(const_cast<std::byte*>(data_.code.data()));
}

void CompiledFunction::disassemble() const {
  auto start = reinterpret_cast<const char*>(vectorcallEntry());
  Disassembler dis{start, codeSize()};
  dis.disassembleAll(std::cout);
}

void CompiledFunction::printHIR() const {
  JIT_CHECK(
      data_.irfunc != nullptr,
      "Can only call CompiledFunction::printHIR() from a debug build");
  jit::hir::HIRPrinter printer;
  printer.Print(std::cout, *data_.irfunc);
}

std::chrono::nanoseconds CompiledFunction::compileTime() const {
  return data_.compile_time;
}

void CompiledFunction::setCompileTime(std::chrono::nanoseconds time) {
  data_.compile_time = time;
}

void CompiledFunction::setCodePatchers(
    std::vector<std::unique_ptr<CodePatcher>>&& code_patchers) {
  data_.code_patchers = std::move(code_patchers);
}

void CompiledFunction::setHirFunc(std::unique_ptr<hir::Function>&& irfunc) {
  data_.irfunc = std::move(irfunc);
}

void* CompiledFunction::staticEntry() const {
  if (data_.runtime == nullptr ||
      !(data_.runtime->frameState()->code()->co_flags &
        CI_CO_STATICALLY_COMPILED)) {
    return nullptr;
  }

  return reinterpret_cast<void*>(
      JITRT_GET_STATIC_ENTRY(data_.vectorcall_entry));
}

} // namespace jit

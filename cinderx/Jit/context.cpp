// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/context.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/elf/reader.h"

#include <dlfcn.h>

namespace jit {

AotContext g_aot_ctx;

void Context::forgetCode(BorrowedRef<PyFunctionObject> func) {
  compiled_codes_.erase(CompilationKey{func});
}

bool Context::didCompile(BorrowedRef<PyFunctionObject> func) {
  ThreadedCompileSerialize guard;
  return compiled_funcs_.contains(func);
}

CompiledFunction* Context::lookupFunc(BorrowedRef<PyFunctionObject> func) {
  return lookupCode(func->func_code, func->func_builtins, func->func_globals);
}

CodeRuntime* Context::lookupCodeRuntime(BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* compiled = lookupFunc(func);
  if (compiled == nullptr) {
    return nullptr;
  }
  return compiled->runtime();
}

const UnorderedMap<CompilationKey, std::unique_ptr<CompiledFunction>>&
Context::compiledCodes() const {
  return compiled_codes_;
}

const UnorderedSet<BorrowedRef<PyFunctionObject>>& Context::compiledFuncs() {
  return compiled_funcs_;
}

const UnorderedSet<BorrowedRef<PyFunctionObject>>& Context::deoptedFuncs() {
  return deopted_funcs_;
}

void Context::addCompileTime(std::chrono::nanoseconds time) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time);
  total_compile_time_ms_.fetch_add(ms.count(), std::memory_order_relaxed);
}

std::chrono::milliseconds Context::totalCompileTime() const {
  return std::chrono::milliseconds{
      total_compile_time_ms_.load(std::memory_order_relaxed)};
}

void Context::setCinderJitModule(Ref<> mod) {
  cinderjit_module_ = std::move(mod);
}

void Context::clearCache() {
  for (auto& entry : compiled_codes_) {
    orphaned_compiled_codes_.emplace_back(std::move(entry.second));
  }
  compiled_codes_.clear();
}

void Context::funcDestroyed(BorrowedRef<PyFunctionObject> func) {
  compiled_funcs_.erase(func);
  deopted_funcs_.erase(func);

  // This doesn't modify compiled_codes_, so if this is a nested function it can
  // easily be reopted later.
}

CompiledFunction* Context::lookupCode(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals) {
  ThreadedCompileSerialize guard;
  auto it = compiled_codes_.find(CompilationKey{code, builtins, globals});
  return it == compiled_codes_.end() ? nullptr : it->second.get();
}

void Context::addDeoptedFunc(BorrowedRef<PyFunctionObject> func) {
  deopted_funcs_.emplace(func);
}

void Context::removeDeoptedFunc(BorrowedRef<PyFunctionObject> func) {
  deopted_funcs_.erase(func);
}

bool Context::addCompiledFunc(BorrowedRef<PyFunctionObject> func) {
  return compiled_funcs_.emplace(func).second;
}

bool Context::removeCompiledFunc(BorrowedRef<PyFunctionObject> func) {
  return compiled_funcs_.erase(func) == 1;
}

bool Context::addActiveCompile(CompilationKey& key) {
  return active_compiles_.insert(key).second;
}

void Context::removeActiveCompile(CompilationKey& key) {
  active_compiles_.erase(key);
}

CompiledFunction* Context::addCompiledFunction(
    CompilationKey& key,
    std::unique_ptr<CompiledFunction> compiled) {
  auto pair = compiled_codes_.emplace(key, std::move(compiled));
  JIT_CHECK(pair.second, "CompilationKey already present");
  return pair.first->second.get();
}

void AotContext::init(void* bundle_handle) {
  JIT_CHECK(
      bundle_handle_ == nullptr,
      "Trying to register AOT bundle at {} but already have one at {}",
      bundle_handle,
      bundle_handle_);
  bundle_handle_ = bundle_handle;
}

void AotContext::destroy() {
  if (bundle_handle_ == nullptr) {
    return;
  }

  // TASK(T183003853): Unmap compiled functions and empty out private data
  // structures.

  dlclose(bundle_handle_);
  bundle_handle_ = nullptr;
}

void AotContext::registerFunc(const elf::Note& note) {
  elf::CodeNoteData note_data = elf::parseCodeNote(note);
  JIT_LOG("  Function {}", note.name);
  JIT_LOG("    File: {}", note_data.file_name);
  JIT_LOG("    Line: {}", note_data.lineno);
  JIT_LOG("    Hash: {:#x}", note_data.hash);
  JIT_LOG("    Size: {}", note_data.size);
  JIT_LOG("    Normal Entry: +{:#x}", note_data.normal_entry_offset);
  JIT_LOG(
      "    Static Entry: {}",
      note_data.static_entry_offset
          ? fmt::format("+{:#x}", *note_data.static_entry_offset)
          : "");

  // This could use std::piecewise_construct for better efficiency.
  auto [it, inserted] = funcs_.emplace(note.name, FuncState{});
  JIT_CHECK(inserted, "Duplicate ELF note for function '{}'", note.name);
  it->second.note = std::move(note_data);

  // Compute the compiled function's address after dynamic linking.
  void* address = dlsym(bundle_handle_, note.name.c_str());
  JIT_CHECK(
      address != nullptr,
      "Cannot find AOT-compiled function with name '{}' despite successfully "
      "loading the AOT bundle",
      note.name);
  it->second.compiled_code = {
      reinterpret_cast<const std::byte*>(address), it->second.note.size};
  JIT_LOG("    Address: {}", address);
}

const AotContext::FuncState* AotContext::lookupFuncState(
    BorrowedRef<PyFunctionObject> func) {
  std::string name = funcFullname(func);
  auto it = funcs_.find(name);
  return it != funcs_.end() ? &it->second : nullptr;
}

} // namespace jit

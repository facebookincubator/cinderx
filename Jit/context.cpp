// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/context.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/jit_gdb_support.h"

namespace jit {

AotContext g_aot_ctx;

Context::~Context() {
  for (auto it = compiled_funcs_.begin(); it != compiled_funcs_.end();) {
    BorrowedRef<PyFunctionObject> func = *it;
    ++it;
    deoptFuncImpl(func);
  }
}

Context::CompilationResult Context::compilePreloader(
    BorrowedRef<PyFunctionObject> func,
    const hir::Preloader& preloader) {
  CompilationResult result = compilePreloaderImpl(preloader);
  if (result.compiled == nullptr) {
    return result;
  }
  if (func != nullptr) {
    finalizeFunc(func, *result.compiled);
  }
  return result;
}

bool Context::deoptFunc(BorrowedRef<PyFunctionObject> func) {
  if (deoptFuncImpl(func)) {
    deopted_funcs_.emplace(func);
    return true;
  }
  return false;
}

bool Context::reoptFunc(BorrowedRef<PyFunctionObject> func) {
  if (didCompile(func)) {
    return true;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    return false;
  }

  // Might be a nested function that was never explicitly deopted, so ignore the
  // result of this.
  deopted_funcs_.erase(func);

  if (CompiledFunction* compiled = lookupFunc(func)) {
    finalizeFunc(func, *compiled);
    return true;
  }
  return false;
}

bool Context::didCompile(BorrowedRef<PyFunctionObject> func) {
  ThreadedCompileSerialize guard;
  return compiled_funcs_.contains(func);
}

CompiledFunction* Context::lookupFunc(BorrowedRef<PyFunctionObject> func) {
  return lookupCode(func->func_code, func->func_builtins, func->func_globals);
}

const UnorderedSet<BorrowedRef<PyFunctionObject>>& Context::compiledFuncs() {
  return compiled_funcs_;
}

const UnorderedSet<BorrowedRef<PyFunctionObject>>& Context::deoptedFuncs() {
  return deopted_funcs_;
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

void Context::funcModified(BorrowedRef<PyFunctionObject> func) {
  deoptFunc(func);
}

void Context::funcDestroyed(BorrowedRef<PyFunctionObject> func) {
  compiled_funcs_.erase(func);
  deopted_funcs_.erase(func);
}

Context::CompilationResult Context::compilePreloaderImpl(
    const hir::Preloader& preloader) {
  BorrowedRef<PyCodeObject> code = preloader.code();
  if (code == nullptr) {
    JIT_DLOG("Can't compile {} as it has no code object", preloader.fullname());
    return {nullptr, PYJIT_RESULT_CANNOT_SPECIALIZE};
  }

  BorrowedRef<PyDictObject> builtins = preloader.builtins();
  BorrowedRef<PyDictObject> globals = preloader.globals();

  // Don't care flags: CO_NOFREE, CO_FUTURE_* (the only still-relevant future is
  // "annotations" which doesn't impact bytecode execution.)
  int required_flags = CO_OPTIMIZED | CO_NEWLOCALS;
  if ((code->co_flags & required_flags) != required_flags) {
    JIT_DLOG(
        "Can't compile {} due to missing required code flags",
        preloader.fullname());
    return {nullptr, PYJIT_RESULT_CANNOT_SPECIALIZE};
  }
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    JIT_DLOG(
        "Can't compile {} as it has had the JIT suppressed",
        preloader.fullname());
    return {nullptr, PYJIT_RESULT_CANNOT_SPECIALIZE};
  }
  constexpr int forbidden_flags =
      PY_VERSION_HEX >= 0x030C0000 ? CO_ASYNC_GENERATOR : 0;
  if (code->co_flags & forbidden_flags) {
    JIT_DLOG(
        "Cannot JIT compile {} as it has prohibited code flags: 0x{:x}",
        preloader.fullname(),
        code->co_flags & forbidden_flags);
    return {nullptr, PYJIT_RESULT_CANNOT_SPECIALIZE};
  }

  CompilationKey key{code, builtins, globals};
  {
    // Attempt to atomically transition the code from "not compiled" to "in
    // progress".
    ThreadedCompileSerialize guard;
    if (CompiledFunction* compiled = lookupCode(code, builtins, globals)) {
      return {compiled, PYJIT_RESULT_OK};
    }
    if (!active_compiles_.insert(key).second) {
      return {nullptr, PYJIT_RESULT_RETRY};
    }
  }

  std::unique_ptr<CompiledFunction> compiled;
  try {
    compiled = jit_compiler_.Compile(preloader);
  } catch (const std::exception& exn) {
    JIT_DLOG("{}", exn.what());
  }

  ThreadedCompileSerialize guard;
  active_compiles_.erase(key);
  if (compiled == nullptr) {
    return {nullptr, PYJIT_RESULT_UNKNOWN_ERROR};
  }

  register_pycode_debug_symbol(
      code, preloader.fullname().c_str(), compiled.get());

  total_compile_time_ms_.fetch_add(
      compiled->compileTime().count(), std::memory_order_relaxed);

  // Store the compiled code.
  auto pair = compiled_codes_.emplace(key, std::move(compiled));
  JIT_CHECK(pair.second, "CompilationKey already present");
  return {pair.first->second.get(), PYJIT_RESULT_OK};
}

CompiledFunction* Context::lookupCode(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals) {
  ThreadedCompileSerialize guard;
  auto it = compiled_codes_.find(CompilationKey{code, builtins, globals});
  return it == compiled_codes_.end() ? nullptr : it->second.get();
}

void Context::finalizeFunc(
    BorrowedRef<PyFunctionObject> func,
    const CompiledFunction& compiled) {
  ThreadedCompileSerialize guard;
  if (!compiled_funcs_.emplace(func).second) {
    // Someone else compiled the function between when our caller checked and
    // called us.
    return;
  }

  // In case the function had previously been deopted.
  deopted_funcs_.erase(func);

  func->vectorcall = compiled.vectorcallEntry();
  Runtime* rt = Runtime::get();
  if (rt->hasFunctionEntryCache(func)) {
    void** indirect = rt->findFunctionEntryCache(func);
    *indirect = compiled.staticEntry();
  }
  return;
}

bool Context::deoptFuncImpl(BorrowedRef<PyFunctionObject> func) {
  // There appear to be instances where the runtime is finalizing and goes to
  // destroy the cinderjit module and deopt all compiled functions, only to find
  // that some of the compiled functions have already been zeroed out and
  // possibly deallocated.  In theory this should be covered by funcDestroyed()
  // but somewhere that isn't being triggered.  This is not a good solution but
  // it fixes some shutdown crashes for now.
  if (func->func_module == nullptr && func->func_qualname == nullptr) {
    JIT_CHECK(
        Py_IsFinalizing(),
        "Trying to deopt destroyed function at {} when runtime is not "
        "finalizing",
        reinterpret_cast<void*>(func.get()));
    return false;
  }

  if (compiled_funcs_.erase(func) == 0) {
    return false;
  }
  func->vectorcall = getInterpretedVectorcall(func);
  return true;
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

  // TODO(alexanderm): Unmap compiled functions and empty out private data
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

  // TODO(alexanderm): Use std::piecewise_construct for better efficiency.
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

// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/jit_context.h"

#include "cinderx/Common/log.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/jit_gdb_support.h"

#include <unordered_set>

#if PY_VERSION_HEX >= 0x030C0000
#include "cinderx/Upgrade/upgrade_assert.h" // @donotremove
#endif

namespace jit {

AotContext g_aot_ctx;

Context::~Context() {
  for (auto it = compiled_funcs_.begin(); it != compiled_funcs_.end();) {
    PyFunctionObject* func = *it;
    ++it;
    deoptFunc(func);
  }
}

_PyJIT_Result Context::compilePreloader(
    BorrowedRef<PyFunctionObject> func,
    const hir::Preloader& preloader) {
  CompilationResult result = compilePreloader(preloader);
  if (result.compiled == nullptr) {
    return result.result;
  }
  if (func != nullptr) {
    finalizeFunc(func, *result.compiled);
  }
  return PYJIT_RESULT_OK;
}

_PyJIT_Result Context::attachCompiledCode(BorrowedRef<PyFunctionObject> func) {
  JIT_DCHECK(!didCompile(func), "Function is already compiled");

  if (CompiledFunction* compiled = lookupFunc(func)) {
    finalizeFunc(func, *compiled);
    return PYJIT_RESULT_OK;
  }
  return PYJIT_RESULT_CANNOT_SPECIALIZE;
}

void Context::funcModified(BorrowedRef<PyFunctionObject> func) {
  deoptFunc(func);
}

void Context::funcDestroyed(BorrowedRef<PyFunctionObject> func) {
  compiled_funcs_.erase(func);
}

bool Context::didCompile(BorrowedRef<PyFunctionObject> func) {
  ThreadedCompileSerialize guard;
  return compiled_funcs_.count(func) != 0;
}

CompiledFunction* Context::lookupFunc(BorrowedRef<PyFunctionObject> func) {
  return lookupCode(func->func_code, func->func_builtins, func->func_globals);
}

// TODO(T142228417): Deprecate this once callsites have been updated to use
// get_inlined_functions_stats
int Context::numInlinedFunctions(BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* jitfunc = lookupFunc(func);
  return jitfunc != nullptr
      ? jitfunc->inlinedFunctionsStats().num_inlined_functions
      : -1;
}

Ref<> Context::inlinedFunctionsStats(BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* jitfunc = lookupFunc(func);
  if (jitfunc == nullptr) {
    return nullptr;
  }
  auto stats = jitfunc->inlinedFunctionsStats();
  auto py_stats = Ref<>::steal(PyDict_New());
  if (py_stats == nullptr) {
    return nullptr;
  }
  auto num_inlined_functions =
      Ref<>::steal(PyLong_FromSize_t(stats.num_inlined_functions));
  if (num_inlined_functions == nullptr) {
    return nullptr;
  }
  if (PyDict_SetItemString(
          py_stats, "num_inlined_functions", num_inlined_functions) < 0) {
    return nullptr;
  }
  auto failure_stats = Ref<>::steal(PyDict_New());
  if (failure_stats == nullptr) {
    return nullptr;
  }
  for (const auto& [reason, functions] : stats.failure_stats) {
    auto py_failure_reason =
        Ref<>::steal(PyUnicode_InternFromString(getInlineFailureName(reason)));
    if (py_failure_reason == nullptr) {
      return nullptr;
    }
    auto py_functions_set = Ref<>::steal(PySet_New(nullptr));
    if (py_functions_set == nullptr) {
      return nullptr;
    }
    if (PyDict_SetItem(failure_stats, py_failure_reason, py_functions_set) <
        0) {
      return nullptr;
    }
    for (const auto& function : functions) {
      auto py_function = Ref<>::steal(PyUnicode_FromString(function.c_str()));
      if (PySet_Add(py_functions_set, py_function) < 0) {
        return nullptr;
      }
    }
  }
  if (PyDict_SetItemString(py_stats, "failure_stats", failure_stats) < 0) {
    return nullptr;
  }
  return py_stats;
}

const hir::OpcodeCounts* Context::hirOpcodeCounts(
    BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* jit_func = lookupFunc(func);
  return jit_func != nullptr ? &jit_func->hirOpcodeCounts() : nullptr;
}

int Context::printHIR(BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* jit_func = lookupFunc(func);
  if (jit_func == nullptr) {
    return -1;
  }
  jit_func->printHIR();
  return 0;
}

int Context::disassemble(BorrowedRef<PyFunctionObject> func) {
  CompiledFunction* jit_func = lookupFunc(func);
  if (jit_func == nullptr) {
    return -1;
  }
  jit_func->disassemble();
  return 0;
}

const UnorderedSet<BorrowedRef<PyFunctionObject>>& Context::compiledFuncs() {
  return compiled_funcs_;
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

Context::CompilationResult Context::compilePreloader(
    const hir::Preloader& preloader) {
  BorrowedRef<PyCodeObject> code = preloader.code();
  BorrowedRef<PyDictObject> builtins = preloader.builtins();
  BorrowedRef<PyDictObject> globals = preloader.globals();

  int required_flags = CO_OPTIMIZED | CO_NEWLOCALS;
  int prohibited_flags = CI_CO_SUPPRESS_JIT;
  // Don't care flags: CO_NOFREE, CO_FUTURE_* (the only still-relevant future
  // is "annotations" which doesn't impact bytecode execution.)
  if (code == nullptr ||
      ((code->co_flags & required_flags) != required_flags) ||
      (code->co_flags & prohibited_flags) != 0) {
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

void Context::deoptFunc(BorrowedRef<PyFunctionObject> func) {
  // There appear to be instances where the runtime is finalizing and goes to
  // destroy the cinderjit module and deopt all compiled functions, only to find
  // that some of the compiled functions have already been zeroed out and
  // possibly deallocated.  In theory this should be covered by funcDestroyed()
  // but somewhere that isn't being triggered.  This is not a good solution but
  // it fixes some shutdown crashes for now.
  if (func->func_module == nullptr && func->func_qualname == nullptr) {
    JIT_CHECK(
        _Py_IsFinalizing(),
        "Trying to deopt destroyed function at {} when runtime is not "
        "finalizing",
        reinterpret_cast<void*>(func.get()));
    return;
  }

  if (compiled_funcs_.erase(func) != 0) {
    // Reset the entry point.
    func->vectorcall = getInterpretedVectorcall(func);
  }
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

  func->vectorcall = compiled.vectorcallEntry();
  Runtime* rt = Runtime::get();
  if (rt->hasFunctionEntryCache(func)) {
    void** indirect = rt->findFunctionEntryCache(func);
    *indirect = compiled.staticEntry();
  }
  return;
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

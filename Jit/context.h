// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/elf/note.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/pyjit_result.h"

#include <memory>
#include <vector>

namespace jit {

// Lookup key for compiled functions in Context: a code object and the globals
// and builtins dicts it was JIT-compiled with.
struct CompilationKey {
  // These three are borrowed references; the values are kept alive by strong
  // references in the corresponding jit::CodeRuntime.
  PyObject* code;
  PyObject* builtins;
  PyObject* globals;

  CompilationKey(PyObject* code, PyObject* builtins, PyObject* globals)
      : code(code), builtins(builtins), globals(globals) {}

  constexpr bool operator==(const CompilationKey& other) const = default;
};

} // namespace jit

template <>
struct std::hash<jit::CompilationKey> {
  std::size_t operator()(const jit::CompilationKey& key) const {
    std::hash<PyObject*> hasher;
    return jit::combineHash(
        hasher(key.code), hasher(key.globals), hasher(key.builtins));
  }
};

namespace jit {

/*
 * A jit::Context encapsulates all the state managed by an instance of the JIT.
 */
class Context {
 public:
  /*
   * Will deopt all compiled functions back to the interpreter.
   */
  ~Context();

  /*
   * JIT compile function/code-object from a Preloader.
   *
   * Patches func entrypoint if a func is provided.
   *
   * Will return PYJIT_RESULT_OK if the function/code object was already
   * compiled.
   */
  _PyJIT_Result compilePreloader(
      BorrowedRef<PyFunctionObject> func,
      const hir::Preloader& preloader);

  /*
   * De-optimize a function by setting it to run through the interpreter if it
   * had been previously JIT-compiled.
   *
   * Return true if the function was previously JIT-compiled, false otherwise.
   */
  bool deoptFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Re-optimize a function by setting it to use JIT-compiled code if there's a
   * matching compiled code object.
   *
   * Intended for functions that have been explicitly deopted and for nested
   * functions.  Nested functions are created and destroyed multiple times but
   * have the same underlying code object.
   *
   * Return true if the function was successfully reopted, false if nothing
   * happened.
   */
  bool reoptFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Return whether or not this context compiled the supplied function.
   */
  bool didCompile(BorrowedRef<PyFunctionObject> func);

  /*
   * Look up the compiled function object for a given Python function object.
   */
  CompiledFunction* lookupFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Get a range over all function objects that have been compiled.
   */
  const UnorderedSet<BorrowedRef<PyFunctionObject>>& compiledFuncs();

  /*
   * Get a range over all function objects that have been compiled and since
   * deopted.
   */
  const UnorderedSet<BorrowedRef<PyFunctionObject>>& deoptedFuncs();

  /*
   * Set and hold a reference to the cinderjit Python module.
   */
  void setCinderJitModule(Ref<> mod);

  /*
   * Clear cache of compiled code such that subsequent compilations are always
   * full rather than just re-binding pre-compiled code. Only intended to be
   * used during multithreaded_compile_test.
   */
  void clearCache();

  /*
   * Callbacks invoked by the runtime when a PyFunctionObject is modified or
   * destroyed.
   */
  void funcModified(BorrowedRef<PyFunctionObject> func);
  void funcDestroyed(BorrowedRef<PyFunctionObject> func);

 private:
  struct CompilationResult {
    CompiledFunction* compiled;
    _PyJIT_Result result;
  };

  CompilationResult compilePreloader(const hir::Preloader& preloader);

  CompiledFunction* lookupCode(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals);

  /*
   * Record per-function metadata for a newly compiled function and set the
   * function's entrypoint.
   */
  void finalizeFunc(
      BorrowedRef<PyFunctionObject> func,
      const CompiledFunction& compiled);

  /* Deopts a function but doesn't touch deopted_funcs_. */
  bool deoptFuncImpl(BorrowedRef<PyFunctionObject> func);

  /* General purpose jit compiler */
  Compiler jit_compiler_;

  /*
   * Map of all compiled code objects, keyed by their address and also their
   * builtins and globals objects.
   */
  UnorderedMap<CompilationKey, std::unique_ptr<CompiledFunction>>
      compiled_codes_;

  /* Set of which functions have JIT-compiled entrypoints. */
  UnorderedSet<BorrowedRef<PyFunctionObject>> compiled_funcs_;

  /* Set of which functions were JIT-compiled but have since been deopted. */
  UnorderedSet<BorrowedRef<PyFunctionObject>> deopted_funcs_;

  /*
   * Set of compilations that are currently active, across all threads.
   */
  UnorderedSet<CompilationKey> active_compiles_;

  /*
   * Code which is being kept alive in case it was in use when
   * clearCache was called. Only intended to be used during
   * multithreaded_compile_test.
   */
  std::vector<std::unique_ptr<CompiledFunction>> orphaned_compiled_codes_;

  Ref<> cinderjit_module_;
};

/*
 * An AotContext is like the JIT context, but it holds onto state for
 * ahead-of-time compiled functions.
 */
class AotContext {
 public:
  struct FuncState {
    elf::CodeNoteData note;
    BorrowedRef<PyFunctionObject> func;
    std::span<const std::byte> compiled_code;

    vectorcallfunc normalEntry() const {
      return reinterpret_cast<vectorcallfunc>(const_cast<std::byte*>(
          compiled_code.data() + note.normal_entry_offset));
    }
  };

  /*
   * Initialize the context with the handle to the AOT bundle created by
   * dlopen().
   */
  void init(void* bundle_handle);

  /* Clean up the context object. */
  void destroy();

  /*
   * Register a new function whose metadata has been parsed out of the AOT
   * bundle.
   */
  void registerFunc(const elf::Note& note);

  /* Look up the state associated with a given Python function. */
  const FuncState* lookupFuncState(BorrowedRef<PyFunctionObject> func);

 private:
  // The handle to the AOT bundle created by dlopen().
  void* bundle_handle_{nullptr};

  jit::UnorderedMap<std::string, FuncState> funcs_;
};

extern AotContext g_aot_ctx;

} // namespace jit

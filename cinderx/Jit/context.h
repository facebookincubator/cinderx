// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context_iface.h"
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

  explicit CompilationKey(BorrowedRef<PyFunctionObject> func)
      : code{func->func_code},
        builtins{func->func_builtins},
        globals{func->func_globals} {}

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
class Context : public IJitContext {
 public:
  struct CompilationResult {
    CompiledFunction* compiled;
    _PyJIT_Result result;
  };

  /*
   * Adds a function to the list of deopted functions - this means the function
   * was once compiled but has now been turned back into a normal Python
   * function. If the JIT is re-enabled the function can be re-initialized to
   * the JITed form.
   */
  void addDeoptedFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Removes a function from the deopted functions set.
   */
  void removeDeoptedFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Fully remove all effects of compilation from a function.
   */
  void uncompile(BorrowedRef<PyFunctionObject> func);

  /*
   * Adds a record indicating that the specified function is currently being
   * compiled. This is used to prevent multiple threads from compiling the same
   * function at the same time.
   */
  bool addActiveCompile(CompilationKey& key);

  /*
   * Indicates that the specified function is no longer being compiled.
   */
  void removeActiveCompile(CompilationKey& key);

  /*
   * Registers the CompiledFunction object for a given compilation key.
   * The compiled code can then be shared amongst compatible functions.
   */
  CompiledFunction* addCompiledFunction(
      CompilationKey& key,
      std::unique_ptr<CompiledFunction> compiled);

  /*
   * Adds a compiled function to the Context. Returns false if the function was
   * previously added.
   */
  bool addCompiledFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Removes a function from the set of functions that are known to be compiled.
   * This happens if a function is deopted.
   *
   * Returns true if the function was removed.
   */
  bool removeCompiledFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Return whether or not this context compiled the supplied function.
   */
  bool didCompile(BorrowedRef<PyFunctionObject> func);

  /*
   * Remove the specified code object from the known compiled codes.
   */
  void forgetCode(BorrowedRef<PyFunctionObject> func);
  /*
   * Look up the compiled function object for a given Python function object.
   */
  CompiledFunction* lookupFunc(BorrowedRef<PyFunctionObject> func);

  /*
   * Gets the CompiledFunction for a given code/builtins/globals triplet.
   */
  CompiledFunction* lookupCode(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals);

  /*
   * Looks up the CodeRuntime for a given function.
   */
  CodeRuntime* lookupCodeRuntime(BorrowedRef<PyFunctionObject> func);

  /*
   * Get the map of all compiled code objects, keyed by their address and also
   * their builtins and globals objects.
   */
  const UnorderedMap<CompilationKey, std::unique_ptr<CompiledFunction>>&
  compiledCodes() const;

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
   * Get the total time spent compiling functions thus far.
   */
  std::chrono::milliseconds totalCompileTime() const;

  /*
   * Adds time to the record of how much time has been spent compiling
   * functions.
   */
  void addCompileTime(std::chrono::nanoseconds time);
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
   * Callbacks invoked by the runtime when a PyFunctionObject is destroyed.
   */
  void funcDestroyed(BorrowedRef<PyFunctionObject> func);

 private:
  CompilationResult compilePreloaderImpl(const hir::Preloader& preloader);

  /*
   * Record per-function metadata for a newly compiled function and set the
   * function's entrypoint.
   */
  void finalizeFunc(
      BorrowedRef<PyFunctionObject> func,
      const CompiledFunction& compiled);

  /* Deopts a function but doesn't touch deopted_funcs_. */
  bool deoptFuncImpl(BorrowedRef<PyFunctionObject> func);

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

  std::atomic_size_t total_compile_time_ms_;
};

// A CompilerContext is like a Context but it also holds a compiler object
// of the consumers choosing.
template <typename T>
class CompilerContext : public Context {
 public:
  T& compiler() {
    return compiler_;
  }

 private:
  T compiler_;
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

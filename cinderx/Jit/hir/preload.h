// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/hir/annotation_index.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/StaticPython/typed-args-info.h"

#include <map>
#include <unordered_map>
#include <utility>

namespace jit::hir {

using ArgToType = std::map<long, Type>;
using GlobalNamesMap = std::unordered_map<int, BorrowedRef<>>;

struct FieldInfo {
  Py_ssize_t offset;
  Type type;
  BorrowedRef<PyUnicodeObject> name;
};

// The target of an INVOKE_FUNCTION or INVOKE_METHOD
struct InvokeTarget {
  BorrowedRef<PyFunctionObject> func() const;

  // Vector-callable Python object
  Ref<> callable;
  // python-level return type (None for void/error-code builtins)
  Type return_type{TObject};
  // map argnum to primitive type code for primitive args only
  ArgToType primitive_arg_types;
  // container is immutable (target is not patchable)
  bool container_is_immutable{false};
  // patching indirection, nullptr if container_is_immutable
  PyObject** indirect_ptr{nullptr};
  // vtable slot number (LOAD_METHOD_STATIC only)
  Py_ssize_t slot{-1};
  // is a CI_CO_STATICALLY_COMPILED Python function or METH_TYPED builtin
  bool is_statically_typed{false};
  // is PyFunctionObject
  bool is_function{false};
  // is PyMethodDescrObject or PyCFunction (has a PyMethodDef)
  bool is_builtin{false};
  // needs the function object available at runtime (e.g. for freevars)
  bool uses_runtime_func{
#if PY_VERSION_HEX < 0x030C0000
      false
#else
      true
#endif
  };
  // underlying C function implementation for builtins
  void* builtin_c_func{nullptr};
  // expected nargs for builtin; if matched, can x64 invoke even if untyped
  long builtin_expected_nargs{-1};
  // is a METH_TYPED builtin that returns void
  bool builtin_returns_void{false};
  // is a METH_TYPED builtin that returns integer error code
  bool builtin_returns_error_code{false};
};

using InvokeTargetMap =
    std::unordered_map<PyObject*, std::unique_ptr<InvokeTarget>>;

// The target of an INVOKE_NATIVE
struct NativeTarget {
  // the address of target
  void* callable;
  // return type (must be a primitive int for native calls)
  Type return_type{TObject};
  // map argnum to primitive type code for primitive args only
  ArgToType primitive_arg_types;
};

// Preloads all globals and classloader type descrs referenced by a code object.
// We need to do this in advance because it can resolve lazy imports (or
// generally just trigger imports) which is Python code execution, which we
// can't allow mid-compile.
class Preloader {
 public:
  Preloader(Preloader&&) = default;
  Preloader() = default;

  static std::unique_ptr<Preloader> makePreloader(
      BorrowedRef<PyFunctionObject> func,
      Ref<> reifier = nullptr) {
    return makePreloader(
        func->func_code,
        func->func_builtins,
        func->func_globals,
        AnnotationIndex::from_function(func),
        funcFullname(func),
        std::move(reifier));
  }

  static std::unique_ptr<Preloader> makePreloader(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      std::unique_ptr<AnnotationIndex> annotations,
      const std::string& fullname,
      Ref<> reifier = nullptr) {
    auto preloader = std::unique_ptr<Preloader>(new Preloader(
        code,
        builtins,
        globals,
        std::move(annotations),
        fullname,
        std::move(reifier)));
    bool success = preloader->preload();
    JIT_DCHECK(
        success != static_cast<bool>(PyErr_Occurred()),
        "Expecting Python exception only when preloading fails, preloading "
        "result: {}",
        success);
    return success ? std::move(preloader) : nullptr;
  }

  Type type(BorrowedRef<> descr) const;
  int primitiveTypecode(BorrowedRef<> descr) const;
  BorrowedRef<PyTypeObject> pyType(BorrowedRef<> descr) const;
  const OwnedType& preloadedType(BorrowedRef<> descr) const;

  const FieldInfo& fieldInfo(BorrowedRef<> descr) const;

  const InvokeTarget& invokeFunctionTarget(BorrowedRef<> descr) const;
  const InvokeTarget& invokeMethodTarget(BorrowedRef<> descr) const;
  const NativeTarget& invokeNativeTarget(BorrowedRef<> target) const;

  const InvokeTargetMap& invokeFunctionTargets() const {
    return func_targets_;
  }

  const GlobalNamesMap& globalNames() const {
    return global_names_;
  }

  // get the type from argument check info for the given locals index, or
  // TObject
  Type checkArgType(long local_idx) const;

  // get value for global at given name index
  BorrowedRef<> global(int name_idx) const;

  std::unique_ptr<Function> makeFunction() const;

  BorrowedRef<PyCodeObject> code() const {
    return code_;
  }

  BorrowedRef<PyDictObject> globals() const {
    return globals_;
  }

  BorrowedRef<PyDictObject> builtins() const {
    return builtins_;
  }

  AnnotationIndex* annotations() const {
    return annotations_.get();
  }

  const std::string& fullname() const {
    return fullname_;
  }

  Type returnType() const {
    return return_type_;
  }

  int numArgs() const {
    if (code_ == nullptr) {
      // code_ might be null if we parsed from textual ir
      return 0;
    }
    return code_->co_argcount + code_->co_kwonlyargcount +
        bool(code_->co_flags & CO_VARARGS) +
        bool(code_->co_flags & CO_VARKEYWORDS);
  }

  bool hasPrimitiveArgs() const {
    return has_primitive_args_;
  }

  std::unique_ptr<InvokeTarget> resolve_target_descr(
      BorrowedRef<> descr,
      int opcode);

  BorrowedRef<> reifier() const {
    return reifier_;
  }

 private:
  BorrowedRef<> constArg(BytecodeInstruction& bc_instr) const;
  PyObject** getGlobalCache(BorrowedRef<> name) const;
  bool canCacheGlobals() const;
  bool preload();

  // Preload information only relevant to Static Python functions.
  bool preloadStatic();

  // Check if a code object is for the top-level code in a module.
  bool isModuleCodeObject() const;

  explicit Preloader(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      std::unique_ptr<AnnotationIndex> annotations,
      const std::string& fullname,
      Ref<> reifier)
      : code_(Ref<>::create(code)),
        builtins_(Ref<>::create(builtins)),
        globals_(Ref<>::create(globals)),
        annotations_(std::move(annotations)),
        fullname_(fullname),
        reifier_(std::move(reifier)) {
    JIT_CHECK(PyCode_Check(code_), "Expected PyCodeObject");
  }

  Ref<PyCodeObject> code_;
  Ref<PyDictObject> builtins_;
  Ref<PyDictObject> globals_;
  std::unique_ptr<AnnotationIndex> annotations_;
  const std::string fullname_;
  Ref<> reifier_;

  // keyed by type descr tuple identity (they are interned in code objects)
  std::unordered_map<PyObject*, OwnedType> types_;
  std::unordered_map<PyObject*, FieldInfo> fields_;
  InvokeTargetMap func_targets_;
  InvokeTargetMap meth_targets_;
  std::unordered_map<PyObject*, std::unique_ptr<NativeTarget>> native_targets_;
  // keyed by locals index
  std::unordered_map<long, Type> check_arg_types_;
  std::map<long, OwnedType> check_arg_pytypes_;
  // keyed by name index, names borrowed from code object
  GlobalNamesMap global_names_;
  Type return_type_{TObject};
  bool has_primitive_args_{false};
  bool has_primitive_first_arg_{false};
  // for primitive args only, null unless has_primitive_args_
  Ref<_PyTypedArgsInfo> prim_args_info_;
};

using PreloaderMap =
    std::unordered_map<BorrowedRef<PyCodeObject>, std::unique_ptr<Preloader>>;

// Manages a map of code objects to their associated preloaders.
class PreloaderManager {
 public:
  // Add a new code object and preloader pair.  Duplicates are not allowed.
  void add(
      BorrowedRef<PyCodeObject> code,
      std::unique_ptr<Preloader> preloader);

  // Find the preloader for the given code object or function object.
  Preloader* find(BorrowedRef<PyCodeObject> code);
  Preloader* find(BorrowedRef<PyFunctionObject> func);

  // Check if there are any preloaders registered.
  bool empty() const;

  // Clear out all preloaders.
  void clear();

  // Swap the inner map with the one passed as an argument.  Needed by
  // IsolatedPreloaders.
  void swap(PreloaderMap& replacement);

 private:
  PreloaderMap preloaders_;
};

// Get the global PreloaderManager object.
PreloaderManager& preloaderManager();

// RAII device for isolating preloaders state.
class IsolatedPreloaders {
 public:
  IsolatedPreloaders();
  ~IsolatedPreloaders();

 private:
  void swap();

  PreloaderMap orig_preloaders_;
};

} // namespace jit::hir

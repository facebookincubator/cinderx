// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/Common/sorted_vec_map.h"
#include "cinderx/Jit/hir/annotation_index.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/StaticPython/typed-args-info.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace cinderx::jit::hir {

// Maps keyed on local indices or name indices.  Keys are small dense integers,
// so a sorted vector is cheaper than a hash table or tree.
using ArgTypeMap = SortedVecMap<int, Type>;
using GlobalNamesMap = SortedVecMap<int, BorrowedRef<>>;
using GlobalsCacheMap = SortedVecMap<int, PyObject**>;

// A map keyed by type descr tuples.
template <class T>
using DescrMap = std::unordered_map<BorrowedRef<>, T>;

struct FieldInfo {
  Py_ssize_t offset;
  Type type;
  BorrowedRef<PyUnicodeObject> name;
  std::string name_str;
};

// The target of an INVOKE_FUNCTION or INVOKE_METHOD
struct InvokeTarget {
  BorrowedRef<PyFunctionObject> func() const;

  bool isBuiltin() const;
  bool isFunction() const;

  // Vector-callable Python object
  Ref<> callable;
  // python-level return type (None for void/error-code builtins)
  Type return_type{TObject};
  // Strong reference keeping return_type's spec PyTypeObject alive for the
  // lifetime of this target (which spans compilation).  return_type only holds
  // a borrowed reference, so without this the type could be freed by the
  // interpreter mid-compile -- a use-after-free during a background compile.
  OwnedType return_type_owned;
  // map argnum to primitive type code for primitive args only
  ArgTypeMap primitive_arg_types;
  // patching indirection, nullptr if container_is_immutable
  PyObject** indirect_ptr{nullptr};
  // vtable slot number (LOAD_METHOD_STATIC only)
  Py_ssize_t slot{-1};
  // underlying C function implementation for builtins
  void* builtin_c_func{nullptr};
  // expected nargs for builtin; if matched, can x64 invoke even if untyped
  int builtin_expected_nargs{-1};
  // container is immutable (target is not patchable)
  bool container_is_immutable{false};
  // is a CI_CO_STATICALLY_COMPILED Python function or METH_TYPED builtin
  bool is_statically_typed{false};
  // is a METH_TYPED builtin that returns void
  bool builtin_returns_void{false};
  // is a METH_TYPED builtin that returns integer error code
  bool builtin_returns_error_code{false};
};

// The target of an INVOKE_NATIVE
struct NativeTarget {
  // the address of target
  void* callable;
  // return type (must be a primitive int for native calls)
  Type return_type{TObject};
  // map argnum to primitive type code for primitive args only
  ArgTypeMap primitive_arg_types;
};

// Preloads all globals and classloader type descrs referenced by a code object.
// We need to do this in advance because it can resolve lazy imports (or
// generally just trigger imports) which is Python code execution, which we
// can't allow mid-compile.
class Preloader {
 public:
  Preloader(Preloader&&) = default;
  Preloader() = default;

  static std::unique_ptr<Preloader> make(
      BorrowedRef<PyFunctionObject> func,
      Ref<> reifier = nullptr);

  static std::unique_ptr<Preloader> make(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      std::unique_ptr<AnnotationIndex> annotations,
      const std::string& fullname,
      Ref<> reifier = nullptr);

  // Fetch the type represented by a type descr tuple.
  const OwnedType* preloadedType(BorrowedRef<> descr) const;

  const FieldInfo* fieldInfo(BorrowedRef<> descr) const;

  const InvokeTarget& invokeFunctionTarget(BorrowedRef<> descr) const;
  const InvokeTarget& invokeMethodTarget(BorrowedRef<> descr) const;
  const NativeTarget& invokeNativeTarget(BorrowedRef<> target) const;

  // All functions (not methods) invoked by the code object.
  const DescrMap<std::unique_ptr<InvokeTarget>>& invokeFunctionTargets() const;

  // All global names used by the code object.
  const GlobalNamesMap& globalNames() const;

  // Precomputed UTF-8 names from co_names, for use during HIR building without
  // GIL.
  const std::vector<std::string>& names() const;
  const std::string& name(Py_ssize_t idx) const;

  // Get the type from argument check info for the given locals index.  Will
  // return TObject for untyped values.
  Type checkArgType(int local_idx) const;

  // Get the global value at a given name index.
  BorrowedRef<> global(int name_idx) const;

  // Get the global cache at a given name index
  PyObject** globalCache(int name_idx) const;

  std::unique_ptr<Function> makeFunction() const;

  BorrowedRef<PyCodeObject> code() const;
  BorrowedRef<PyDictObject> globals() const;
  BorrowedRef<PyDictObject> builtins() const;

  AnnotationIndex* annotations() const;

  const std::string& fullname() const;

  // Return type of the function.  Object for untyped Python functions, can only
  // be a more specific type for Static Python functions.
  Type returnType() const;

  int numArgs() const;

  bool hasPrimitiveArgs() const;

  BorrowedRef<> reifier() const;

 private:
  explicit Preloader(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      std::unique_ptr<AnnotationIndex> annotations,
      const std::string& fullname,
      Ref<> reifier);

  BorrowedRef<> constArg(BytecodeInstruction& bc_instr) const;
  PyObject** getGlobalCache(BorrowedRef<> name) const;
  bool canCacheGlobals() const;
  bool preload();

  // Preload information only relevant to Static Python functions.
  bool preloadStatic();

  // Check if a code object is for the top-level code in a module.
  bool isModuleCodeObject() const;

  std::unique_ptr<InvokeTarget> resolveTargetDescr(
      BorrowedRef<> descr,
      int opcode);

  Ref<PyCodeObject> code_;
  Ref<PyDictObject> builtins_;
  Ref<PyDictObject> globals_;
  std::unique_ptr<AnnotationIndex> annotations_;
  std::string fullname_;
  Ref<> reifier_;
  std::vector<std::string> names_;

  DescrMap<OwnedType> types_;
  DescrMap<FieldInfo> fields_;
  DescrMap<std::unique_ptr<InvokeTarget>> func_targets_;
  DescrMap<std::unique_ptr<InvokeTarget>> meth_targets_;
  DescrMap<std::unique_ptr<NativeTarget>> native_targets_;

  // Keyed by locals index.
  SortedVecMap<int, OwnedType> check_arg_types_;
  // Keyed by name index, names borrowed from code object.
  GlobalNamesMap global_names_;
  // keyed by name index; stable GlobalCache value slots captured during
  // preload (while the GIL is held).  Preloader::global() reads these slots
  // directly during HIR building rather than re-entering the
  // GlobalCacheManager, which is unsafe with the GIL released in a background
  // compile.
  GlobalsCacheMap global_caches_;
  OwnedType return_type_;
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

  size_t size() const;

  bool isGlobalManager() const;

  // Clear out all preloaders.
  void clear();

 private:
  PreloaderMap preloaders_;
};

// Get the global PreloaderManager object.
PreloaderManager& preloaderManager();

// RAII device for isolating preloaders state.
// Uses thread-local storage to give each thread its own PreloaderManager
// while isolation is active, avoiding race conditions between threads.
class IsolatedPreloaders {
 public:
  IsolatedPreloaders();
  ~IsolatedPreloaders();

 private:
  PreloaderManager local_manager_;
  PreloaderManager* prev_manager_;
};

} // namespace cinderx::jit::hir

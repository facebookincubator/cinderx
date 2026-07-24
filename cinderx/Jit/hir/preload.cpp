// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/preload.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/vtable_builder.h"
#include "cinderx/module_state.h"

#if PY_VERSION_HEX >= 0x030F0000
#include "internal/pycore_lazyimportobject.h" // PyLazyImport_CheckExact
#endif

#include <utility>

namespace cinderx::jit::hir {

namespace {

OwnedType resolve_type_descr(BorrowedRef<> descr) {
  int optional, exact;
  auto type = Ref<PyTypeObject>::steal(
      _PyClassLoader_ResolveType(descr, &optional, &exact));

  return {
      std::move(type), static_cast<bool>(optional), static_cast<bool>(exact)};
}

FieldInfo resolve_field_descr(BorrowedRef<PyTupleObject> descr) {
  int field_type;
  Py_ssize_t offset = _PyClassLoader_ResolveFieldOffset(descr, &field_type);

  JIT_THROW_IF(offset == -1, "Failed to resolve field {}", repr(descr));
  JIT_DCHECK(!PyErr_Occurred(), "shouldn't preload with an error set");

  BorrowedRef<PyUnicodeObject> name_obj{reinterpret_cast<PyUnicodeObject*>(
      PyTuple_GET_ITEM(descr, PyTuple_GET_SIZE(descr) - 1))};
  const char* utf8 = PyUnicode_AsUTF8(name_obj);
  PyErr_Clear();
  std::string name_str = utf8 != nullptr ? utf8 : "";

  return {offset, prim_type_to_type(field_type), name_obj, std::move(name_str)};
}

void _fill_primitive_arg_types_helper(
    BorrowedRef<_PyTypedArgsInfo> prim_args_info,
    ArgTypeMap& map) {
  for (Py_ssize_t i = 0; i < Py_SIZE(prim_args_info.get()); i++) {
    map.emplace(
        prim_args_info->tai_args[i].tai_argnum,
        prim_type_to_type(prim_args_info->tai_args[i].tai_primitive_type));
  }
}

void fill_primitive_arg_types_func(
    BorrowedRef<PyFunctionObject> func,
    ArgTypeMap& map) {
  auto prim_args_info =
      Ref<_PyTypedArgsInfo>::steal(_PyClassLoader_GetTypedArgsInfo(
          reinterpret_cast<PyCodeObject*>(func->func_code), 1));
  _fill_primitive_arg_types_helper(prim_args_info, map);
}

void fill_primitive_arg_types_thunk(
    BorrowedRef<PyObject> thunk,
    ArgTypeMap& map,
    PyObject* container) {
  auto prim_args_info = Ref<_PyTypedArgsInfo>::steal(
      _PyClassLoader_GetTypedArgsInfoFromThunk(thunk, container, 1));

  _fill_primitive_arg_types_helper(prim_args_info, map);
}

void fill_primitive_arg_types_builtin(BorrowedRef<> callable, ArgTypeMap& map) {
  Ci_PyTypedMethodDef* def = _PyClassLoader_GetTypedMethodDef(callable);
  JIT_THROW_IF(
      def == nullptr,
      "Failed to load typed method def from {} object",
      Py_TYPE(callable)->tp_name);
  for (Py_ssize_t i = 0; def->tmd_sig[i] != nullptr; i++) {
    const Ci_Py_SigElement* elem = def->tmd_sig[i];
    int code = Ci_Py_SIG_TYPE_MASK(elem->se_argtype);
    Type typ = prim_type_to_type(code);
    if (typ <= TPrimitive) {
      map.emplace(i, typ);
    }
  }
}

#ifndef WIN32
std::unique_ptr<NativeTarget> resolve_native_target(
    BorrowedRef<> native_descr,
    BorrowedRef<> signature) {
  auto target = std::make_unique<NativeTarget>();
  void* raw_ptr = _PyClassloader_LookupSymbol(
      PyTuple_GET_ITEM(native_descr.get(), 0),
      PyTuple_GET_ITEM(native_descr.get(), 1));

  JIT_THROW_IF(
      raw_ptr == nullptr,
      "Invalid address {} for native function descr {}",
      raw_ptr,
      repr(native_descr));

  target->callable = raw_ptr;

  Py_ssize_t siglen = PyTuple_GET_SIZE(signature.get());
  auto return_type_code = _PyClassLoader_ResolvePrimitiveType(
      PyTuple_GET_ITEM(signature.get(), siglen - 1));
  target->return_type = prim_type_to_type(return_type_code);
  JIT_THROW_IF(
      !(target->return_type <= TCInt),
      "Native function return type must be a primitive int, got {}",
      target->return_type);

  // Fill in the primitive arg type map in the target (index -> Type)
  ArgTypeMap& primitive_arg_types = target->primitive_arg_types;
  for (Py_ssize_t i = 0; i < siglen - 1; i++) {
    int arg_type_code = _PyClassLoader_ResolvePrimitiveType(
        PyTuple_GET_ITEM(signature.get(), i));
    Type typ = prim_type_to_type(arg_type_code);
    JIT_THROW_IF(
        !(typ <= TCInt),
        "Native function argument {} must be a primitive int, got {}",
        i,
        typ);
    primitive_arg_types.emplace(i, typ);
  }

  return target;
}
#endif

PreloaderManager s_manager;
thread_local PreloaderManager* tls_manager = nullptr;

} // namespace

std::unique_ptr<Preloader> Preloader::make(
    BorrowedRef<PyFunctionObject> func,
    Ref<> reifier) {
  return Preloader::make(
      func->func_code,
      func->func_builtins,
      func->func_globals,
      AnnotationIndex::fromFunction(func),
      funcFullname(func),
      std::move(reifier));
}

std::unique_ptr<Preloader> Preloader::make(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals,
    std::unique_ptr<AnnotationIndex> annotations,
    const std::string& fullname,
    Ref<> reifier) {
  auto preloader = std::unique_ptr<Preloader>(new Preloader(
      code,
      builtins,
      globals,
      std::move(annotations),
      fullname,
      std::move(reifier)));
  bool success = preloader->preload();
  JIT_THROW_IF(
      success == static_cast<bool>(PyErr_Occurred()),
      "Expecting Python exception only when preloading fails, preloading "
      "result: {}",
      success);
  return success ? std::move(preloader) : nullptr;
}

BorrowedRef<PyFunctionObject> InvokeTarget::func() const {
  JIT_THROW_IF(!isFunction(), "InvokeTarget is not a PyFunctionObject");
  return reinterpret_cast<PyFunctionObject*>(callable.get());
}

bool InvokeTarget::isBuiltin() const {
  return builtin_c_func != nullptr;
}

bool InvokeTarget::isFunction() const {
  return PyFunction_Check(callable);
}

const OwnedType* Preloader::preloadedType(BorrowedRef<> descr) const {
  auto it = types_.find(descr);
  return it != types_.end() ? &it->second : nullptr;
}

const FieldInfo* Preloader::fieldInfo(BorrowedRef<> descr) const {
  auto it = fields_.find(descr);
  return it != fields_.end() ? &it->second : nullptr;
}

const InvokeTarget& Preloader::invokeFunctionTarget(BorrowedRef<> descr) const {
  return *(map_get(func_targets_, descr));
}

const InvokeTarget& Preloader::invokeMethodTarget(BorrowedRef<> descr) const {
  return *(map_get(meth_targets_, descr));
}

const NativeTarget& Preloader::invokeNativeTarget(BorrowedRef<> target) const {
  return *(map_get(native_targets_, target));
}

const DescrMap<std::unique_ptr<InvokeTarget>>&
Preloader::invokeFunctionTargets() const {
  return func_targets_;
}

const GlobalNamesMap& Preloader::globalNames() const {
  return global_names_;
}

const std::vector<std::string>& Preloader::names() const {
  return names_;
}

const std::string& Preloader::name(Py_ssize_t idx) const {
  return names_[idx];
}

Type Preloader::checkArgType(int local_idx) const {
  auto it = check_arg_types_.find(local_idx);
  return it != check_arg_types_.end() ? it->second.toHir() : TObject;
}

PyObject** Preloader::getGlobalCache(BorrowedRef<> name_obj) const {
  JIT_THROW_IF(
      !canCacheGlobals(),
      "Trying to get a globals cache with unwatchable builtins and/or globals "
      "for {}",
      fullname());
  JIT_THROW_IF(
      !PyUnicode_CheckExact(name_obj),
      "Name must be a str, got {}",
      Py_TYPE(name_obj)->tp_name);
  BorrowedRef<PyUnicodeObject> name{name_obj};
  return cinderx::getModuleState()->cache_manager->getGlobalCache(
      builtins_, globals_, name);
}

bool Preloader::canCacheGlobals() const {
  return hasOnlyUnicodeKeys(builtins_) && hasOnlyUnicodeKeys(globals_);
}

BorrowedRef<> Preloader::global(int name_idx) const {
  auto it = global_caches_.find(name_idx);
  if (it == global_caches_.end()) {
    return nullptr;
  }
  return *it->second;
}

PyObject** Preloader::globalCache(int name_idx) const {
  auto it = global_caches_.find(name_idx);
  if (it == global_caches_.end()) {
    return nullptr;
  }
  return it->second;
}

std::unique_ptr<Function> Preloader::makeFunction() const {
  // We touch refcounts of Python objects here, so must serialize
  ThreadedCompileSerialize guard;
  auto irfunc = std::make_unique<Function>();
  irfunc->fullname = fullname_;
  irfunc->setCode(code_);
  irfunc->builtins.reset(builtins_);
  irfunc->globals.reset(globals_);
  irfunc->prim_args_info.reset(prim_args_info_);
  irfunc->return_type = returnType();
  irfunc->has_primitive_args = hasPrimitiveArgs();
  irfunc->env.reifier = reifier();
  for (auto& [local, preloaded_type] : check_arg_types_) {
    irfunc->typed_args.emplace_back(
        local,
        preloaded_type.type,
        preloaded_type.optional,
        preloaded_type.exact,
        preloaded_type.toHir());
  }
  return irfunc;
}

BorrowedRef<PyCodeObject> Preloader::code() const {
  return code_;
}

BorrowedRef<PyDictObject> Preloader::globals() const {
  return globals_;
}

BorrowedRef<PyDictObject> Preloader::builtins() const {
  return builtins_;
}

AnnotationIndex* Preloader::annotations() const {
  return annotations_.get();
}

const std::string& Preloader::fullname() const {
  return fullname_;
}

Type Preloader::returnType() const {
  return return_type_.type != nullptr ? return_type_.toHir() : TObject;
}

int Preloader::numArgs() const {
  if (code_ == nullptr) {
    // code_ might be null if we parsed from textual ir
    return 0;
  }
  return code_->co_argcount + code_->co_kwonlyargcount +
      bool(code_->co_flags & CO_VARARGS) +
      bool(code_->co_flags & CO_VARKEYWORDS);
}

bool Preloader::hasPrimitiveArgs() const {
  return prim_args_info_ != nullptr;
}

BorrowedRef<> Preloader::reifier() const {
  return reifier_;
}

Preloader::Preloader(
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
      reifier_(std::move(reifier)) {}

BorrowedRef<> Preloader::constArg(BytecodeInstruction& bc_instr) const {
  return PyTuple_GET_ITEM(code_->co_consts, bc_instr.oparg());
}

bool Preloader::preload() {
  // Precompute UTF-8 names from co_names for use during HIR building without
  // GIL.
  PyObject* names_tuple = code_->co_names;
  Py_ssize_t num_names = PyTuple_GET_SIZE(names_tuple);
  names_.reserve(num_names);
  JIT_DCHECK(!PyErr_Occurred(), "shouldn't preload with an error set");
  for (Py_ssize_t i = 0; i < num_names; i++) {
    const char* utf8 = PyUnicode_AsUTF8(PyTuple_GET_ITEM(names_tuple, i));
    names_.emplace_back(utf8 != nullptr ? utf8 : "");
    PyErr_Clear();
  }

  bool is_static = code_->co_flags & CI_CO_STATICALLY_COMPILED;
  if (is_static && !preloadStatic()) {
    return false;
  }

  jit::BytecodeInstructionBlock bc_instrs{code_};
  for (auto bc_instr : bc_instrs) {
    switch (bc_instr.opcode()) {
      case LOAD_GLOBAL: {
        if (!canCacheGlobals()) {
          break;
        }
        PyObject* names = code_->co_names;
        Py_ssize_t names_len = PyTuple_Size(names);
        int name_idx = loadGlobalIndex(bc_instr.oparg());
        JIT_THROW_IF(
            name_idx >= names_len,
            "Preloaded LOAD_GLOBAL with index {} for names tuple of length {}",
            name_idx,
            names_len);

        BorrowedRef<> name = PyTuple_GET_ITEM(names, name_idx);
        JIT_THROW_IF(name == nullptr, "Name cannot be null");
        // Make sure the cached value has been loaded and any side effects of
        // loading it (e.g. lazy imports) have been exercised before we create
        // the GlobalCache; otherwise GlobalCache initialization can
        // self-destroy due to side effects of PyDict_GetItem and cause a
        // use-after-free.
        PyObject* global_value = PyDict_GetItemWithError(globals_, name);
        if (!global_value && !PyErr_Occurred()) {
          // It's extremely unlikely that builtins dict could ever contain a
          // lazy import that needs warming up, but since it is technically
          // possible, we may as well go ahead and warm that up too if the key
          // isn't in globals.
          global_value = PyDict_GetItemWithError(builtins_, name);
        }
        if (PyErr_Occurred()) {
          return false;
        }
#if PY_VERSION_HEX >= 0x030F0000
        // In 3.15+ a name bound by a lazy import is stored in the namespace
        // dict as an unresolved placeholder that LOAD_GLOBAL resolves on
        // access. Don't cache it, otherwise the GlobalCache would hand JIT'd
        // code the placeholder instead of the imported value.
        if (global_value != nullptr && PyLazyImport_CheckExact(global_value)) {
          break;
        }
#endif
        // The above dict fetches may have had side effects that mean globals
        // are no longer cacheable, so recheck that.
        if (canCacheGlobals()) {
          // Eagerly load the global cache, we can't re-enter the global cache
          // manager on the background thread because it can call PyDict_Watch
          if (PyObject** cache = getGlobalCache(name)) {
            global_caches_.emplace(name_idx, cache);
            global_names_.emplace(name_idx, name);
          }
        }
        break;
      }
      case BUILD_CHECKED_LIST:
      case BUILD_CHECKED_MAP: {
        BorrowedRef<> descr = PyTuple_GetItem(constArg(bc_instr), 0);
        OwnedType collection_type = resolve_type_descr(descr);
        JIT_THROW_IF(
            collection_type.type == nullptr,
            "Unknown collection type descr {} during preloading of {}",
            repr(descr),
            fullname());
        types_.emplace(descr, std::move(collection_type));
        break;
      }
      case CAST:
      case LOAD_CLASS:
      case REFINE_TYPE:
      case TP_ALLOC: {
        BorrowedRef<> descr = constArg(bc_instr);
        OwnedType alloc_type = resolve_type_descr(descr);
        JIT_THROW_IF(
            alloc_type.type == nullptr,
            "Unknown {} type descr {} during preloading of {}",
            bc_instr.opcode(),
            repr(descr),
            fullname());
        types_.emplace(descr, std::move(alloc_type));
        break;
      }
      case LOAD_FIELD:
      case STORE_FIELD: {
        BorrowedRef<PyTupleObject> descr(constArg(bc_instr));
        fields_.emplace(descr, resolve_field_descr(descr));
        break;
      }
      case LOAD_METHOD_STATIC:
      case INVOKE_FUNCTION:
      case INVOKE_METHOD: {
        BorrowedRef<> descr = PyTuple_GetItem(constArg(bc_instr), 0);
        auto& map = bc_instr.opcode() == INVOKE_FUNCTION ? func_targets_
                                                         : meth_targets_;
        std::unique_ptr<InvokeTarget> target =
            resolveTargetDescr(descr, bc_instr.opcode());
        if (target) {
          map.emplace(descr, std::move(target));
          break;
        } else {
          return false;
        }
      }
#ifndef WIN32
      case INVOKE_NATIVE: {
        BorrowedRef<> target_descr = PyTuple_GetItem(constArg(bc_instr), 0);
        BorrowedRef<> signature = PyTuple_GetItem(constArg(bc_instr), 1);
        native_targets_.emplace(
            target_descr, resolve_native_target(target_descr, signature));
        break;
      }
#endif
    }
  }

  return true;
}

bool Preloader::preloadStatic() {
  BorrowedRef<> ret_type_descr = _PyClassLoader_GetCodeReturnTypeDescr(code_);
  if (ret_type_descr == nullptr) {
    // Special case where a module's code object is being preloaded.  It will
    // not have argument or return type descrs (and they cannot be added as they
    // interfere with the "<import-from>" list!).
    if (isModuleCodeObject()) {
      return true;
    }

    JIT_THROW(
        "Statically typed function {} has no return type descr, co_consts "
        "is {}",
        fullname(),
        repr(code_->co_consts));
  }

  OwnedType ret_type = resolve_type_descr(ret_type_descr);
  JIT_THROW_IF(
      ret_type.type == nullptr,
      "Unknown return type descr {} during preloading of {}",
      repr(ret_type_descr),
      fullname());

  return_type_ = std::move(ret_type);

  BorrowedRef<PyTupleObject> checks = reinterpret_cast<PyTupleObject*>(
      _PyClassLoader_GetCodeArgumentTypeDescrs(code_));

  bool has_primitive_args = false;
  constexpr Py_ssize_t kMaxLocals = 16384;
  for (int i = 0; i < PyTuple_GET_SIZE(checks); i += 2) {
    Py_ssize_t local = PyLong_AsSsize_t(PyTuple_GET_ITEM(checks, i));
    JIT_THROW_IF(
        local < 0 || local >= kMaxLocals,
        "In Static Python function {}, hit bad local {} at index {}, "
        "arguments checks tuple is {}",
        fullname(),
        local,
        i,
        repr(checks));
    OwnedType preloaded_type =
        resolve_type_descr(PyTuple_GET_ITEM(checks, i + 1));
    JIT_THROW_IF(
        preloaded_type.type == nullptr,
        "Unknown type descr {} during preloading of {}",
        repr(PyTuple_GET_ITEM(checks, i + 1)),
        fullname());
    JIT_THROW_IF(
        preloaded_type.type == reinterpret_cast<PyTypeObject*>(&PyObject_Type),
        "Shouldn't generate type checks for object type, in {} for local {} at "
        "index {}",
        fullname(),
        local,
        i);
    Type type = preloaded_type.toHir();
    check_arg_types_.emplace(local, std::move(preloaded_type));
    if (type <= TPrimitive) {
      has_primitive_args = true;
    }
  }

  if (has_primitive_args) {
    prim_args_info_ = Ref<_PyTypedArgsInfo>::steal(
        _PyClassLoader_GetTypedArgsInfo(code_, true));
  }

  return true;
}

// Check if a code object is for the top-level code in a module.
bool Preloader::isModuleCodeObject() const {
  return fullname().ends_with("<module>") || fullname() == "__main__:__main__";
}

std::unique_ptr<InvokeTarget> Preloader::resolveTargetDescr(
    BorrowedRef<> descr,
    int opcode) {
  auto target = std::make_unique<InvokeTarget>();
  PyObject* container;
  auto callable =
      Ref<>::steal(_PyClassLoader_ResolveFunction(descr, &container));
  JIT_THROW_IF(
      callable == nullptr,
      "Unknown invoke target {} during preloading {}",
      repr(descr),
      fullname());

  int optional, exact, func_flags;
  auto return_pytype =
      Ref<PyTypeObject>::steal(_PyClassLoader_ResolveReturnType(
          callable, &optional, &exact, &func_flags));

  target->container_is_immutable = _PyClassLoader_IsImmutable(container);
  if (return_pytype != nullptr) {
    if (func_flags & Ci_FUNC_FLAGS_COROUTINE) {
      // TODO properly handle coroutine returns awaitable type
      target->return_type = TObject;
    } else {
      target->return_type_owned = OwnedType{
          std::move(return_pytype),
          static_cast<bool>(optional),
          static_cast<bool>(exact)};
      target->return_type = target->return_type_owned.toHir();
    }
  }
  target->is_statically_typed = _PyClassLoader_IsStaticCallable(callable);
  PyMethodDef* def;
  Ci_PyTypedMethodDef* tmd;
  bool is_thunk = false;
  if (_PyClassLoader_IsPatchedThunk(callable)) {
    is_thunk = true;
  } else if ((def = _PyClassLoader_GetMethodDef(callable)) != nullptr) {
    target->builtin_c_func = reinterpret_cast<void*>(def->ml_meth);
    if (def->ml_flags == METH_NOARGS) {
      target->builtin_expected_nargs = 1;
    } else if (def->ml_flags == METH_O) {
      target->builtin_expected_nargs = 2;
    } else if ((tmd = _PyClassLoader_GetTypedMethodDef(callable))) {
      target->builtin_returns_error_code = (tmd->tmd_ret == Ci_Py_SIG_ERROR);
      target->builtin_returns_void = (tmd->tmd_ret == Ci_Py_SIG_VOID);
      target->builtin_c_func = tmd->tmd_meth;
    }
  }
  target->callable = std::move(callable);

  if (opcode == LOAD_METHOD_STATIC) {
    target->slot = _PyClassLoader_ResolveMethod(descr);
    JIT_THROW_IF(
        target->slot == -1,
        "Method lookup failed for descr {} in function {}",
        repr(descr),
        fullname());
  } else { // the rest of this only used by INVOKE_FUNCTION currently
    if (!target->container_is_immutable) {
      target->indirect_ptr = _PyClassLoader_ResolveIndirectPtr(descr);
      JIT_THROW_IF(
          target->indirect_ptr == nullptr,
          "Indirect ptr null for {} in {} (stale bytecode?)",
          repr(descr),
          fullname());
    }
  }

  if (target->is_statically_typed) {
    if (target->isFunction()) {
      fill_primitive_arg_types_func(
          target->func(), target->primitive_arg_types);
    } else {
      fill_primitive_arg_types_builtin(
          target->callable, target->primitive_arg_types);
    }
  }

  if (is_thunk) {
    fill_primitive_arg_types_thunk(
        target->callable.get(), target->primitive_arg_types, container);
  }
  return target;
}

void PreloaderManager::add(
    BorrowedRef<PyCodeObject> code,
    std::unique_ptr<Preloader> preloader) {
  auto [_, inserted] = preloaders_.emplace(code, std::move(preloader));
  JIT_CHECK(
      inserted,
      "Trying to create a duplicate preloader for {}",
      PyUnicode_AsUTF8(code->co_qualname));
}

Preloader* PreloaderManager::find(BorrowedRef<PyCodeObject> code) {
  auto it = preloaders_.find(code);
  return it != preloaders_.end() ? it->second.get() : nullptr;
}

Preloader* PreloaderManager::find(BorrowedRef<PyFunctionObject> func) {
  BorrowedRef<PyCodeObject> code = func->func_code;
  return find(code);
}

bool PreloaderManager::empty() const {
  return preloaders_.empty();
}

size_t PreloaderManager::size() const {
  return preloaders_.size();
}

void PreloaderManager::clear() {
  preloaders_.clear();
}

bool PreloaderManager::isGlobalManager() const {
  return tls_manager == nullptr;
}

PreloaderManager& preloaderManager() {
  if (tls_manager != nullptr) {
    return *tls_manager;
  }
  return s_manager;
}

IsolatedPreloaders::IsolatedPreloaders() : prev_manager_(tls_manager) {
  tls_manager = &local_manager_;
}

IsolatedPreloaders::~IsolatedPreloaders() {
  tls_manager = prev_manager_;
}

} // namespace cinderx::jit::hir

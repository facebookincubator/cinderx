// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/preload.h"

#include "internal/pycore_runtime.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/bytecode.h"
#include "cinderx/Jit/compilation_lock.h"
#include "cinderx/Jit/eligibility.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/vtable_builder.h"
#include "cinderx/module_state.h"

#if PY_VERSION_HEX >= 0x030F0000
#include "internal/pycore_lazyimportobject.h" // PyLazyImport_CheckExact
#endif

#include <optional>
#include <utility>
#include <vector>

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
  const char* symbol =
      PyUnicode_AsUTF8(PyTuple_GET_ITEM(native_descr.get(), 1));
  PyErr_Clear();
  target->name = symbol != nullptr ? symbol : "";

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

thread_local PreloaderManager* tls_manager = nullptr;

// 3.15 packs the lazy-import flags into the low two bits of the IMPORT_NAME
// oparg and the name index into the rest.  CPython decodes this inline in
// bytecodes.c and exports no names for it.
constexpr int kImportEagerFlag = 0x02;
constexpr int kImportNameIdxShift = 2;

bool isExactInternedUnicode(BorrowedRef<> obj) {
  return PyUnicode_CheckExact(obj) && PyUnicode_CHECK_INTERNED(obj.get());
}

bool isCacheableDict(BorrowedRef<PyDictObject> dict) {
  if (!PyDict_CheckExact(dict)) {
    return false;
  }
  CriticalSectionGuard guard(dict);
  return hasOnlyUnicodeKeys(dict);
}

bool isInitializedModule(BorrowedRef<> module) {
  if (!PyModule_CheckExact(module)) {
    return false;
  }
  BorrowedRef<PyDictObject> dict{PyModule_GetDict(module)};
  BorrowedRef<> spec{PyDict_GetItem(dict, &_Py_ID(__spec__))};
  if (spec == nullptr) {
    return true;
  }
  Ref<> initializing{
      Ref<>::steal(PyObject_GetAttr(spec, &_Py_ID(_initializing)))};
  if (initializing == nullptr) {
    bool attr_error = PyErr_ExceptionMatches(PyExc_AttributeError);
    PyErr_Clear();
    return attr_error;
  }
  int is_initializing = PyObject_IsTrue(initializing);
  if (is_initializing < 0) {
    PyErr_Clear();
    return false;
  }
  return is_initializing == 0;
}

int importNameIndex(const BytecodeInstruction& instr) {
  if constexpr (PY_VERSION_HEX >= 0x030F0000) {
    return instr.oparg() >> kImportNameIdxShift;
  }
  return instr.oparg();
}

bool isEagerImport(const BytecodeInstruction& instr, bool is_module_code) {
  if (instr.opcode() == EAGER_IMPORT_NAME) {
    return true;
  }
  if (instr.opcode() != IMPORT_NAME) {
    return false;
  }
  if constexpr (PY_VERSION_HEX >= 0x030F0000) {
    return instr.oparg() & kImportEagerFlag;
  }
  if (!is_module_code) {
    return true;
  }
#ifdef ENABLE_LAZY_IMPORTS
  return !PyImport_IsLazyImportsEnabled();
#else
  return true;
#endif
}

// Name of the module an import expression evaluates to.  `from a.b import c`
// yields the module it names, but a bare `import a.b` binds the top-level
// package `a`.  Returns nullptr on failure.
Ref<PyUnicodeObject> returnedModuleName(
    BorrowedRef<PyUnicodeObject> full_name,
    bool has_from_members) {
  if (has_from_members) {
    return Ref<PyUnicodeObject>::create(full_name);
  }
  Py_ssize_t dot =
      PyUnicode_FindChar(full_name, '.', 0, PyUnicode_GET_LENGTH(full_name), 1);
  if (dot == -2) {
    PyErr_Clear();
    return nullptr;
  }
  if (dot < 0) {
    return Ref<PyUnicodeObject>::create(full_name);
  }
  Ref<> prefix = Ref<>::steal(PyUnicode_Substring(full_name, 0, dot));
  if (prefix == nullptr) {
    return nullptr;
  }
  PyObject* raw_prefix = prefix.release();
  PyUnicode_InternInPlace(&raw_prefix);
  return Ref<PyUnicodeObject>::steal(raw_prefix);
}

PyObject** dictCache(
    BorrowedRef<PyDictObject> dict,
    BorrowedRef<PyUnicodeObject> name) {
  auto* state = cinderx::getModuleState();
  if (state == nullptr || state->cache_manager == nullptr) {
    return nullptr;
  }
  return state->cache_manager->getGlobalCache(dict, dict, name);
}

bool importInfoStillValid(const PreloadedImport& info) {
  if (!info.full_module.isValid() || !info.returned_module.isValid() ||
      !info.import_func.isValid() || !isCacheableDict(info.modules) ||
      !isCacheableDict(info.source_module_dict)) {
    return false;
  }
  for (const auto& member : info.from_members) {
    if (!member.value.isValid()) {
      return false;
    }
  }
  return true;
}

std::optional<PreloadedImport> makePreloadedImport(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    const BytecodeInstruction& instr,
    BorrowedRef<> fromlist,
    bool is_module_code) {
  if (!isEagerImport(instr, is_module_code) ||
      (fromlist != Py_None && !PyTuple_CheckExact(fromlist))) {
    return std::nullopt;
  }

  int name_idx = importNameIndex(instr);
  BorrowedRef<PyTupleObject> names{code->co_names};
  if (name_idx < 0 || name_idx >= PyTuple_GET_SIZE(names)) {
    return std::nullopt;
  }
  BorrowedRef<PyUnicodeObject> full_name{PyTuple_GET_ITEM(names, name_idx)};
  if (!isExactInternedUnicode(full_name)) {
    return std::nullopt;
  }

  Py_ssize_t num_from_members =
      fromlist == Py_None ? 0 : PyTuple_GET_SIZE(fromlist);
  for (Py_ssize_t i = 0; i < num_from_members; i++) {
    BorrowedRef<> member_name{PyTuple_GET_ITEM(fromlist.get(), i)};
    if (!isExactInternedUnicode(member_name) ||
        PyUnicode_CompareWithASCIIString(member_name, "*") == 0) {
      return std::nullopt;
    }
  }

  PyInterpreterState* interp = PyInterpreterState_Get();
  BorrowedRef<PyDictObject> interpreter_builtins{interp->builtins};
  if (builtins != interpreter_builtins || !isCacheableDict(builtins)) {
    return std::nullopt;
  }
  BorrowedRef<> canonical_import{CI_INTERP_IMPORT_FIELD(interp, import_func)};
  Ref<PyUnicodeObject> import_name =
      Ref<PyUnicodeObject>::steal(PyUnicode_InternFromString("__import__"));
  if (canonical_import == nullptr || import_name == nullptr ||
      PyDict_GetItemWithError(builtins, import_name) != canonical_import) {
    PyErr_Clear();
    return std::nullopt;
  }

  BorrowedRef<PyDictObject> modules{CI_INTERP_IMPORT_FIELD(interp, modules)};
  if (!isCacheableDict(modules)) {
    return std::nullopt;
  }
  BorrowedRef<> full_module{PyDict_GetItemWithError(modules, full_name)};
  if (full_module == nullptr || !isInitializedModule(full_module)) {
    PyErr_Clear();
    return std::nullopt;
  }

  Ref<PyUnicodeObject> returned_name =
      returnedModuleName(full_name, num_from_members != 0);
  if (returned_name == nullptr) {
    PyErr_Clear();
    return std::nullopt;
  }
  BorrowedRef<> returned_module{
      PyDict_GetItemWithError(modules, returned_name)};
  if (returned_module == nullptr || !isInitializedModule(returned_module)) {
    PyErr_Clear();
    return std::nullopt;
  }

  auto* state = cinderx::getModuleState();
  if (state != nullptr &&
      (full_module == state->cinderx_module ||
       returned_module == state->cinderx_module)) {
    // Pinning _cinderx in code owned by its JIT Context creates a cycle that
    // can re-enter Context teardown when compiled references are released.
    return std::nullopt;
  }

  BorrowedRef<PyDictObject> source_module_dict{PyModule_GetDict(full_module)};
  if (!isCacheableDict(source_module_dict)) {
    return std::nullopt;
  }

  PyObject** full_module_cache = dictCache(modules, full_name);
  PyObject** returned_module_cache = dictCache(modules, returned_name);
  PyObject** import_func_cache = dictCache(builtins, import_name);
  if (full_module_cache == nullptr || returned_module_cache == nullptr ||
      import_func_cache == nullptr) {
    return std::nullopt;
  }

  PreloadedImport info{
      .opcode = instr.opcode(),
      .name_idx = name_idx,
      .full_name = Ref<PyUnicodeObject>::create(full_name),
      .returned_name = std::move(returned_name),
      .modules = Ref<PyDictObject>::create(modules),
      .full_module =
          {.cache = full_module_cache, .expected = Ref<>::create(full_module)},
      .returned_module =
          {.cache = returned_module_cache,
           .expected = Ref<>::create(returned_module)},
      .import_func =
          {.cache = import_func_cache,
           .expected = Ref<>::create(canonical_import)},
      .source_module_dict = Ref<PyDictObject>::create(source_module_dict),
  };

  info.from_members.reserve(num_from_members);
  for (Py_ssize_t i = 0; i < num_from_members; i++) {
    BorrowedRef<PyUnicodeObject> member_name{
        PyTuple_GET_ITEM(fromlist.get(), i)};
    Py_ssize_t member_name_idx = -1;
    for (Py_ssize_t j = 0; j < PyTuple_GET_SIZE(names); j++) {
      if (PyTuple_GET_ITEM(names, j) == member_name) {
        member_name_idx = j;
        break;
      }
    }
    if (member_name_idx < 0) {
      return std::nullopt;
    }
    BorrowedRef<> member_value{
        PyDict_GetItemWithError(source_module_dict, member_name)};
    if (member_value == nullptr) {
      PyErr_Clear();
      return std::nullopt;
    }
#if PY_VERSION_HEX >= 0x030F0000
    if (PyLazyImport_CheckExact(member_value)) {
      return std::nullopt;
    }
#endif
    PyObject** member_cache = dictCache(source_module_dict, member_name);
    if (member_cache == nullptr) {
      return std::nullopt;
    }
    info.from_members.emplace_back(
        PreloadedImportMember{
            .name_idx = member_name_idx,
            .name = Ref<PyUnicodeObject>::create(member_name),
            .value =
                {.cache = member_cache,
                 .expected = Ref<>::create(member_value)},
        });
  }
  return info;
}

} // namespace

namespace {

void registerNestedCompileData(
    BorrowedRef<> module,
    BorrowedRef<PyCodeObject> root_code,
    bool register_root) {
  FreeThreadedJITEntrypointGuard guard;
  auto* state = cinderx::getModuleState();
  if (state == nullptr || state->jit_context == nullptr) {
    return;
  }

  if (register_root) {
    JitEligibility eligibility = getCompilationEligibility(module, root_code);
    if (eligibility == JitEligibility::Ineligible) {
      return;
    }
    state->jit_context->getOrCreateNestedCompileData(
        module, root_code, eligibility);
  }
  BorrowedRef<PyTupleObject> consts{root_code->co_consts};
  for (Py_ssize_t i = 0, size = PyTuple_GET_SIZE(consts); i < size; i++) {
    BorrowedRef<PyCodeObject> code = PyTuple_GET_ITEM(consts, i);
    if (!PyCode_Check(code) || code->co_qualname == nullptr) {
      continue;
    }
    JitEligibility eligibility = getCompilationEligibility(module, code);
    if (eligibility != JitEligibility::Ineligible) {
      state->jit_context->getOrCreateNestedCompileData(
          module, code, eligibility);
    }
  }
}

} // namespace

std::unique_ptr<Preloader> Preloader::make(
    BorrowedRef<PyFunctionObject> func,
    Ref<> reifier) {
  // Snapshot inputs before annotation evaluation runs arbitrary Python.
  auto owned_func = Ref<PyFunctionObject>::create(func);
  auto code = Ref<PyCodeObject>::create(func->func_code);
  auto builtins = Ref<PyDictObject>::create(func->func_builtins);
  auto globals = Ref<PyDictObject>::create(func->func_globals);
  auto module = Ref<>::create(func->func_module);
  auto fullname = funcFullname(func);
  auto annotations = AnnotationIndex::fromFunction(func);
  if (PyErr_Occurred()) {
    return nullptr;
  }
  auto preloader = Preloader::makeImpl(
      std::move(code),
      std::move(builtins),
      std::move(globals),
      module,
      std::move(annotations),
      fullname,
      std::move(reifier),
      false);
  // Capture after makeImpl: preloading can execute arbitrary Python, which
  // may reassign __defaults__ in the meantime. Strong reference keeps the
  // tuple alive for background-compile reads that cannot safely touch the
  // function object without the GIL.
  if (preloader != nullptr && func->func_defaults != nullptr) {
    preloader->func_defaults_ = Ref<PyTupleObject>::create(func->func_defaults);
  }
  return preloader;
}

std::unique_ptr<Preloader> Preloader::make(
    Ref<PyCodeObject> code,
    Ref<PyDictObject> builtins,
    Ref<PyDictObject> globals,
    BorrowedRef<> module,
    std::unique_ptr<AnnotationIndex> annotations,
    const std::string& fullname,
    Ref<> reifier) {
  return makeImpl(
      Ref<>::create(code),
      Ref<>::create(builtins),
      Ref<>::create(globals),
      module,
      std::move(annotations),
      fullname,
      std::move(reifier),
      true);
}

std::unique_ptr<Preloader> Preloader::makeImpl(
    Ref<PyCodeObject> code,
    Ref<PyDictObject> builtins,
    Ref<PyDictObject> globals,
    BorrowedRef<> module,
    std::unique_ptr<AnnotationIndex> annotations,
    const std::string& fullname,
    Ref<> reifier,
    bool register_code) {
  auto preloader = std::unique_ptr<Preloader>(new Preloader(
      std::move(code),
      std::move(builtins),
      std::move(globals),
      std::move(annotations),
      fullname,
      std::move(reifier)));
  bool success = preloader->preload(module, register_code);
  JIT_THROW_IF(
      success == static_cast<bool>(PyErr_Occurred()),
      "Expecting Python exception only when preloading fails, preloading "
      "result: {}",
      success);
  return success ? std::move(preloader) : nullptr;
}

bool CachedValue::isValid() const {
  BorrowedRef<> current =
      cache == nullptr ? nullptr : ftAtomicLoadPtrAcquire(*cache);
  return current == expected;
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
  auto target_iter = func_targets_.find(descr);
  JIT_THROW_IF(
      target_iter == func_targets_.end(),
      "Failed to find invoke function target {} in {}",
      repr(descr),
      fullname_);
  return *target_iter->second;
}

const InvokeTarget& Preloader::invokeMethodTarget(BorrowedRef<> descr) const {
  auto target_iter = meth_targets_.find(descr);
  JIT_THROW_IF(
      target_iter == meth_targets_.end(),
      "Failed to find invoke method target {} in {}",
      repr(descr),
      fullname_);
  return *target_iter->second;
}

const NativeTarget& Preloader::invokeNativeTarget(BorrowedRef<> descr) const {
  auto target_iter = native_targets_.find(descr);
  JIT_THROW_IF(
      target_iter == native_targets_.end(),
      "Failed to find invoke native target {} in {}",
      repr(descr),
      fullname_);
  return *target_iter->second;
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

const ArgTypeMap& Preloader::primitiveLocalTypes() const {
  return primitive_local_types_;
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
  {
    CriticalSectionGuard guard(builtins_);
    if (!hasOnlyUnicodeKeys(builtins_)) {
      return false;
    }
  }
  CriticalSectionGuard guard(globals_);
  return hasOnlyUnicodeKeys(globals_);
}

BorrowedRef<> Preloader::global(int name_idx) const {
  auto it = global_values_.find(name_idx);
  if (it == global_values_.end()) {
    return nullptr;
  }
  return it->second.get();
}

PyObject** Preloader::globalCache(int name_idx) const {
  auto it = global_caches_.find(name_idx);
  if (it == global_caches_.end()) {
    return nullptr;
  }
  return it->second;
}

const PreloadedImport* Preloader::preloadedImport(BCOffset bc_off) const {
  auto it = preloaded_imports_.find(bc_off);
  return it != preloaded_imports_.end() ? &it->second : nullptr;
}

std::unique_ptr<Function> Preloader::makeFunction() const {
  // We touch refcounts of Python objects here, so must serialize
  auto irfunc = std::make_unique<Function>();
  irfunc->fullname = fullname_;
  irfunc->setCode(code_);
  irfunc->builtins = builtins_;
  irfunc->globals = globals_;
  irfunc->prim_args_info = prim_args_info_;
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

BorrowedRef<PyTupleObject> Preloader::funcDefaults() const {
  return func_defaults_;
}

Preloader::Preloader(
    Ref<PyCodeObject> code,
    Ref<PyDictObject> builtins,
    Ref<PyDictObject> globals,
    std::unique_ptr<AnnotationIndex> annotations,
    const std::string& fullname,
    Ref<> reifier)
    : code_(std::move(code)),
      builtins_(std::move(builtins)),
      globals_(std::move(globals)),
      annotations_(std::move(annotations)),
      fullname_(fullname),
      reifier_(std::move(reifier)) {}

BorrowedRef<> Preloader::constArg(BytecodeInstruction& bc_instr) const {
  return PyTuple_GET_ITEM(code_->co_consts, bc_instr.oparg());
}

bool Preloader::preload(BorrowedRef<> module, bool register_code) {
  registerNestedCompileData(module, code_, register_code);

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

  BorrowedRef<> names = code_->co_names;
  auto get_global = [&](int name_idx) -> Ref<> {
    BorrowedRef<> name = PyTuple_GET_ITEM(names.get(), name_idx);
    JIT_THROW_IF(name == nullptr, "Name cannot be null");
    Ref<> global_value = getDictRef(globals_, name);
    if (!global_value && !PyErr_Occurred()) {
      // It's extremely unlikely that builtins dict could ever contain a
      // lazy import that needs warming up, but since it is technically
      // possible, we may as well go ahead and warm that up too if the key
      // isn't in globals.
      global_value = getDictRef(builtins_, name);
    }
    return global_value;
  };
  jit::BytecodeInstructionBlock bc_instrs{code_};
  std::optional<BytecodeInstruction> previous_instr;
  std::optional<BytecodeInstruction> previous_previous_instr;
  std::vector<std::pair<BCOffset, PreloadedImport>> import_candidates;
  for (auto bc_instr : bc_instrs) {
    switch (bc_instr.opcode()) {
      case LOAD_GLOBAL: {
        if (!canCacheGlobals()) {
          break;
        }

        Py_ssize_t names_len = PyTuple_Size(names);
        int name_idx = loadGlobalIndex(bc_instr.oparg());
        JIT_THROW_IF(
            name_idx >= names_len,
            "Preloaded LOAD_GLOBAL with index {} for names tuple of length {}",
            name_idx,
            names_len);

        // Make sure the cached value has been loaded and any side effects of
        // loading it (e.g. lazy imports) have been exercised before we create
        // the GlobalCache; otherwise GlobalCache initialization can
        // self-destroy due to side effects of PyDict_GetItem and cause a
        // use-after-free.
        Ref<> global_value = get_global(name_idx);
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
        BorrowedRef<> name = PyTuple_GET_ITEM(names.get(), name_idx);
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
      case EAGER_IMPORT_NAME:
      case IMPORT_NAME: {
        if (previous_previous_instr == std::nullopt ||
            previous_instr == std::nullopt) {
          break;
        }

        bool is_absolute = false;
        if (previous_previous_instr->opcode() == LOAD_CONST) {
          BorrowedRef<> level = constArg(*previous_previous_instr);
          if (PyLong_CheckExact(level)) {
            is_absolute = PyLong_AsLong(level) == 0;
            PyErr_Clear();
          }
        }
#if PY_VERSION_HEX >= 0x030E0000
        if (previous_previous_instr->opcode() == LOAD_SMALL_INT) {
          is_absolute = previous_previous_instr->oparg() == 0;
        }
#endif
        if (!is_absolute) {
          break;
        }

        BorrowedRef<> fromlist;
        if (previous_instr->opcode() == LOAD_CONST) {
          fromlist = constArg(*previous_instr);
        }
#if PY_VERSION_HEX >= 0x030F0000
        constexpr int kCommonConstantNone = 7;
        if (previous_instr->opcode() == LOAD_COMMON_CONSTANT &&
            previous_instr->oparg() == kCommonConstantNone) {
          fromlist = Py_None;
        }
#endif
        if (fromlist == nullptr) {
          break;
        }
        std::optional<PreloadedImport> info = makePreloadedImport(
            code_, builtins_, bc_instr, fromlist, isModuleCodeObject());
        if (info != std::nullopt) {
          import_candidates.emplace_back(
              bc_instr.baseOffset(), std::move(*info));
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
      case LOAD_LOCAL:
      case STORE_LOCAL: {
        BorrowedRef<PyTupleObject> index_and_descr{constArg(bc_instr)};
        BorrowedRef<> descr = PyTuple_GET_ITEM(index_and_descr, 1);
        int prim_type = _PyClassLoader_ResolvePrimitiveType(descr);
        JIT_THROW_IF(
            prim_type == -1,
            "Unknown local type descr {} during preloading of {}",
            repr(descr),
            fullname());
        if (prim_type != TYPED_OBJECT) {
          int index = PyLong_AsLong(PyTuple_GET_ITEM(index_and_descr, 0));
          primitive_local_types_.emplace(index, prim_type_to_type(prim_type));
        }
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
    previous_previous_instr = previous_instr;
    previous_instr = bc_instr;
  }

  // Re-fetch the global values at the end after we may have triggered side
  // effects
  for (auto& idx_and_ref : global_caches_) {
    Ref<> global_value = get_global(idx_and_ref.first);
    if (PyErr_Occurred()) {
      return false;
    }
#if PY_VERSION_HEX >= 0x030F0000
    // In 3.15+ a name bound by a lazy import is stored in the namespace
    // dict as an unresolved placeholder that LOAD_GLOBAL resolves on
    // access. Don't cache it, otherwise the GlobalCache would hand JIT'd
    // code the placeholder instead of the imported value.
    if (global_value != nullptr && PyLazyImport_CheckExact(global_value)) {
      continue;
    }
#endif
    global_values_.emplace(idx_and_ref.first, std::move(global_value));
  }

  // Import and global preloading can execute Python and mutate any of the
  // watched dictionaries. Only publish records whose cells still contain the
  // exact objects observed when the record was built.
  for (auto& [bc_off, info] : import_candidates) {
    if (importInfoStillValid(info)) {
      preloaded_imports_.emplace(bc_off, std::move(info));
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
    target->builtin_name = def->ml_name != nullptr ? def->ml_name : "";
    if (def->ml_flags == METH_NOARGS) {
      target->builtin_expected_nargs = 1;
    } else if (def->ml_flags == METH_O) {
      target->builtin_expected_nargs = 2;
    } else if ((tmd = _PyClassLoader_GetTypedMethodDef(callable))) {
      target->builtin_returns_error_code = (tmd->tmd_ret == Ci_Py_SIG_ERROR);
      target->builtin_returns_void = (tmd->tmd_ret == Ci_Py_SIG_VOID);
      target->builtin_c_func = tmd->tmd_meth;
      target->builtin_name = def->ml_name != nullptr ? def->ml_name : "";
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

Preloader* PreloaderManager::add(std::unique_ptr<Preloader> preloader) {
  CompilationKey key{
      preloader->code(), preloader->builtins(), preloader->globals()};
  JITCompilationLock lock;
  auto [val, inserted] = preloaders_.try_emplace(key, std::move(preloader));
  return val->second.get();
}

Preloader* PreloaderManager::add(
    BorrowedRef<PyFunctionObject> func,
    Ref<> reifier) {
  JIT_DCHECK(
      ThreadedCompileContext::canAccessSharedData(),
      "preloading requires Python runtime access");

  std::unique_ptr<Preloader> preloader =
      Preloader::make(func, std::move(reifier));
  if (preloader == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  CompilationKey key{
      preloader->code(), preloader->builtins(), preloader->globals()};
  JITCompilationLock lock;
  auto [it, inserted] = preloaders_.try_emplace(key, nullptr);
  if (inserted) {
    it->second = std::move(preloader);
  }
  return it->second.get();
}

Preloader* PreloaderManager::find(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals) {
  // Preloaders are kept alive by their PreloaderManager which is typically a
  // thread-local which gets cleaned up when the compilation is done. In
  // multi-threaded compile the PreloaderManager is shared until all the
  // compiling threads are released. The lock is only necessary for
  // multi-threaded compile.
  JITCompilationLock lock;
  auto it = preloaders_.find(CompilationKey{code, builtins, globals});
  return it != preloaders_.end() ? it->second.get() : nullptr;
}

Preloader* PreloaderManager::find(BorrowedRef<PyFunctionObject> func) {
  JIT_DCHECK(PyCode_Check(func->func_code), "always code");
  JIT_DCHECK(PyDict_Check(func->func_builtins), "always dict");
  if (!PyDict_Check(func->func_builtins)) {
    return nullptr;
  }
  return find(
      BorrowedRef<PyCodeObject>{func->func_code},
      BorrowedRef<PyDictObject>{func->func_builtins},
      BorrowedRef<PyDictObject>{func->func_globals});
}

bool PreloaderManager::empty() const {
  JITCompilationLock lock;
  return preloaders_.empty();
}

size_t PreloaderManager::size() const {
  JITCompilationLock lock;
  return preloaders_.size();
}

void PreloaderManager::clear() {
  JITCompilationLock lock;
  preloaders_.clear();
}

PreloaderMap PreloaderManager::extract() {
  JITCompilationLock lock;
  PreloaderMap result;
  result.swap(preloaders_);
  return result;
}

void PreloaderManager::install(PreloaderMap preloaders) {
  JITCompilationLock lock;
  preloaders_ = std::move(preloaders);
}

PreloaderManager& preloaderManager() {
  JIT_DCHECK(
      tls_manager != nullptr, "Should have isolated preloader installed");
  return *tls_manager;
}

void setThreadLocalPreloaderManager(PreloaderManager* mgr) {
  tls_manager = mgr;
}

IsolatedPreloaders::IsolatedPreloaders() : prev_manager_(tls_manager) {
  tls_manager = &local_manager_;
}

IsolatedPreloaders::~IsolatedPreloaders() {
  tls_manager = prev_manager_;
}

PreloaderManager* IsolatedPreloaders::manager() {
  return &local_manager_;
}

const PreloaderManager* IsolatedPreloaders::manager() const {
  return &local_manager_;
}

} // namespace cinderx::jit::hir

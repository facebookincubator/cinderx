// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/runtime.h"

#include "internal/pycore_interp.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/module_state.h"
#include "cinderx/python_runtime.h"

#include <sys/mman.h>

namespace jit {

PyObject* yieldFromValue(
    GenDataFooter* gen_footer,
    const GenYieldPoint* yield_point) {
  return yield_point->isYieldFrom()
      ? reinterpret_cast<PyObject*>(
            *(reinterpret_cast<uint64_t*>(gen_footer) +
              yield_point->yieldFromOffset()))
      : nullptr;
}

void Builtins::init() {
  ThreadedCompileSerialize guard;
  if (is_initialized_) {
    return;
  }
  // we want to check the exact function address, rather than relying on
  // modules which can be mutated.  First find builtins, which we have
  // to do a search for because PyEval_GetBuiltins() returns the
  // module dict.
  PyObject* mods =
      CI_INTERP_IMPORT_FIELD(_PyInterpreterState_GET(), modules_by_index);
  PyModuleDef* builtins = nullptr;
  for (Py_ssize_t i = 0; i < PyList_GET_SIZE(mods); i++) {
    PyObject* cur = PyList_GET_ITEM(mods, i);
    if (cur == Py_None) {
      continue;
    }
    PyModuleDef* def = PyModule_GetDef(cur);
    if (def == nullptr) {
      PyErr_Clear();
      continue;
    }
    if (std::strcmp(def->m_name, "builtins") == 0) {
      builtins = def;
      break;
    }
  }
  JIT_CHECK(builtins != nullptr, "could not find builtins module");

  auto add = [this](const std::string& name, PyMethodDef* meth) {
    cfunc_to_name_[meth] = name;
    name_to_cfunc_[name] = meth;
  };
  // Find all free functions.
  for (PyMethodDef* fdef = builtins->m_methods; fdef->ml_name != nullptr;
       fdef++) {
    add(fdef->ml_name, fdef);
  }
  // Find all methods on types.
  PyTypeObject* types[] = {
      &PyDict_Type,
      &PyList_Type,
      &PyTuple_Type,
      &PyUnicode_Type,
  };
  for (auto type : types) {
    for (PyMethodDef* fdef = type->tp_methods; fdef->ml_name != nullptr;
         fdef++) {
      add(fmt::format("{}.{}", type->tp_name, fdef->ml_name), fdef);
    }
  }
  // Only mark as initialized after everything is done to avoid concurrent
  // reads of an unfinished map.
  is_initialized_ = true;
}

bool Builtins::isInitialized() const {
  return is_initialized_;
}

std::optional<std::string> Builtins::find(PyMethodDef* meth) const {
  auto result = cfunc_to_name_.find(meth);
  if (result == cfunc_to_name_.end()) {
    return std::nullopt;
  }
  return result->second;
}

std::optional<PyMethodDef*> Builtins::find(const std::string& name) const {
  auto result = name_to_cfunc_.find(name);
  if (result == name_to_cfunc_.end()) {
    return std::nullopt;
  }
  return result->second;
}

Runtime* Runtime::get() {
  return static_cast<Runtime*>(cinderx::getModuleState()->runtime());
}

Runtime* Runtime::getUnchecked() {
  if (auto moduleState = cinderx::getModuleState()) {
    return static_cast<Runtime*>(moduleState->runtime());
  }
  return nullptr;
}

Runtime::Runtime()
    : zero_(Ref<>::create(PyLong_FromLong(0))),
#if PY_VERSION_HEX >= 0x030C0000
      str_build_class_(Ref<>::create(&_Py_ID(__build_class__)))
#else
      str_build_class_(
          Ref<>::create(PyUnicode_InternFromString("__build_class__")))
#endif
{
#if PY_VERSION_HEX >= 0x030E0000
  PyObject** common_consts = PyThreadState_GET()->interp->common_consts;
  for (int i = 0; i < NUM_COMMON_CONSTANTS; i++) {
    common_constant_types_.emplace_back(
        hir::Type::fromObject(common_consts[i]));
  }
#endif
}

void Runtime::mlockProfilerDependencies() {
  for (auto& codert : code_runtimes_) {
    PyCodeObject* code = codert.frameState()->code().get();
    ::mlock(code, sizeof(PyCodeObject));
    ::mlock(code->co_qualname, Py_SIZE(code->co_qualname));
  }
  code_runtimes_.mlock();
}

Ref<> Runtime::pageInProfilerDependencies() {
  ThreadedCompileSerialize guard;
  Ref<> qualnames = Ref<>::steal(PyList_New(0));
  if (qualnames == nullptr) {
    return nullptr;
  }
  // We want to force the OS to page in the memory on the
  // code_rt->code->qualname path and keep the compiler from optimizing away
  // the code to do so. There are probably more efficient ways of doing this
  // but perf isn't a major concern.
  for (auto& code_rt : code_runtimes_) {
    BorrowedRef<> qualname = code_rt.frameState()->code()->co_qualname;
    if (qualname == nullptr) {
      continue;
    }
    if (PyList_Append(qualnames, qualname) < 0) {
      return nullptr;
    }
  }
  return qualnames;
}

void** Runtime::findFunctionEntryCache(PyFunctionObject* function) {
  auto result = function_entry_caches_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(function),
      std::forward_as_tuple());
  addReference(reinterpret_cast<PyObject*>(function));
  if (result.second) {
    result.first->second.ptr = pointer_caches_.allocate();
    // _PyClassLoader_HasPrimitiveArgs doesn't work well in multi-threaded
    // compile in 3.12+ due to access of a dictionary with non-key strings.
    // We fix this up post-compile in the multi-threaded case.
    if (!getThreadedCompileContext().compileRunning() &&
        _PyClassLoader_HasPrimitiveArgs((PyCodeObject*)function->func_code)) {
      result.first->second.arg_info =
          Ref<_PyTypedArgsInfo>::steal(_PyClassLoader_GetTypedArgsInfo(
              (PyCodeObject*)function->func_code, 1));
    }
  }
  return result.first->second.ptr;
}

// See comments in findFunctionEntryCache.
void Runtime::fixupFunctionEntryCachePostMultiThreadedCompile() {
  for (auto& entry : function_entry_caches_) {
    BorrowedRef<PyCodeObject> code{entry.first->func_code};
    if (entry.second.arg_info.get() == nullptr &&
        _PyClassLoader_HasPrimitiveArgs(code)) {
      entry.second.arg_info = Ref<_PyTypedArgsInfo>::steal(
          _PyClassLoader_GetTypedArgsInfo(code, 1));
    }
  }
}

bool Runtime::hasFunctionEntryCache(PyFunctionObject* function) const {
  return function_entry_caches_.find(function) != function_entry_caches_.end();
}

_PyTypedArgsInfo* Runtime::findFunctionPrimitiveArgInfo(
    PyFunctionObject* function) {
  auto cache = function_entry_caches_.find(function);
  if (cache == function_entry_caches_.end()) {
    return nullptr;
  }
  return cache->second.arg_info.get();
}

void Runtime::recordDeopt(
    CodeRuntime* code_runtime,
    std::size_t idx,
    BorrowedRef<> guilty_value) {
  DeoptStat& stat = deopt_stats_[code_runtime][idx];
  stat.count++;
  if (guilty_value != nullptr) {
    stat.types.recordType(Py_TYPE(guilty_value));
  }
}

const DeoptStat* Runtime::deoptStat(
    const CodeRuntime* code_runtime,
    std::size_t deopt_idx) const {
  auto map_it = deopt_stats_.find(code_runtime);
  if (map_it == deopt_stats_.end()) {
    return nullptr;
  }
  auto stat_it = map_it->second.find(deopt_idx);
  if (stat_it == map_it->second.end()) {
    return nullptr;
  }
  return &stat_it->second;
}

void Runtime::clearDeoptStats() {
  deopt_stats_.clear();
}

InlineCacheStats Runtime::getAndClearLoadMethodCacheStats() {
  InlineCacheStats stats;
  for (auto& cache : load_method_caches_) {
    if (cache.cacheStats() == nullptr) {
      // Cache stat may not have been initialized if LoadMethodCached instr was
      // optimized away.
      continue;
    }
    stats.push_back(*cache.cacheStats());
    cache.clearCacheStats();
  }
  return stats;
}

InlineCacheStats Runtime::getAndClearLoadTypeMethodCacheStats() {
  InlineCacheStats stats;
  for (auto& cache : load_type_method_caches_) {
    if (cache.cacheStats() == nullptr) {
      // Cache stat may not have been initialized if LoadTypeMethod instr
      // was optimized away.
      continue;
    }
    stats.push_back(*cache.cacheStats());
    cache.clearCacheStats();
  }
  return stats;
}

void Runtime::setGuardFailureCallback(Runtime::GuardFailureCallback cb) {
  guard_failure_callback_ = cb;
}

void Runtime::guardFailed(const DeoptMetadata& deopt_meta) {
  if (guard_failure_callback_) {
    guard_failure_callback_(deopt_meta);
  }
}

void Runtime::clearGuardFailureCallback() {
  guard_failure_callback_ = nullptr;
}

void Runtime::addReference(BorrowedRef<> obj) {
  // Serialize as we modify the ref-count to obj which may be widely accessible.
  ThreadedCompileSerialize guard;
  references_.emplace(ThreadedRef<>::create(obj));
}

void Runtime::releaseReferences() {
  for (auto& code_rt : code_runtimes_) {
    code_rt.releaseReferences();
  }
  references_.clear();
  type_deopt_patchers_.clear();
}

LoadAttrCache* Runtime::allocateLoadAttrCache() {
  return load_attr_caches_.allocate();
}

LoadTypeAttrCache* Runtime::allocateLoadTypeAttrCache() {
  return load_type_attr_caches_.allocate();
}

LoadMethodCache* Runtime::allocateLoadMethodCache() {
  return load_method_caches_.allocate();
}

LoadModuleAttrCache* Runtime::allocateLoadModuleAttrCache() {
  return load_module_attr_caches_.allocate();
}

LoadModuleMethodCache* Runtime::allocateLoadModuleMethodCache() {
  return load_module_method_caches_.allocate();
}

LoadTypeMethodCache* Runtime::allocateLoadTypeMethodCache() {
  return load_type_method_caches_.allocate();
}

StoreAttrCache* Runtime::allocateStoreAttrCache() {
  return store_attr_caches_.allocate();
}

const Builtins& Runtime::builtins() {
  // Lock-free fast path followed by single-lock slow path during
  // initialization.
  if (!builtins_.isInitialized()) {
    builtins_.init();
  }
  return builtins_;
}

void Runtime::watchType(
    BorrowedRef<PyTypeObject> type,
    TypeDeoptPatcher* patcher) {
  ThreadedCompileSerialize guard;
  type_deopt_patchers_[type].emplace_back(patcher);
  if constexpr (PY_VERSION_HEX >= 0x030C0000) {
    // In 3.12 we require the interpreter state in order to watch types
    if (getThreadedCompileContext().compileRunning()) {
      pending_watches_.emplace(type);
      return;
    }
  }

  JIT_CHECK(
      cinderx::getModuleState()->watcherState().watchType(type) == 0,
      "Failed to watch type {}",
      type->tp_name);
}

BorrowedRef<> Runtime::zero() {
  return zero_.get();
}

BorrowedRef<> Runtime::strBuildClass() {
  return str_build_class_.get();
}

void Runtime::watchPendingTypes() {
  for (auto& type : pending_watches_) {
    JIT_CHECK(
        cinderx::getModuleState()->watcherState().watchType(type) == 0,
        "Failed to watch pending type {}",
        type->tp_name);
  }
  pending_watches_.clear();
}

void Runtime::notifyTypeModified(
    BorrowedRef<PyTypeObject> lookup_type,
    BorrowedRef<PyTypeObject> new_type) {
  notifyICsTypeChanged(lookup_type);

  ThreadedCompileSerialize guard;
  auto it = type_deopt_patchers_.find(lookup_type);
  if (it == type_deopt_patchers_.end()) {
    return;
  }

  std::vector<TypeDeoptPatcher*> remaining_patchers;
  for (TypeDeoptPatcher* patcher : it->second) {
    if (!patcher->maybePatch(new_type)) {
      remaining_patchers.emplace_back(patcher);
    }
  }

  if (remaining_patchers.empty()) {
    type_deopt_patchers_.erase(it);
    // don't unwatch type; shadowcode may still be watching it
  } else {
    it->second = std::move(remaining_patchers);
  }
}

const hir::Type& Runtime::typeForCommonConstant([[maybe_unused]] int i) const {
#if PY_VERSION_HEX >= 0x030E0000
  return common_constant_types_.at(i);
#endif
  JIT_ABORT("Common constants are a feature of 3.14+");
}

#if PY_VERSION_HEX < 0x030C0000
// JIT generator data free-list globals
const size_t kGenDataFreeListMaxSize = 1024;
static size_t gen_data_free_list_size = 0;
static void* gen_data_free_list_tail;

jit::GenDataFooter* jitgen_data_allocate(size_t spill_words) {
  spill_words = std::max(spill_words, jit::kMinGenSpillWords);
  if (spill_words > jit::kMinGenSpillWords || !gen_data_free_list_size) {
    auto data =
        malloc(spill_words * sizeof(uint64_t) + sizeof(jit::GenDataFooter));
    auto footer = reinterpret_cast<jit::GenDataFooter*>(
        reinterpret_cast<uint64_t*>(data) + spill_words);
    footer->spillWords = spill_words;
    return footer;
  }

  // All free list entries are spill-word size 89, so we don't need to set
  // footer->spillWords again, it should still be set to 89 from previous use.
  JIT_DCHECK(spill_words == jit::kMinGenSpillWords, "invalid size");

  gen_data_free_list_size--;
  void* data = gen_data_free_list_tail;
  gen_data_free_list_tail = *reinterpret_cast<void**>(gen_data_free_list_tail);
  return reinterpret_cast<jit::GenDataFooter*>(
      reinterpret_cast<uint64_t*>(data) + spill_words);
}

void jitgen_data_free(PyGenObject* gen) {
  auto gen_data_footer =
      reinterpret_cast<jit::GenDataFooter*>(gen->gi_jit_data);
  gen->gi_jit_data = nullptr;
  auto gen_data = reinterpret_cast<uint64_t*>(gen_data_footer) -
      gen_data_footer->spillWords;

  if (gen_data_footer->spillWords != jit::kMinGenSpillWords ||
      gen_data_free_list_size == kGenDataFreeListMaxSize) {
    free(gen_data);
    return;
  }

  if (gen_data_free_list_size) {
    *reinterpret_cast<void**>(gen_data) = gen_data_free_list_tail;
  }
  gen_data_free_list_size++;
  gen_data_free_list_tail = gen_data;
}
#endif // PY_VERSION_HEX < 0x030C0000

} // namespace jit

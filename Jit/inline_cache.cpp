// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/inline_cache.h"

#include <Python.h>

#include "cinderx/Common/dict.h"
#include "cinderx/Common/func.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Common/watchers.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#include <algorithm>
#include <memory>

// clang-format off
#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif
#include "cinderx/Upgrade/upgrade_assert.h"  // @donotremove
#include "cinderx/Upgrade/upgrade_stubs.h"  // @donotremove
#include "internal/pycore_pystate.h"
#include "internal/pycore_object.h"
#include "structmember.h"
// clang-format on

namespace jit {

namespace {

template <class T>
struct TypeWatcher {
  jit::UnorderedMap<BorrowedRef<PyTypeObject>, jit::UnorderedSet<T*>> caches;

  void watch(BorrowedRef<PyTypeObject> type, T* cache) {
    Ci_Watchers_WatchType(type);
    caches[type].emplace(cache);
  }

  void unwatch(BorrowedRef<PyTypeObject> type, T* cache) {
    auto it = caches.find(type);
    if (it == caches.end()) {
      return;
    }
    it->second.erase(cache);
    // don't unwatch type; shadowcode may still be watching it
  }

  void typeChanged(BorrowedRef<PyTypeObject> type) {
    auto it = caches.find(type);
    if (it == caches.end()) {
      return;
    }
    jit::UnorderedSet<T*> to_notify = std::move(it->second);
    caches.erase(it);
    for (T* cache : to_notify) {
      cache->typeChanged(type);
    }
  }
};

TypeWatcher<AttributeCache> ac_watcher;
TypeWatcher<LoadTypeAttrCache> ltac_watcher;
TypeWatcher<LoadMethodCache> lm_watcher;
TypeWatcher<LoadTypeMethodCache> ltm_watcher;

constexpr uintptr_t kKindMask = 0x07;

// Sentinel PyTypeObject that must never escape into user code.
PyTypeObject s_empty_type_attr_cache = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "EmptyLoadTypeAttrCache",
};

inline PyDictObject* get_dict(PyObject* obj, Py_ssize_t dictoffset) {
  PyObject** dictptr = (PyObject**)((char*)obj + dictoffset);
  return (PyDictObject*)*dictptr;
}

inline PyDictObject* get_or_allocate_dict(
    PyObject* obj,
    Py_ssize_t dict_offset) {
  PyDictObject* dict = get_dict(obj, dict_offset);
  if (dict == nullptr) {
    dict =
        reinterpret_cast<PyDictObject*>(PyObject_GenericGetDict(obj, nullptr));
    if (dict == nullptr) {
      return nullptr;
    }
    Py_DECREF(dict);
  }
  return dict;
}

PyObject* __attribute__((noinline)) raise_attribute_error(
    PyObject* obj,
    PyObject* name) {
  PyErr_Format(
      PyExc_AttributeError,
      "'%.50s' object has no attribute '%U'",
      Py_TYPE(obj)->tp_name,
      name);
  Cix_set_attribute_error_context(obj, name);
  return nullptr;
}

uint64_t getModuleVersion(BorrowedRef<PyModuleObject> mod) {
  if (mod->md_dict) {
    BorrowedRef<PyDictObject> md_dict = mod->md_dict;
    return md_dict->ma_version_tag;
  }
  return 0;
}

uint64_t getModuleVersion(BorrowedRef<Ci_StrictModuleObject> mod) {
  if (mod->globals) {
    BorrowedRef<PyDictObject> globals = mod->globals;
    return globals->ma_version_tag;
  }
  return 0;
}

void maybeCollectCacheStats(
    std::unique_ptr<CacheStats>& stat,
    BorrowedRef<PyTypeObject> tp,
    BorrowedRef<> name,
    CacheMissReason reason) {
  if (!getConfig().collect_attr_cache_stats) {
    return;
  }
  std::string key =
      fmt::format("{}.{}", typeFullname(tp), PyUnicode_AsUTF8(name));
  stat->misses.insert({key, CacheMiss{0, reason}}).first->second.count++;
}

} // namespace

int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  BorrowedRef<PyDictObject> dict = get_or_allocate_dict(obj, dict_offset);
  if (dict == nullptr) {
    return -1;
  }
  if ((dict->ma_keys == keys) &&
      ((dict->ma_used == val_offset) ||
       (DICT_VALUES(dict.get())[val_offset] != nullptr))) {
    PyObject* old_value = DICT_VALUES(dict.get())[val_offset];

    if (!_PyObject_GC_IS_TRACKED(dict.getObj())) {
      if (_PyObject_GC_MAY_BE_TRACKED(value)) {
        _PyObject_GC_TRACK(dict.getObj());
      }
    }

    uint64_t new_version =
        _PyDict_NotifyEvent(PyDict_EVENT_MODIFIED, dict, name, value);

    Py_INCREF(value);
    DICT_VALUES(dict.get())[val_offset] = value;
    dict->ma_version_tag = new_version;

    if (old_value == nullptr) {
      dict->ma_used++;
    } else {
      Py_DECREF(old_value);
    }

    return 0;
  }

  auto strong_ref = Ref<>::create(dict);
  return PyDict_SetItem(dict, name, value);
}

PyObject* SplitMutator::getAttr(PyObject* obj, PyObject* name) {
  PyDictObject* dict = get_dict(obj, dict_offset);
  if (dict == nullptr) {
    return raise_attribute_error(obj, name);
  }
  PyObject* result = nullptr;
  if (dict->ma_keys == keys) {
    result = DICT_VALUES(dict)[val_offset];
  } else {
    auto dictobj = reinterpret_cast<PyObject*>(dict);
    Py_INCREF(dictobj);
    result = PyDict_GetItem(dictobj, name);
    Py_DECREF(dictobj);
  }
  if (result == nullptr) {
    return raise_attribute_error(obj, name);
  }
  Py_INCREF(result);
  return result;
}

int CombinedMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  BorrowedRef<PyDictObject> dict = get_or_allocate_dict(obj, dict_offset);
  if (dict == nullptr) {
    return -1;
  }
  auto strong_ref = Ref<>::create(dict);
  return PyDict_SetItem(dict, name, value);
}

PyObject* CombinedMutator::getAttr(PyObject* obj, PyObject* name) {
  auto dict = reinterpret_cast<PyObject*>(get_dict(obj, dict_offset));
  if (dict == nullptr) {
    return raise_attribute_error(obj, name);
  }
  Py_INCREF(dict);
  PyObject* result = PyDict_GetItem(dict, name);
  Py_DECREF(dict);
  if (result == nullptr) {
    return raise_attribute_error(obj, name);
  }
  Py_INCREF(result);
  return result;
}

int DataDescrMutator::setAttr(PyObject* obj, PyObject* value) {
  return Py_TYPE(descr)->tp_descr_set(descr, obj, value);
}

PyObject* DataDescrMutator::getAttr(PyObject* obj) {
  return Py_TYPE(descr)->tp_descr_get(descr, obj, (PyObject*)Py_TYPE(obj));
}

int MemberDescrMutator::setAttr(PyObject* obj, PyObject* value) {
  return PyMember_SetOne((char*)obj, memberdef, value);
}

PyObject* MemberDescrMutator::getAttr(PyObject* obj) {
  return PyMember_GetOne((char*)obj, memberdef);
}

int DescrOrClassVarMutator::setAttr(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  descrsetfunc setter = Py_TYPE(descr)->tp_descr_set;
  if (setter != nullptr) {
    auto descr_guard = Ref<>::create(descr);
    return setter(descr, obj, value);
  }
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr == nullptr) {
    PyErr_Format(
        PyExc_AttributeError,
        "'%.50s' object attribute '%U' is read-only",
        Py_TYPE(obj)->tp_name,
        name);
    return -1;
  }
  BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
  int st = Cix_PyObjectDict_SetItem(type, dictptr, name, value);
  if (st < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
    PyErr_SetObject(PyExc_AttributeError, name);
  }
#if PY_VERSION_HEX < 0x030C0000
  if (PyType_HasFeature(type, Py_TPFLAGS_NO_SHADOWING_INSTANCES)) {
    _PyType_ClearNoShadowingInstances(type, descr);
  }
#else
  UPGRADE_NOTE(CHANGED_NO_SHADOWING_INSTANCES, T200294456)
#endif
  return st;
}

PyObject* DescrOrClassVarMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyTypeObject> descr_type(Py_TYPE(descr));
  descrsetfunc setter = descr_type->tp_descr_set;
  descrgetfunc getter = descr_type->tp_descr_get;

  auto descr_guard = Ref<>::create(descr);
  if (setter != nullptr && getter != nullptr) {
    BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
    return getter(descr, obj, type);
  }

  Ref<> dict;
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr != nullptr) {
    dict.reset(*dictptr);
  }

  // Check instance dict.
  if (dict != nullptr) {
    auto res = Ref<>::create(PyDict_GetItem(dict, name));
    if (res != nullptr) {
      return res.release();
    }
  }

  if (getter != nullptr) {
    // Non-data descriptor
    BorrowedRef<PyTypeObject> type(Py_TYPE(obj));
    return getter(descr, obj, type);
  }

  // Class var
  return descr_guard.release();
}

AttributeMutator::AttributeMutator() {
  reset();
}

PyTypeObject* AttributeMutator::type() const {
  // clear tagged bits and return
  return reinterpret_cast<PyTypeObject*>(type_ & ~kKindMask);
}

void AttributeMutator::reset() {
  set_type(nullptr, Kind::kEmpty);
}

bool AttributeMutator::isEmpty() const {
  return get_kind() == Kind::kEmpty;
}

void AttributeMutator::set_combined(PyTypeObject* type) {
  set_type(type, Kind::kCombined);
  combined_.dict_offset = type->tp_dictoffset;
}

void AttributeMutator::set_data_descr(PyTypeObject* type, PyObject* descr) {
  set_type(type, Kind::kDataDescr);
  data_descr_.descr = descr;
}

void AttributeMutator::set_member_descr(PyTypeObject* type, PyObject* descr) {
  set_type(type, Kind::kMemberDescr);
  member_descr_.memberdef = ((PyMemberDescrObject*)descr)->d_member;
}

void AttributeMutator::set_descr_or_classvar(
    PyTypeObject* type,
    PyObject* descr) {
  set_type(type, Kind::kDescrOrClassVar);
  descr_or_cvar_.descr = descr;
}

void AttributeMutator::set_split(
    PyTypeObject* type,
    Py_ssize_t val_offset,
    PyDictKeysObject* keys) {
  set_type(type, Kind::kSplit);
  JIT_CHECK(
      type->tp_dictoffset <= std::numeric_limits<uint32_t>::max(),
      "Dict offset does not fit into a 32-bit int");
  JIT_CHECK(
      val_offset <= std::numeric_limits<uint32_t>::max(),
      "Val offset does not fit into a 32-bit int");
  split_.dict_offset = static_cast<uint32_t>(type->tp_dictoffset);
  split_.val_offset = static_cast<uint32_t>(val_offset);
  split_.keys = keys;
}

inline int
AttributeMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  AttributeMutator::Kind kind = get_kind();
  switch (kind) {
    case AttributeMutator::Kind::kSplit:
      return split_.setAttr(obj, name, value);
    case AttributeMutator::Kind::kCombined:
      return combined_.setAttr(obj, name, value);
    case AttributeMutator::Kind::kDataDescr:
      return data_descr_.setAttr(obj, value);
    case AttributeMutator::Kind::kMemberDescr:
      return member_descr_.setAttr(obj, value);
    case AttributeMutator::Kind::kDescrOrClassVar:
      return descr_or_cvar_.setAttr(obj, name, value);
    default:
      JIT_ABORT(
          "Cannot invoke setAttr for attr of kind {}", static_cast<int>(kind));
  }
}

inline PyObject* AttributeMutator::getAttr(PyObject* obj, PyObject* name) {
  AttributeMutator::Kind kind = get_kind();
  switch (kind) {
    case AttributeMutator::Kind::kSplit:
      return split_.getAttr(obj, name);
    case AttributeMutator::Kind::kCombined:
      return combined_.getAttr(obj, name);
    case AttributeMutator::Kind::kDataDescr:
      return data_descr_.getAttr(obj);
    case AttributeMutator::Kind::kMemberDescr:
      return member_descr_.getAttr(obj);
    case AttributeMutator::Kind::kDescrOrClassVar:
      return descr_or_cvar_.getAttr(obj, name);
    default:
      JIT_ABORT(
          "Cannot invoke getAttr for attr of kind {}", static_cast<int>(kind));
  }
}

void AttributeMutator::set_type(PyTypeObject* type, Kind kind) {
  auto raw = reinterpret_cast<uintptr_t>(type);
  JIT_CHECK((raw & kKindMask) == 0, "PyTypeObject* expected to be aligned");
  auto mask = static_cast<uintptr_t>(kind);
  type_ = raw | mask;
}

AttributeMutator::Kind AttributeMutator::get_kind() const {
  return static_cast<Kind>(type_ & kKindMask);
}

AttributeCache::AttributeCache() {
  for (auto& entry : entries()) {
    entry.reset();
  }
}

AttributeCache::~AttributeCache() {
  for (auto& entry : entries()) {
    if (entry.type() != nullptr) {
      ac_watcher.unwatch(entry.type(), this);
      entry.reset();
    }
  }
}

void AttributeCache::typeChanged(PyTypeObject*) {
  for (auto& entry : entries()) {
    entry.reset();
  }
}

std::span<AttributeMutator> AttributeCache::entries() {
  return {entries_, getConfig().attr_cache_size};
}

AttributeMutator* AttributeCache::findEmptyEntry() {
  auto it = std::ranges::find_if(
      entries(), [](const AttributeMutator& e) { return e.isEmpty(); });
  return it == entries().end() ? nullptr : &*it;
}

void AttributeCache::fill(BorrowedRef<PyTypeObject> type, BorrowedRef<> name) {
  BorrowedRef<> descr = _PyType_Lookup(type, name);
  fill(type, name, descr);
}

void AttributeCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name,
    BorrowedRef<> descr) {
  if (!PyType_HasFeature(type, Py_TPFLAGS_VALID_VERSION_TAG)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  AttributeMutator* mut = findEmptyEntry();
  if (mut == nullptr) {
    return;
  }

  if (descr != nullptr) {
    BorrowedRef<PyTypeObject> descr_type(Py_TYPE(descr));
    if (descr_type->tp_descr_get != nullptr &&
        descr_type->tp_descr_set != nullptr) {
      // Data descriptor
      if (descr_type == &PyMemberDescr_Type) {
        mut->set_member_descr(type, descr);
      } else {
        // If someone deletes descr_types's __set__ method, it will no longer
        // be a data descriptor, and the cache kind has to change.
        ac_watcher.watch(descr_type, this);
        mut->set_data_descr(type, descr);
      }
    } else {
      // Non-data descriptor or class var
      mut->set_descr_or_classvar(type, descr);
    }
    ac_watcher.watch(type, this);
    return;
  }

  if (type->tp_dictoffset < 0 ||
      !PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE)) {
    // We only support the common case for objects - fixed-size instances
    // (tp_dictoffset >= 0) of heap types (Py_TPFLAGS_HEAPTYPE).
    return;
  }

  // Instance attribute with no shadowing. Specialize the lookup based on
  // whether or not the type is using split dictionaries.
  PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(type.get());
  PyDictKeysObject* keys = ht->ht_cached_keys;
  Py_ssize_t val_offset;
  if (keys != nullptr &&
      (val_offset = _PyDictKeys_GetSplitIndex(keys, name)) != -1) {
    mut->set_split(type, val_offset, keys);
  } else {
    mut->set_combined(type);
  }
  ac_watcher.watch(type, this);
}

int StoreAttrCache::invoke(
    StoreAttrCache* cache,
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  return cache->doInvoke(obj, name, value);
}

int StoreAttrCache::doInvoke(PyObject* obj, PyObject* name, PyObject* value) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);
  for (auto& entry : entries()) {
    if (entry.type() == tp) {
      return entry.setAttr(obj, name, value);
    }
  }
  return invokeSlowPath(obj, name, value);
}

int __attribute__((noinline))
StoreAttrCache::invokeSlowPath(PyObject* obj, PyObject* name, PyObject* value) {
  int result = PyObject_SetAttr(obj, name, value);
  if (result < 0) {
    JIT_DCHECK(
        PyErr_Occurred(),
        "PyObject_SetAttr failed so there should be a Python error");
    return result;
  }

  BorrowedRef<PyTypeObject> type{Py_TYPE(obj)};
  if (type->tp_setattro == PyObject_GenericSetAttr) {
    fill(type, name);
  }

  return result;
}

PyObject*
LoadAttrCache::invoke(LoadAttrCache* cache, PyObject* obj, PyObject* name) {
  return cache->doInvoke(obj, name);
}

PyObject* LoadAttrCache::doInvoke(PyObject* obj, PyObject* name) {
  PyTypeObject* tp = Py_TYPE(obj);
  for (auto& entry : entries()) {
    if (entry.type() == tp) {
      return entry.getAttr(obj, name);
    }
  }
  return invokeSlowPath(obj, name);
}

PyObject* __attribute__((noinline)) LoadAttrCache::invokeSlowPath(
    PyObject* obj,
    PyObject* name) {
  auto result = Ref<>::steal(PyObject_GetAttr(obj, name));
  if (result == nullptr) {
    JIT_DCHECK(
        PyErr_Occurred(),
        "PyObject_GetAttr failed so there should be a Python error");
    return nullptr;
  }

  BorrowedRef<PyTypeObject> type{Py_TYPE(obj)};
  if (type->tp_getattro == PyObject_GenericGetAttr) {
    fill(type, name);
  }

  return result.release();
}

LoadTypeAttrCache::LoadTypeAttrCache() {
  reset();
}

LoadTypeAttrCache::~LoadTypeAttrCache() {
  ltac_watcher.unwatch(type_, this);
}

PyObject* LoadTypeAttrCache::invoke(
    LoadTypeAttrCache* cache,
    PyObject* obj,
    PyObject* name) {
  // The fast path is handled by direct memory access via valueAddr().
  return cache->invokeSlowPath(obj, name);
}

PyTypeObject** LoadTypeAttrCache::typeAddr() {
  return &type_;
}

PyObject** LoadTypeAttrCache::valueAddr() {
  return &value_;
}

// NB: This function needs to be kept in sync with PyType_Type.tp_getattro.
PyObject* LoadTypeAttrCache::invokeSlowPath(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> metatype{Py_TYPE(obj)};
  if (metatype->tp_getattro != PyType_Type.tp_getattro) {
    return PyObject_GetAttr(obj, name);
  }

  BorrowedRef<PyTypeObject> type{obj};
  if (!_PyType_IsReady(type) && PyType_Ready(type) < 0) {
    return nullptr;
  }

  descrgetfunc meta_get = nullptr;
  auto meta_attribute = Ref<>::create(_PyType_Lookup(metatype, name));
  if (meta_attribute != nullptr) {
    meta_get = Py_TYPE(meta_attribute)->tp_descr_get;
    if (meta_get != nullptr && PyDescr_IsData(meta_attribute)) {
      // Data descriptors implement tp_descr_set to intercept writes. Assume the
      // attribute is not overridden in type's tp_dict (and bases): call the
      // descriptor now.
      return meta_get(meta_attribute, type, metatype);
    }
  }

  // No data descriptor found on metatype. Look in tp_dict of this type and its
  // bases.
  auto attribute = Ref<>::create(_PyType_Lookup(type, name));
  if (attribute != nullptr) {
    // Implement descriptor functionality, if any.
    descrgetfunc local_get = Py_TYPE(attribute)->tp_descr_get;

    meta_attribute.reset();

    if (local_get != nullptr) {
      // nullptr 2nd argument indicates the descriptor was found on the target
      // object itself (or a base).
      return local_get(attribute, nullptr, type);
    }

    fill(type, attribute);
    return attribute.release();
  }

  // No attribute found in local __dict__ (or bases): use the descriptor from
  // the metatype, if any.
  if (meta_get != nullptr) {
    return meta_get(meta_attribute, type, metatype);
  }

  // If an ordinary attribute was found on the metatype, return it now.
  if (meta_attribute != nullptr) {
    return meta_attribute.release();
  }

  // Give up.
  PyErr_Format(
      PyExc_AttributeError,
      "type object '%.50s' has no attribute '%U'",
      type->tp_name,
      name);
  return nullptr;
}

void LoadTypeAttrCache::typeChanged(
    [[maybe_unused]] BorrowedRef<PyTypeObject> arg) {
  JIT_DCHECK(arg == type_, "Type watcher notified the wrong LoadTypeAttrCache");
  reset();
}

void LoadTypeAttrCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value) {
  if (!PyType_HasFeature(type, Py_TPFLAGS_VALID_VERSION_TAG)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  ltac_watcher.unwatch(type_, this);
  type_ = type;
  value_ = value;
  ltac_watcher.watch(type_, this);
}

void LoadTypeAttrCache::reset() {
  // We need to return a PyTypeObject* even in the empty case so that subsequent
  // refcounting operations work correctly.
  type_ = &s_empty_type_attr_cache;
  value_ = nullptr;
}

std::string_view kCacheMissReasons[] = {
#define NAME_REASON(reason) #reason,
    FOREACH_CACHE_MISS_REASON(NAME_REASON)
#undef NAME_REASON
};

std::string_view cacheMissReason(CacheMissReason reason) {
  return kCacheMissReasons[static_cast<size_t>(reason)];
}

LoadMethodCache::~LoadMethodCache() {
  for (auto& entry : entries_) {
    if (entry.type != nullptr) {
      lm_watcher.unwatch(entry.type, this);
      entry.type.reset();
      entry.value.reset();
    }
  }
}

LoadMethodResult LoadMethodCache::lookupHelper(
    LoadMethodCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

LoadMethodResult LoadMethodCache::lookup(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);

  for (auto& entry : entries_) {
    if (entry.type == tp) {
      PyObject* result = entry.value;
      Py_INCREF(result);
      Py_INCREF(obj);
      return {result, obj};
    }
  }

  return lookupSlowPath(obj, name);
}

void LoadMethodCache::typeChanged(PyTypeObject* type) {
  for (auto& entry : entries_) {
    if (entry.type == type) {
      entry.type.reset();
      entry.value.reset();
    }
  }
}

void LoadMethodCache::initCacheStats(
    const char* filename,
    const char* method_name) {
  cache_stats_ = std::make_unique<CacheStats>();
  cache_stats_->filename = filename;
  cache_stats_->method_name = method_name;
}

void LoadMethodCache::clearCacheStats() {
  cache_stats_->misses.clear();
}

const CacheStats* LoadMethodCache::cacheStats() {
  return cache_stats_.get();
}

LoadMethodResult __attribute__((noinline)) LoadMethodCache::lookupSlowPath(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  PyTypeObject* tp = Py_TYPE(obj);
  PyObject* descr;
  descrgetfunc f = nullptr;
  PyObject **dictptr, *dict;
  PyObject* attr;
  bool is_method = false;

  if ((tp->tp_getattro != PyObject_GenericGetAttr)) {
    PyObject* res = PyObject_GetAttr(obj, name);
    if (res != nullptr) {
      maybeCollectCacheStats(
          cache_stats_, tp, name, CacheMissReason::kWrongTpGetAttro);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
    return {nullptr, nullptr};
  } else if (_PyType_GetDict(tp) == nullptr && PyType_Ready(tp) < 0) {
    return {nullptr, nullptr};
  }

  descr = _PyType_Lookup(tp, name);
  if (descr != nullptr) {
    Py_INCREF(descr);
    if (PyFunction_Check(descr) || Py_TYPE(descr) == &PyMethodDescr_Type ||
        PyType_HasFeature(Py_TYPE(descr), Py_TPFLAGS_METHOD_DESCRIPTOR)) {
      is_method = true;
    } else {
      f = descr->ob_type->tp_descr_get;
      if (f != nullptr && PyDescr_IsData(descr)) {
        maybeCollectCacheStats(
            cache_stats_, tp, name, CacheMissReason::kPyDescrIsData);
        PyObject* result = f(descr, obj, (PyObject*)obj->ob_type);
        Py_DECREF(descr);
        Py_INCREF(Py_None);
        return {Py_None, result};
      }
    }
  }

  dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr != nullptr && (dict = *dictptr) != nullptr) {
    Py_INCREF(dict);
    attr = PyDict_GetItem(dict, name);
    if (attr != nullptr) {
      maybeCollectCacheStats(
          cache_stats_, tp, name, CacheMissReason::kUncategorized);
      Py_INCREF(attr);
      Py_DECREF(dict);
      Py_XDECREF(descr);
      Py_INCREF(Py_None);
      return {Py_None, attr};
    }
    Py_DECREF(dict);
  }

  if (is_method) {
    fill(tp, descr);
    Py_INCREF(obj);
    return {descr, obj};
  }

  if (f != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, tp, name, CacheMissReason::kUncategorized);
    PyObject* result = f(descr, obj, (PyObject*)Py_TYPE(obj));
    Py_DECREF(descr);
    Py_INCREF(Py_None);
    return {Py_None, result};
  }

  if (descr != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, tp, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, descr};
  }

  PyErr_Format(
      PyExc_AttributeError,
      "'%.50s' object has no attribute '%U'",
      tp->tp_name,
      name);
  return {nullptr, nullptr};
}

void LoadMethodCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value) {
  if (!PyType_HasFeature(type, Py_TPFLAGS_VALID_VERSION_TAG)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

#if PY_VERSION_HEX < 0x030C0000
  if (!PyType_HasFeature(type, Py_TPFLAGS_NO_SHADOWING_INSTANCES) &&
      (type->tp_dictoffset != 0)) {
    return;
  }
#else
  UPGRADE_NOTE(CHANGED_NO_SHADOWING_INSTANCES, T200294456)
  return;
#endif

  for (auto& entry : entries_) {
    if (entry.type == nullptr) {
      lm_watcher.watch(type, this);
      entry.type = type;
      entry.value = value;
      return;
    }
  }
}

LoadTypeMethodCache::~LoadTypeMethodCache() {
  if (type_ != nullptr) {
    ltm_watcher.unwatch(type_, this);
  }
}

LoadMethodResult LoadTypeMethodCache::lookupHelper(
    LoadTypeMethodCache* cache,
    PyTypeObject* obj,
    PyObject* name) {
  return cache->lookup(obj, name);
}

LoadMethodResult LoadTypeMethodCache::getValueHelper(
    LoadTypeMethodCache* cache,
    PyObject* obj) {
  PyObject* result = cache->value_;
  Py_INCREF(result);
  if (cache->is_unbound_meth_) {
    Py_INCREF(obj);
    return {result, obj};
  }
  Py_INCREF(Py_None);
  return {Py_None, result};
}

// This needs to be kept in sync with PyType_Type.tp_getattro.
LoadMethodResult LoadTypeMethodCache::lookup(
    BorrowedRef<PyTypeObject> obj,
    BorrowedRef<> name) {
  PyTypeObject* metatype = Py_TYPE(obj);
  if (metatype->tp_getattro != PyType_Type.tp_getattro) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kWrongTpGetAttro);
    PyObject* res = PyObject_GetAttr(obj, name);
    Py_INCREF(Py_None);
    return {Py_None, res};
  }
  if (_PyType_GetDict(obj) == nullptr) {
    if (PyType_Ready(obj) < 0) {
      return {nullptr, nullptr};
    }
  }

  descrgetfunc meta_get = nullptr;
  PyObject* meta_attribute = _PyType_Lookup(metatype, name);
  if (meta_attribute != nullptr) {
    Py_INCREF(meta_attribute);
    meta_get = Py_TYPE(meta_attribute)->tp_descr_get;

    if (meta_get != nullptr && PyDescr_IsData(meta_attribute)) {
      /* Data descriptors implement tp_descr_set to intercept
       * writes. Assume the attribute is not overridden in
       * type's tp_dict (and bases): call the descriptor now.
       */
      maybeCollectCacheStats(
          cache_stats_, metatype, name, CacheMissReason::kPyDescrIsData);
      PyObject* res =
          meta_get(meta_attribute, obj, reinterpret_cast<PyObject*>(metatype));
      Py_DECREF(meta_attribute);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
  }

  /* No data descriptor found on metatype. Look in tp_dict of this
   * type and its bases */
  PyObject* attribute = _PyType_Lookup(obj, name);
  if (attribute != nullptr) {
    Py_XDECREF(meta_attribute);
    BorrowedRef<PyTypeObject> attribute_type = Py_TYPE(attribute);
    if (attribute_type == &PyClassMethod_Type) {
      BorrowedRef<> cm_callable = Ci_PyClassMethod_GetFunc(attribute);
      if (Py_TYPE(cm_callable) == &PyFunction_Type) {
        Py_INCREF(obj);
        Py_INCREF(cm_callable);

        // Get the underlying callable from classmethod and return the
        // callable alongside the class object, allowing the runtime to call
        // the method as an unbound method.
        fill(obj, cm_callable, true);
        return {cm_callable, obj};
      } else if (Py_TYPE(cm_callable)->tp_descr_get != nullptr) {
        // cm_callable has custom tp_descr_get that can run arbitrary
        // user code. Do not cache in this instance.
        maybeCollectCacheStats(
            cache_stats_, metatype, name, CacheMissReason::kUncategorized);
        Py_INCREF(Py_None);
        return {
            Py_None, Py_TYPE(cm_callable)->tp_descr_get(cm_callable, obj, obj)};
      } else {
        // It is not safe to cache custom objects decorated with classmethod
        // as they can be modified later
        maybeCollectCacheStats(
            cache_stats_, metatype, name, CacheMissReason::kUncategorized);
        BorrowedRef<> py_meth = PyMethod_New(cm_callable, obj);
        Py_INCREF(Py_None);
        return {Py_None, py_meth};
      }
    }
    if (attribute_type == &PyStaticMethod_Type) {
      BorrowedRef<> cm_callable = Ci_PyStaticMethod_GetFunc(attribute);
      Py_INCREF(cm_callable);
      Py_INCREF(Py_None);
      fill(obj, cm_callable, false);
      return {Py_None, cm_callable};
    }
    if (PyFunction_Check(attribute)) {
      Py_INCREF(attribute);
      Py_INCREF(Py_None);
      fill(obj, attribute, false);
      return {Py_None, attribute};
    }
    Py_INCREF(attribute);
    /* Implement descriptor functionality, if any */
    descrgetfunc local_get = Py_TYPE(attribute)->tp_descr_get;
    if (local_get != nullptr) {
      /* nullptr 2nd argument indicates the descriptor was
       * found on the target object itself (or a base)  */
      maybeCollectCacheStats(
          cache_stats_, metatype, name, CacheMissReason::kUncategorized);
      PyObject* res = local_get(attribute, nullptr, obj);
      Py_DECREF(attribute);
      Py_INCREF(Py_None);
      return {Py_None, res};
    }
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, attribute};
  }

  /* No attribute found in local __dict__ (or bases): use the
   * descriptor from the metatype, if any */
  if (meta_get != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    PyObject* res;
    res = meta_get(meta_attribute, obj, reinterpret_cast<PyObject*>(metatype));
    Py_DECREF(meta_attribute);
    Py_INCREF(Py_None);
    return {Py_None, res};
  }

  /* If an ordinary attribute was found on the metatype, return it now */
  if (meta_attribute != nullptr) {
    maybeCollectCacheStats(
        cache_stats_, metatype, name, CacheMissReason::kUncategorized);
    Py_INCREF(Py_None);
    return {Py_None, meta_attribute};
  }

  /* Give up */
  PyErr_Format(
      PyExc_AttributeError,
      "type object '%.50s' has no attribute '%U'",
      obj->tp_name,
      name);
  return {nullptr, nullptr};
}

PyTypeObject** LoadTypeMethodCache::typeAddr() {
  return &type_;
}

BorrowedRef<> LoadTypeMethodCache::value() {
  return value_;
}

void LoadTypeMethodCache::typeChanged(BorrowedRef<PyTypeObject> /* type */) {
  type_ = nullptr;
  value_.reset();
}

void LoadTypeMethodCache::initCacheStats(
    const char* filename,
    const char* method_name) {
  cache_stats_ = std::make_unique<CacheStats>();
  cache_stats_->filename = filename;
  cache_stats_->method_name = method_name;
}

void LoadTypeMethodCache::clearCacheStats() {
  cache_stats_->misses.clear();
}

const CacheStats* LoadTypeMethodCache::cacheStats() {
  return cache_stats_.get();
}

void LoadTypeMethodCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value,
    bool is_unbound_meth) {
  if (!PyType_HasFeature(type, Py_TPFLAGS_VALID_VERSION_TAG)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

#if PY_VERSION_HEX < 0x030C0000
  if (!PyType_HasFeature(type, Py_TPFLAGS_NO_SHADOWING_INSTANCES) &&
      type->tp_dictoffset != 0) {
    return;
  }
#else
  UPGRADE_NOTE(CHANGED_NO_SHADOWING_INSTANCES, T200294456)
  return;
#endif

  ltm_watcher.unwatch(type_, this);
  type_ = type;
  value_ = value;
  is_unbound_meth_ = is_unbound_meth;
  ltm_watcher.watch(type_, this);
}

LoadMethodResult LoadModuleMethodCache::lookupHelper(
    LoadModuleMethodCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

LoadMethodResult LoadModuleMethodCache::lookup(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  if (module_obj_ == obj && value_ != nullptr) {
    uint64_t version = 0;
    if (PyModule_Check(obj)) {
      BorrowedRef<PyModuleObject> mod{obj};
      version = getModuleVersion(mod);
    } else if (Ci_StrictModule_Check(obj)) {
      BorrowedRef<Ci_StrictModuleObject> mod{obj};
      version = getModuleVersion(mod);
    }
    if (module_version_ == version) {
      Py_INCREF(Py_None);
      Py_INCREF(value_);
      return {Py_None, value_};
    }
  }
  return lookupSlowPath(obj, name);
}

BorrowedRef<> LoadModuleMethodCache::moduleObj() {
  return module_obj_;
}

BorrowedRef<> LoadModuleMethodCache::value() {
  return value_;
}

LoadMethodResult __attribute__((noinline))
LoadModuleMethodCache::lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);
  uint64_t dict_version = 0;
  BorrowedRef<> res = nullptr;
  if (PyModule_Check(obj) && tp->tp_getattro == PyModule_Type.tp_getattro) {
    if (_PyType_Lookup(tp, name) == nullptr) {
      BorrowedRef<PyModuleObject> mod{obj};
      BorrowedRef<> dict = mod->md_dict;
      if (dict) {
        dict_version = getModuleVersion(mod);
        res = PyDict_GetItemWithError(dict, name);
      }
    }
  } else if (
      Ci_StrictModule_Check(obj) &&
      tp->tp_getattro == Ci_StrictModule_Type.tp_getattro) {
    if (_PyType_Lookup(tp, name) == nullptr) {
      BorrowedRef<Ci_StrictModuleObject> mod{obj};
      BorrowedRef<> dict = mod->globals;
      if (dict && Ci_strictmodule_is_unassigned(dict, name) == 0) {
        dict_version = getModuleVersion(mod);
        res = PyDict_GetItemWithError(dict, name);
      }
    }
  }
  if (res != nullptr) {
    if (PyFunction_Check(res) || PyCFunction_Check(res) ||
        Py_TYPE(res) == &PyMethodDescr_Type) {
      fill(obj, res, dict_version);
    }
    Py_INCREF(Py_None);
    // PyDict_GetItemWithError returns a borrowed reference, so
    // we need to increment it before returning.
    Py_INCREF(res);
    return {Py_None, res};
  }
  auto generic_res = Ref<>::steal(PyObject_GetAttr(obj, name));
  if (generic_res != nullptr) {
    return {Py_None, generic_res.release()};
  }
  return {nullptr, nullptr};
}

void LoadModuleMethodCache::fill(
    BorrowedRef<> obj,
    BorrowedRef<> value,
    uint64_t version) {
  module_obj_ = obj;
  value_ = value;
  module_version_ = version;
}

void notifyICsTypeChanged(BorrowedRef<PyTypeObject> type) {
  ac_watcher.typeChanged(type);
  ltac_watcher.typeChanged(type);
  lm_watcher.typeChanged(type);
  ltm_watcher.typeChanged(type);
}

} // namespace jit

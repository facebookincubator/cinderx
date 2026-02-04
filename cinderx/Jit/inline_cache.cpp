// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/inline_cache.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/func.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/type.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

#include <algorithm>
#include <memory>

namespace jit {

namespace {

template <class T>
struct TypeWatcher {
  jit::UnorderedMap<BorrowedRef<PyTypeObject>, jit::UnorderedSet<T*>> caches;

  void watch(BorrowedRef<PyTypeObject> type, T* cache) {
    JIT_CHECK(
        cinderx::getModuleState()->watcherState().watchType(type) == 0,
        "Failed to watch type {} for attribute cache",
        type->tp_name);
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
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
PyTypeObject s_empty_type_attr_cache = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "EmptyLoadTypeAttrCache",
};
#pragma clang diagnostic pop

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

ci_dict_version_tag_t getModuleVersion(BorrowedRef<PyModuleObject> mod) {
  if (mod->md_dict) {
    BorrowedRef<PyDictObject> md_dict = mod->md_dict;
    return Ci_DictVersionTag(md_dict.get());
  }
  return 0;
}

ci_dict_version_tag_t getModuleVersion(BorrowedRef<Ci_StrictModuleObject> mod) {
  if (mod->globals) {
    BorrowedRef<PyDictObject> globals = mod->globals;
    return Ci_DictVersionTag(globals.get());
  }
  return 0;
}

ci_dict_version_tag_t getModuleVersion(BorrowedRef<> obj) {
  if (PyModule_Check(obj)) {
    BorrowedRef<PyModuleObject> mod{obj};
    return getModuleVersion(mod);
  } else if (Ci_StrictModule_Check(obj)) {
    BorrowedRef<Ci_StrictModuleObject> mod{obj};
    return getModuleVersion(mod);
  } else {
    return 0;
  }
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

void AttributeMutator::changeKindFromSplitInline(
    SplitMutator* split,
    Kind new_kind) {
  AttributeMutator* mutator = reinterpret_cast<AttributeMutator*>(
      reinterpret_cast<uintptr_t>(split) - offsetof(AttributeMutator, split_));
  mutator->type_ = reinterpret_cast<uintptr_t>(mutator->type()) |
      static_cast<uintptr_t>(new_kind);
}

PyDictKeysObject* getSplitKeys(BorrowedRef<PyTypeObject> type) {
  assert(PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE));
  PyHeapTypeObject* ht = reinterpret_cast<PyHeapTypeObject*>(type.get());
  return ht->ht_cached_keys;
}

bool SplitMutator::canInsertToSplitDict(
    BorrowedRef<PyDictObject> dict,
    BorrowedRef<> name) {
  if (dict->ma_keys != keys) {
    return false;
  }
#if PY_VERSION_HEX >= 0x030C0000
  // In 3.12 we can insert in any order, we just need to update the insertion
  // order
  return (
      val_offset != -1 || (val_offset = getDictKeysIndex(keys, name)) != -1);
#else
  return (
      (dict->ma_used == val_offset) ||
      (DICT_VALUES(dict.get())[val_offset] != nullptr));
#endif
}

bool SplitMutator::ensureValueOffset(BorrowedRef<> name) {
#if PY_VERSION_HEX >= 0x030C0000
  if (val_offset == -1) {
    val_offset = getDictKeysIndex(keys, name);
    if (val_offset == -1) {
      return false;
    }
  }
#else
  JIT_DCHECK(
      val_offset != -1,
      "Value offset not set for {} on split dict instance",
      repr(name));
#endif
  return true;
}

#if PY_VERSION_HEX >= 0x030E0000
PyObject* SplitMutator::getAttrInline(PyObject* obj, PyObject* name) {
  if (!ensureValueOffset(name)) {
    return PyObject_GetAttr(obj, name);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitInlineKnownOffset);
  return getAttrInlineKnownOffset(obj, name);
}

PyObject* SplitMutator::getAttrInlineKnownOffset(
    PyObject* obj,
    PyObject* name) {
  PyDictValues* values = _PyObject_InlineValues(obj);
  if (!values->valid) {
    // Downgrade to the slightly slower path in future
    AttributeMutator::changeKindFromSplitInline(
        this, AttributeMutator::Kind::kSplitKnownOffset);
    return getAttr(obj, name);
  }
  PyObject* result = values->values[val_offset];
  if (result == nullptr) {
    return raise_attribute_error(obj, name);
  }
  return Py_NewRef(result);
}

PyObject* SplitMutator::getAttrSlowPath(
    PyObject* obj,
    PyObject* name,
    BorrowedRef<PyDictObject> dict) {
  PyObject* attr_o;
  int res = [&] {
    auto strong_ref = Ref<>::create(dict);
    return PyDict_GetItemRef(dict, name, &attr_o);
  }();
  if (res == 0) {
    return raise_attribute_error(obj, name);
  }
  if (res == -1) {
    return nullptr;
  }
  return attr_o;
}

PyObject* SplitMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);

  JIT_DCHECK(
      PyDict_Check(dict), "Expected dict, got {}", Py_TYPE(dict)->tp_name);

  if (dict == nullptr) {
    return PyObject_GetAttr(obj, name);
  }
  if (!ensureValueOffset(name)) {
    return getAttrSlowPath(obj, name, dict);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitKnownOffset);
  return getAttrKnownOffset(obj, name);
}

PyObject* SplitMutator::getAttrKnownOffset(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);

  JIT_DCHECK(
      PyDict_Check(dict), "Expected dict, got {}", Py_TYPE(dict)->tp_name);

  if (dict == nullptr) {
    return PyObject_GetAttr(obj, name);
  }
  if (dict->ma_keys != keys) {
    return getAttrSlowPath(obj, name, dict);
  }
  JIT_DCHECK(
      DK_IS_UNICODE(keys) && val_offset < keys->dk_nentries,
      "Expected dictionary keys object to change");
  PyObject* attr_o = dict->ma_values->values[val_offset];
  if (attr_o == nullptr) {
    return raise_attribute_error(obj, name);
  }
  return Py_NewRef(attr_o);
}

int SplitMutator::setAttrInline(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  if (!ensureValueOffset(name)) {
    return PyObject_SetAttr(obj, name, value);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitInlineKnownOffset);
  return setAttrInlineKnownOffset(obj, name, value);
}

int SplitMutator::setAttrInlineKnownOffset(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  PyDictValues* values = _PyObject_InlineValues(obj);
  PyDictObject* dict = _PyObject_GetManagedDict(obj);
  if (!values->valid || dict) {
    // Downgrade to the slightly slower path in future
    AttributeMutator::changeKindFromSplitInline(
        this, AttributeMutator::Kind::kSplitKnownOffset);
    return setAttr(obj, name, value);
  }
  auto old_value = Ref<>::steal(values->values[val_offset]);
  values->values[val_offset] = Py_NewRef(value);
  if (!old_value) {
    _PyDictValues_AddToInsertionOrder(values, val_offset);
  }
  return 0;
}

int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  if (!ensureValueOffset(name)) {
    return PyObject_SetAttr(obj, name, value);
  }
  AttributeMutator::changeKindFromSplitInline(
      this, AttributeMutator::Kind::kSplitKnownOffset);
  return setAttrKnownOffset(obj, name, value);
}

int SplitMutator::setAttrKnownOffset(
    PyObject* obj,
    PyObject* name,
    PyObject* value) {
  BorrowedRef<PyDictObject> dict = _PyObject_GetManagedDict(obj);
  if (dict == nullptr) {
    return PyObject_SetAttr(obj, name, value);
  }
  if (keys != dict->ma_keys) {
    // Slow path
    auto strong_ref = Ref<>::create(dict);
    return PyDict_SetItem(dict, name, value);
  }
  Cix_dict_insert_split_value(
      _PyInterpreterState_GET(), dict, name, value, val_offset);
  return 0;
}

#else

int SplitMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
#if PY_VERSION_HEX >= 0x030C0000
  PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
  if (_PyDictOrValues_IsValues(dorv)) {
    // Values are stored in a values array not attached to a dictionary.
    if (!ensureValueOffset(name)) {
      return PyObject_SetAttr(obj, name, value);
    }

    PyDictValues* values = _PyDictOrValues_GetValues(dorv);
    PyObject* old_value = values->values[val_offset];
    values->values[val_offset] = value;
    Py_INCREF(value);
    if (old_value == nullptr) {
      _PyDictValues_AddToInsertionOrder(values, val_offset);
    } else {
      Py_DECREF(old_value);
    }
    return 0;
  }

  // Dictionary has been materialized but may still be using shared keys.
  BorrowedRef<PyDictObject> dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
  if (dict == nullptr) {
    dict =
        reinterpret_cast<PyDictObject*>(PyObject_GenericGetDict(obj, nullptr));
    if (dict == nullptr) {
      return -1;
    }
    Py_DECREF(dict);
  }
#else
  BorrowedRef<PyDictObject> dict = get_or_allocate_dict(obj, dict_offset);
#endif

  if (dict == nullptr) {
    return -1;
  }

  if (canInsertToSplitDict(dict, name)) {
    PyObject* old_value = DICT_VALUES(dict.get())[val_offset];
#if PY_VERSION_HEX >= 0x030C0000
    if (old_value == nullptr) {
      // Track insertion order on 3.12.
      _PyDictValues_AddToInsertionOrder(dict->ma_values, val_offset);
    }
#endif
    if (!_PyObject_GC_IS_TRACKED(dict.getObj())) {
      if (_PyObject_GC_MAY_BE_TRACKED(value)) {
        PyObject_GC_Track(dict.getObj());
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
#if PY_VERSION_HEX >= 0x030C0000
  PyDictOrValues dorv = *_PyObject_DictOrValuesPointer(obj);
  if (_PyDictOrValues_IsValues(dorv)) {
    if (!ensureValueOffset(name)) {
      return raise_attribute_error(obj, name);
    }
    // Values are stored in values w/o materialized dictionary
    PyDictValues* values = _PyDictOrValues_GetValues(dorv);
    PyObject* result = values->values[val_offset];
    if (result == nullptr) {
      return raise_attribute_error(obj, name);
    }
    Py_INCREF(result);
    return result;
  }

  PyDictObject* dict = (PyDictObject*)_PyDictOrValues_GetDict(dorv);
#else
  PyDictObject* dict = get_dict(obj, dict_offset);
#endif
  if (dict == nullptr) {
    return raise_attribute_error(obj, name);
  }
  PyObject* result = nullptr;
  if (dict->ma_keys == keys) {
    if (!ensureValueOffset(name)) {
      return raise_attribute_error(obj, name);
    }
    // We are still sharing keys with the inline object.
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
#endif // PY_VERSION_HEX < 0x030E0000

int CombinedMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  BorrowedRef<PyDictObject> dict = get_or_allocate_dict(obj, dict_offset);
  if (dict == nullptr) {
    return -1;
  }
  auto strong_ref = Ref<>::create(dict);
  return PyDict_SetItem(dict, name, value);
}

PyObject* CombinedMutator::getAttr(PyObject* obj, PyObject* name) {
  BorrowedRef<PyDictObject> dict = get_dict(obj, dict_offset);

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
  int st = Cix_PyObjectDict_SetItem(type, obj, dictptr, name, value);
  if (st < 0 && PyErr_ExceptionMatches(PyExc_KeyError)) {
    PyErr_SetObject(PyExc_AttributeError, name);
  }
#if PY_VERSION_HEX < 0x030C0000
  if (PyType_HasFeature(type, Py_TPFLAGS_NO_SHADOWING_INSTANCES)) {
    _PyType_ClearNoShadowingInstances(type, descr);
  }
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
#if PY_VERSION_HEX >= 0x030C0000
    if (keys_version == 0 ||
        reinterpret_cast<PyDictObject*>(dict.get())->ma_keys->dk_version !=
            keys_version) {
#endif
      auto res = Ref<>::create(PyDict_GetItem(dict, name));
      if (res != nullptr) {
        return res.release();
      }
#if PY_VERSION_HEX >= 0x030C0000
    }
#endif
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
  type_ = 0;
}

bool AttributeMutator::isEmpty() const {
  return type_ == 0;
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
    PyObject* descr,
    uint keys_version) {
  set_type(type, Kind::kDescrOrClassVar);
  descr_or_cvar_.descr = descr;
  descr_or_cvar_.keys_version = keys_version;
}

void AttributeMutator::set_split(
    PyTypeObject* type,
    Py_ssize_t val_offset,
    [[maybe_unused]] PyDictKeysObject* keys,
    bool inline_values) {
  set_type(type, inline_values ? Kind::kSplitInline : Kind::kSplit);
#if PY_VERSION_HEX >= 0x030C0000
  split_.val_offset = val_offset;
  split_.keys = keys;
#else
  JIT_CHECK(
      type->tp_dictoffset <= std::numeric_limits<uint32_t>::max(),
      "Dict offset does not fit into a 32-bit int");
  split_.dict_offset = static_cast<uint32_t>(type->tp_dictoffset);
  JIT_CHECK(
      val_offset <= std::numeric_limits<int32_t>::max(),
      "Val offset does not fit into a 32-bit int");
  split_.val_offset = static_cast<int32_t>(val_offset);
#endif
}

inline int
AttributeMutator::setAttr(PyObject* obj, PyObject* name, PyObject* value) {
  JIT_CHECK(
      !isEmpty(),
      "Empty attribute mutator setting field {} on object of type {}",
      repr(name),
      Py_TYPE(obj)->tp_name);
  AttributeMutator::Kind kind = get_kind();
  switch (kind) {
    case AttributeMutator::Kind::kSplit:
      return split_.setAttr(obj, name, value);
#if PY_VERSION_HEX >= 0x030E0000
    case AttributeMutator::Kind::kSplitKnownOffset:
      return split_.setAttrKnownOffset(obj, name, value);
    case AttributeMutator::Kind::kSplitInline:
      return split_.setAttrInline(obj, name, value);
    case AttributeMutator::Kind::kSplitInlineKnownOffset:
      return split_.setAttrInlineKnownOffset(obj, name, value);
#endif
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
  JIT_CHECK(
      !isEmpty(),
      "Empty attribute mutator getting field {} on object of type {}",
      repr(name),
      Py_TYPE(obj)->tp_name);
  AttributeMutator::Kind kind = get_kind();
  switch (kind) {
    case AttributeMutator::Kind::kSplit:
      return split_.getAttr(obj, name);
#if PY_VERSION_HEX >= 0x030E0000
    case AttributeMutator::Kind::kSplitKnownOffset:
      return split_.getAttrKnownOffset(obj, name);
    case AttributeMutator::Kind::kSplitInline:
      return split_.getAttrInline(obj, name);
    case AttributeMutator::Kind::kSplitInlineKnownOffset:
      return split_.getAttrInlineKnownOffset(obj, name);
#endif
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

bool canCacheType(PyTypeObject* type) {
#if PY_VERSION_HEX >= 0x030C0000
  if (PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    // We can cache values for types which have managed dictionaries on 3.12 or
    // later.
    return true;
  }
#endif

  // We only support the common case for objects - fixed-size instances
  // (tp_dictoffset >= 0) of heap types (Py_TPFLAGS_HEAPTYPE).
  return type->tp_dictoffset >= 0 &&
      PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE);
}

bool canCacheAttribute(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name,
    uint& keys_version) {
#if PY_VERSION_HEX < 0x030C0000
  if (!PyType_HasFeature(type, Py_TPFLAGS_NO_SHADOWING_INSTANCES) &&
      (type->tp_dictoffset != 0)) {
    return false;
  }
#else
  if (type->tp_dictoffset == 0) {
    return true;
  }

  if (!PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE)) {
    return false;
  }

  PyDictKeysObject* keys = getSplitKeys(type);
  if (keys == nullptr) {
    return false;
  }

  // If we don't have a valid keys version or the key exists in the shared
  // keys then we can't cache the value as we need to check and see if it's
  // been overridden.
  if (dictGetKeysVersion(PyInterpreterState_Get(), keys) == 0 ||
      getDictKeysIndex(keys, name) != -1) {
    return false;
  }
  keys_version = keys->dk_version;
#endif
  return true;
}

void AttributeCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name,
    BorrowedRef<> descr) {
  if (!Ci_Type_HasValidVersionTag(type)) {
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
    // Not yet working.
    if (PY_VERSION_HEX >= 0x030E0000) {
      return;
    }
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
      uint keys_version = 0;
#if PY_VERSION_HEX >= 0x030C0000
      canCacheAttribute(type, name, keys_version);
#endif
      mut->set_descr_or_classvar(type, descr, keys_version);
    }
    ac_watcher.watch(type, this);
    return;
  }

  // Not working yet.
  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    return;
  }

  if (!canCacheType(type)) {
    return;
  }

  // Instance attribute with no shadowing. Specialize the lookup based on
  // whether or not the type is using split dictionaries.
  PyDictKeysObject* keys = getSplitKeys(type);
#if PY_VERSION_HEX >= 0x030C0000
  if (PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    JIT_DCHECK(keys != nullptr, "Managed dict should have a split dict");
    bool inline_values = false;
#if PY_VERSION_HEX >= 0x030E0000
    inline_values = type->tp_flags & Py_TPFLAGS_INLINE_VALUES;
#endif
    mut->set_split(type, getDictKeysIndex(keys, name), keys, inline_values);
#else
  Py_ssize_t val_offset;
  if (keys != nullptr && (val_offset = getDictKeysIndex(keys, name)) != -1) {
    mut->set_split(type, val_offset, keys, false);
#endif
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
  if (PyType_Ready(type) < 0) {
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

    bool is_cachable = local_get == nullptr;
    if constexpr (PY_VERSION_HEX >= 0x030C0000) {
      if (PyFunction_Check(attribute)) {
        // Loading a function from a type returns the type
        is_cachable = true;
      } else if (Py_TYPE(attribute) == &PyStaticMethod_Type) {
        // static method returns the underlying object
        attribute = Ref<>::create(Ci_PyStaticMethod_GetFunc(attribute));
        is_cachable = true;
      }
    }
    if (!is_cachable) {
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
  raise_attribute_error(obj, name);
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
  if (!Ci_Type_HasValidVersionTag(type)) {
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

#if PY_VERSION_HEX >= 0x030C0000
// Checks to see if the cached keys version allows a lookup w/o looking in
// the dictionary. This could be either that we have a match of the keys version
// or that we a non-heap type w/ no dictionary.
bool isValidKeysVersion(uint keys_version, BorrowedRef<> obj) {
  if (keys_version == 0) {
    // 0 is an invalid keys version and a sentinel value that we'll never
    // generate a a cache for a heap type with. We may have a non-heap type
    // that is cached w/ a keys_version of 0 that has no dictionary in which
    // case the cache is always valid.
    return true;
  }
  PyObject** dictptr = _PyObject_GetDictPtr(obj);
  assert(dictptr != nullptr);

  PyDictObject* dict = reinterpret_cast<PyDictObject*>(*dictptr);
  if (dict == nullptr) {
    return true;
  }

  return dict->ma_keys->dk_version == keys_version;
}
#endif

LoadMethodResult LoadMethodCache::lookup(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);

  for (auto& entry : entries_) {
    if (entry.type == tp) {
#if PY_VERSION_HEX >= 0x030C0000
      if (!isValidKeysVersion(entry.keys_version, obj)) {
        continue;
      }
#endif

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
    fill(tp, descr, name);
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

  raise_attribute_error(obj, name);
  return {nullptr, nullptr};
}

void LoadMethodCache::fill(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> value,
    BorrowedRef<> name) {
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  for (auto& entry : entries_) {
    if (entry.type == nullptr) {
      uint keys_version = 0;
      if (!canCacheAttribute(type, name, keys_version)) {
        break;
      }

      lm_watcher.watch(type, this);
      entry.type = type;
      entry.value = value;
#if PY_VERSION_HEX >= 0x030C0000
      entry.keys_version = keys_version;
#endif
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

  raise_attribute_error(obj, name);
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
  if (!Ci_Type_HasValidVersionTag(type)) {
    // The type must have a valid version tag in order for us to be able to
    // invalidate the cache when the type is modified. See the comment at
    // the top of `PyType_Modified` for more details.
    return;
  }

  ltm_watcher.unwatch(type_, this);
  type_ = type;
  value_ = value;
  is_unbound_meth_ = is_unbound_meth;
  ltm_watcher.watch(type_, this);
}

PyObject* LoadModuleAttrCache::lookupHelper(
    LoadModuleAttrCache* cache,
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  return cache->lookup(obj, name);
}

static BorrowedRef<PyDictObject> getModuleDict(BorrowedRef<> obj) {
  if (PyModule_Check(obj)) {
    BorrowedRef<PyModuleObject> mod{obj};
    return mod->md_dict;
  } else if (Ci_StrictModule_Check(obj)) {
    BorrowedRef<Ci_StrictModuleObject> mod{obj};
    return mod->globals;
  }
  return nullptr;
}

PyObject* LoadModuleAttrCache::lookup(
    BorrowedRef<> object,
    BorrowedRef<> name) {
  // First, check if we can use the cached value. If we can, we will return a
  // new reference to it.
#if PY_VERSION_HEX >= 0x030E0000
  if (module_ == object && cache_ != nullptr) {
    BorrowedRef<> res = *cache_;
    if (res != nullptr) {
      return Py_NewRef(res);
    }
  }
#else
  if (module_ == object && value_ != nullptr &&
      version_ == getModuleVersion(object)) {
    return Py_NewRef(value_);
  }
#endif

  // Otherwise, we will fall back to the slow path.
  return lookupSlowPath(object, name);
}

static std::pair<ci_dict_version_tag_t, PyObject*> getModuleAttribute(
    BorrowedRef<> obj,
    BorrowedRef<> name) {
  BorrowedRef<PyTypeObject> tp = Py_TYPE(obj);
  BorrowedRef<PyDictObject> dict = getModuleDict(obj);

  if (dict != nullptr &&
      (tp->tp_getattro == PyModule_Type.tp_getattro ||
       tp->tp_getattro == Ci_StrictModule_Type.tp_getattro) &&
      _PyType_Lookup(tp, name) == nullptr) {
    return {Ci_DictVersionTag(dict), PyDict_GetItemWithError(dict, name)};
  }

  return {0, nullptr};
}

PyObject* __attribute__((noinline)) LoadModuleAttrCache::lookupSlowPath(
    BorrowedRef<> object,
    BorrowedRef<> name) {
  auto [version, value] = getModuleAttribute(object, name);

  if (value != nullptr) {
#if PY_VERSION_HEX >= 0x030E0000
    PyObject* dict = getModuleDict(object);
    BorrowedRef<PyUnicodeObject> uname{name};
    if (hasOnlyUnicodeKeys(dict)) {
      cache_ = cinderx::getModuleState()->cacheManager()->getGlobalCache(
          dict, dict, uname);
    }
#else
    value_ = value;
    version_ = version;
#endif
    module_ = object;

    // PyDict_GetItemWithError returns a borrowed reference, so
    // we need to increment it before returning.
    return Py_NewRef(value);
  }

  auto generic = Ref<>::steal(PyObject_GetAttr(object, name));
  return generic == nullptr ? nullptr : generic.release();
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
#if PY_VERSION_HEX >= 0x030E0000
  if (module_obj_ == obj && cache_ != nullptr) {
    BorrowedRef<> res = *cache_;
    if (res != nullptr) {
      return {Py_None, Py_NewRef(res)};
    }
  }
#else
  BorrowedRef<PyDictObject> dict = getModuleDict(obj);
  ci_dict_version_tag_t version = Ci_DictVersionTag(dict);

  if (module_obj_ == obj && value_ != nullptr && module_version_ == version) {
    return {Py_None, Py_NewRef(value_)};
  }
#endif

  return lookupSlowPath(obj, name);
}

BorrowedRef<> LoadModuleMethodCache::moduleObj() {
  return module_obj_;
}

#if PY_VERSION_HEX < 0x030E0000
BorrowedRef<> LoadModuleMethodCache::value() {
  return value_;
}
#endif

LoadMethodResult __attribute__((noinline))
LoadModuleMethodCache::lookupSlowPath(BorrowedRef<> obj, BorrowedRef<> name) {
  auto [version, res] = getModuleAttribute(obj, name);

  if (res != nullptr) {
    if (PyFunction_Check(res) || PyCFunction_Check(res) ||
        Py_TYPE(res) == &PyMethodDescr_Type) {
      module_obj_ = obj;
#if PY_VERSION_HEX >= 0x030E0000
      BorrowedRef<PyUnicodeObject> uname{name};
      cache_ = cinderx::getModuleState()->cacheManager()->getGlobalCache(
          getModuleDict(obj), getModuleDict(obj), uname);
#else
      module_version_ = version;
      value_ = res;
#endif
    }
    // PyDict_GetItemWithError returns a borrowed reference, so
    // we need to increment it before returning.
    return {Py_None, Py_NewRef(res)};
  }
  auto generic_res = Ref<>::steal(PyObject_GetAttr(obj, name));
  if (generic_res != nullptr) {
    return {Py_None, generic_res.release()};
  }
  return {nullptr, nullptr};
}

void notifyICsTypeChanged(BorrowedRef<PyTypeObject> type) {
  ac_watcher.typeChanged(type);
  ltac_watcher.typeChanged(type);
  lm_watcher.typeChanged(type);
  ltm_watcher.typeChanged(type);
}

} // namespace jit

// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/load_method_elimination.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/type.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/Jit/type_deopt_patchers.h"
#include "cinderx/module_state.h"

namespace cinderx::jit::hir {

namespace {

struct MethodInvoke {
  LoadMethodBase* load_method{nullptr};
  GetSecondOutput* get_instance{nullptr};
  CallMethod* call_method{nullptr};
};

struct MethodTarget {
  BorrowedRef<> callable;
  bool is_static_python_invoke{false};
  bool needs_type_patcher{false};
  bool needs_instance_dict_guard{false};
};

enum class InstanceDictCheck {
  // We can't eliminate the method load
  kUnsupported,
  // We don't need to check the dictionary (it doesn't exist)
  kNotNeeded,
  // We need a runtime check on the dictionary to make sure it's the
  // right inline dict.
  kRuntime,
};

InstanceDictCheck instanceDictCheck(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name) {
  JIT_DCHECK(
      ThreadedCompileContext::canAccessSharedData(),
      "shared keys require the GIL during compilation");
  if (type->tp_dictoffset == 0 &&
      !PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    // Could be a type w/ __slots__
    return InstanceDictCheck::kNotNeeded;
  }

#if defined(ENABLE_SHARED_KEYS_TYPE_MODIFIED) || PY_VERSION_HEX >= 0x03100000
  // These builds notify type watchers when shared keys change, so the type
  // patchpoint below protects this compile-time absence check.
  if (!PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE)) {
    return InstanceDictCheck::kUnsupported;
  }
#if PY_VERSION_HEX >= 0x030E0000
  if (!PyType_HasFeature(type, Py_TPFLAGS_INLINE_VALUES)) {
    return InstanceDictCheck::kUnsupported;
  }
#else
  if (!PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    return InstanceDictCheck::kUnsupported;
  }
#endif

  BorrowedRef<PyHeapTypeObject> heap_type{type};
  PyDictKeysObject* keys = heap_type->ht_cached_keys;
  if (keys == nullptr || keys->dk_nentries == SHARED_KEYS_MAX_SIZE) {
    return InstanceDictCheck::kUnsupported;
  }
  if (getDictKeysIndex(keys, name) == -1) {
    return InstanceDictCheck::kRuntime;
  }
#endif
  return InstanceDictCheck::kUnsupported;
}

// Gets a directly invokable method object from a JIT Type. This only succeeds
// if we know the type can be directly invoked.
MethodTarget getMethodObjectFromType(
    Function& irfunc,
    Type receiver_type,
    BorrowedRef<> name) {
  if (!receiver_type.hasTypeExactSpec()) {
    return {};
  }
  PyTypeObject* type = receiver_type.runtimePyType();
  if (type == nullptr) {
    // This might happen for a variety of reasons, such as encountering a
    // method load on a maybe-defined value where the definition occurs in a
    // block of code that isn't seen by the compiler (e.g. in an except
    // block).
    JIT_DCHECK(
        receiver_type == TBottom,
        "Type {} expected to have PyTypeObject*",
        receiver_type);
    return {};
  }

  BorrowedRef<> method_obj = nullptr;
  // Methods on these builtin types cannot be overwritten from managed code,
  // and their lookup cannot invoke arbitrary Python.
  if (PyType_HasFeature(type, Py_TPFLAGS_IMMUTABLETYPE) &&
      PyType_CheckExact(type) && type->tp_dictoffset == 0) {
    method_obj = typeLookupSafe(type, name);
    if (method_obj != nullptr &&
        Py_TYPE(method_obj) != &PyClassMethodDescr_Type &&
        Py_TYPE(method_obj) != &PyMethodDescr_Type &&
        Py_TYPE(method_obj) != &PyWrapperDescr_Type &&
        Py_TYPE(method_obj) != &PyFunction_Type) {
      method_obj = nullptr;
    }
    return {method_obj, method_obj != nullptr, false, false};
  }

  if constexpr (kFreeThreadedBuild) {
    return {};
  }
  if (ThreadedCompileContext::compileRunning()) {
    return {};
  }

  ThreadedCompileGILHolder gil;
  method_obj = typeLookupSafe(type, name);

  InstanceDictCheck dict_check = instanceDictCheck(type, name);
  if (!PyType_HasFeature(type, Py_TPFLAGS_READY) ||
      type->tp_getattro != PyObject_GenericGetAttr ||
      dict_check == InstanceDictCheck::kUnsupported ||
      !ensureVersionTag(type)) {
    return {};
  }

  if (method_obj == nullptr || !PyFunction_Check(method_obj)) {
    return {};
  }

  // Now we need to keep the function alive
  irfunc.env.addReference(Ref<>::create(method_obj));
  return {method_obj, false, true, dict_check == InstanceDictCheck::kRuntime};
}

void addDeoptPatcher(
    Function& irfunc,
    MethodInvoke& invoke,
    Register* receiver,
    std::vector<Instr*>& replacement,
    MethodTarget& target,
    BorrowedRef<> name) {
  JIT_DCHECK(
      receiver->type().runtimePyType() != nullptr, "should have receiver");
  BorrowedRef<PyTypeObject> type{receiver->type().runtimePyType()};
  auto method_patcher = irfunc.allocateCodePatcher<TypeDeoptPatcher>(type);
  // The type is kept alive by the preloader, the name by the code object, and
  // the target we added a strong reference to (if necessary) when we looked it
  // up in getMethodObjectFromType.
  irfunc.env.setWatchValidator(
      method_patcher,
      [type,
       name,
       target,
       needs_dict_guard = target.needs_instance_dict_guard] {
        InstanceDictCheck dict_check = instanceDictCheck(type, name);
        return PyType_HasFeature(type, Py_TPFLAGS_READY) &&
            type->tp_getattro == PyObject_GenericGetAttr &&
            _PyType_Lookup(type, name) == target.callable &&
            PyUnstable_Type_AssignVersionTag(type) &&
            (dict_check == InstanceDictCheck::kRuntime) == needs_dict_guard &&
            dict_check != InstanceDictCheck::kUnsupported;
      });
  auto patchpoint = DeoptPatchpoint::create(method_patcher);
  patchpoint->setBytecodeOffset(invoke.load_method->bytecodeOffset());
  patchpoint->setGuiltyReg(receiver);
  patchpoint->setDescr("Python method");
  replacement.push_back(patchpoint);
}

void addInstanceDictGuard(
    Function& irfunc,
    MethodInvoke& invoke,
    Register* receiver,
    std::vector<Instr*>& replacement) {
  BCOffset bc_offset = invoke.load_method->bytecodeOffset();
  FrameState guard_frame{*invoke.load_method->frameState()};
  guard_frame.stack.push(receiver);
  auto append_guard = [&](Register* condition, const char* descr) {
    auto snapshot = Snapshot::create(guard_frame);
    snapshot->setBytecodeOffset(bc_offset);
    replacement.push_back(snapshot);
    auto guard = Guard::create(condition, guard_frame);
    guard->setBytecodeOffset(bc_offset);
    guard->setGuiltyReg(receiver);
    guard->setDescr(descr);
    replacement.push_back(guard);
  };

#if PY_VERSION_HEX >= 0x030E0000
  Register* inline_values_valid = irfunc.env.allocateRegister();
  auto load_valid = LoadField::create(
      inline_values_valid,
      receiver,
      "inline_values.valid",
      receiver->type().runtimePyType()->tp_basicsize +
          offsetof(PyDictValues, valid),
      TCUInt8);
  load_valid->setBytecodeOffset(bc_offset);
  replacement.push_back(load_valid);
  append_guard(inline_values_valid, "inline values are valid");
#else
  Register* dict_or_values = irfunc.env.allocateRegister();
  auto load_dict_or_values = LoadField::create(
      dict_or_values,
      receiver,
      "__dict__",
      -3 * static_cast<Py_ssize_t>(sizeof(PyObject*)),
      TOptObject);
  load_dict_or_values->setBytecodeOffset(bc_offset);
  replacement.push_back(load_dict_or_values);
  Register* dict_or_values_ptr = irfunc.env.allocateRegister();
  auto bitcast = BitCast::create(dict_or_values_ptr, dict_or_values, TCUInt64);
  bitcast->setBytecodeOffset(bc_offset);
  replacement.push_back(bitcast);
  Register* one = irfunc.env.allocateRegister();
  auto load_one = LoadConst::create(one, Type::fromCUInt(1, TCUInt64));
  load_one->setBytecodeOffset(bc_offset);
  replacement.push_back(load_one);
  Register* is_values = irfunc.env.allocateRegister();
  auto check_values = IntBinaryOp::create(
      is_values, BinaryOpKind::kAnd, dict_or_values_ptr, one);
  check_values->setBytecodeOffset(bc_offset);
  replacement.push_back(check_values);
  append_guard(is_values, "dict values check");
#endif

  invoke.load_method->expandInto(replacement);
}

// Returns true if LoadMethod/CallMethod/GetSecondOutput were removed.
// Returns false if they could not be removed.
bool tryEliminateLoadMethod(Function& irfunc, MethodInvoke& invoke) {
  PyCodeObject* code = invoke.load_method->frameState()->code;
  PyObject* names = code->co_names;
  PyObject* name = PyTuple_GetItem(names, invoke.load_method->nameIdx());
  JIT_DCHECK(name != nullptr, "name must not be null");

  Register* receiver = invoke.load_method->receiver();
  Type receiver_type = receiver->type();
  MethodTarget target = getMethodObjectFromType(irfunc, receiver_type, name);
  if (target.callable == nullptr) {
    // No such method. Let the LoadMethod fail at runtime. _PyType_Lookup does
    // not raise an exception.
    return false;
  }
  BorrowedRef<> method_obj = target.callable;
  if (Py_TYPE(method_obj) == &PyStaticMethod_Type) {
    // This is slightly tricky and nobody uses this except for
    // bytearray/bytes/str.maketrans. Not worth optimizing.
    return false;
  }
  Register* method_reg = invoke.load_method->output();
  auto load_const = LoadConst::create(
      method_reg, Type::fromObject(irfunc.env.addReference(method_obj.get())));
  auto call_static = VectorCall::create(
      invoke.call_method->numOperands(),
      invoke.call_method->output(),
      target.is_static_python_invoke
          ? invoke.call_method->flags() | CallFlags::Static
          : invoke.call_method->flags(),
      *invoke.call_method->frameState());
  call_static->setOperand(0, method_reg);
  if (Py_TYPE(method_obj) == &PyClassMethodDescr_Type) {
    // Pass the type as the first argument (e.g. dict.fromkeys).
    Register* type_reg = irfunc.env.allocateRegister();
    auto load_type = LoadConst::create(
        type_reg,
        Type::fromObject(
            reinterpret_cast<PyObject*>(receiver_type.runtimePyType())));
    load_type->setBytecodeOffset(invoke.load_method->bytecodeOffset());
    load_type->insertBefore(*invoke.call_method);
    call_static->setOperand(1, type_reg);
  } else {
    JIT_DCHECK(
        Py_TYPE(method_obj) == &PyMethodDescr_Type ||
            Py_TYPE(method_obj) == &PyWrapperDescr_Type ||
            Py_TYPE(method_obj) == &PyFunction_Type,
        "unexpected type");
    // Pass the instance as the first argument (e.g. str.join, str.__mod__).
    call_static->setOperand(1, receiver);
  }
  for (std::size_t i = 2; i < invoke.call_method->numOperands(); i++) {
    call_static->setOperand(i, invoke.call_method->getOperand(i));
  }
  auto use_type = UseType::create(
      receiver,
      target.needs_type_patcher ? receiver_type
                                : receiver_type.unspecialized());
  std::vector<Instr*> replacement{use_type};
  if (target.needs_type_patcher) {
    addDeoptPatcher(irfunc, invoke, receiver, replacement, target, name);
  }
  replacement.push_back(load_const);
  if (target.needs_instance_dict_guard) {
    addInstanceDictGuard(irfunc, invoke, receiver, replacement);
  } else {
    invoke.load_method->expandInto(replacement);
  }
  invoke.get_instance->replaceWith(
      *Assign::create(invoke.get_instance->output(), receiver));
  invoke.call_method->replaceWith(*call_static);
  delete invoke.load_method;
  delete invoke.get_instance;
  delete invoke.call_method;
  return true;
}

} // namespace

void LoadMethodElimination::run(Function& irfunc) {
  bool changed = true;
  while (changed) {
    changed = false;
    UnorderedMap<LoadMethodBase*, MethodInvoke> invokes;
    for (auto& block : irfunc.cfg.blocks) {
      for (auto& instr : block) {
        if (!instr.isCallMethod()) {
          continue;
        }
        auto cm = static_cast<CallMethod*>(&instr);
        auto func_instr = cm->func()->instr();
        if (func_instr->isLoadMethodSuper()) {
          continue;
        }

        if (!isLoadMethodBase(*func_instr)) {
          // {FillTypeMethodCache | LoadTypeMethodCacheEntryValue} and
          // CallMethod represent loading and invoking methods off a type (e.g.
          // dict.fromkeys(...)) which do not need to follow
          // LoadMethod/CallMethod pairing invariant and do not benefit from
          // tryEliminateLoadMethod which only handles eliminating of method
          // calls on the instance
          continue;
        }

        auto lm = static_cast<LoadMethodBase*>(func_instr);

        JIT_DCHECK(
            cm->self()->instr()->isGetSecondOutput(),
            "GetSecondOutput/CallMethod should be paired but got "
            "{}/CallMethod",
            cm->self()->instr()->opname());
        auto glmi = static_cast<GetSecondOutput*>(cm->self()->instr());
        auto result = invokes.emplace(lm, MethodInvoke{lm, glmi, cm});
        if (!result.second) {
          // This pass currently only handles 1:1 LoadMethod/CallMethod
          // combinations. If there are multiple CallMethod for a given
          // LoadMethod, bail out.
          // TASK(T138839090): support multiple CallMethod
          invokes.erase(result.first);
        }
      }
    }
    for (auto [lm, invoke] : invokes) {
      changed |= tryEliminateLoadMethod(irfunc, invoke);
    }
    reflowTypes(irfunc);
  }
}

} // namespace cinderx::jit::hir

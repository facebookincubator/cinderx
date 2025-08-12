// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/builtin_load_method_elimination.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/module_state.h"

namespace jit::hir {

namespace {

struct MethodInvoke {
  LoadMethodBase* load_method{nullptr};
  GetSecondOutput* get_instance{nullptr};
  CallMethod* call_method{nullptr};
};

#if PY_VERSION_HEX >= 0x030C0000

BorrowedRef<> immutableMultithreadedTypeLookup(
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<> name) {
  BorrowedRef<> mro = type->tp_mro;
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(mro.get()); i++) {
    PyTypeObject* mro_type =
        reinterpret_cast<PyTypeObject*>(PyTuple_GET_ITEM(mro.get(), i));
    if (PyType_HasFeature(mro_type, _Py_TPFLAGS_STATIC_BUILTIN)) {
      auto& builtins = cinderx::getModuleState()->builtinMembers();

      auto members = builtins.find(mro_type);
      if (members == builtins.end()) {
        // We don't know anything about this builtin type.
        return nullptr;
      }
      // We load all of the members from the MRO in the builtins
      // cache so it's completely authorative.
      return PyDict_GetItemWithError(members->second, name);
    } else if (
        !PyType_HasFeature(mro_type, Py_TPFLAGS_IMMUTABLETYPE) ||
        !PyType_CheckExact(mro_type)) {
      // We can't trust anything about this base type
      return nullptr;
    }

    BorrowedRef<> method_obj =
        PyDict_GetItemWithError(_PyType_GetDict(mro_type), name);
    if (method_obj != nullptr) {
      return method_obj;
    }
  }
  return nullptr;
}

#endif

// Gets a directly invokable method object from a JIT Type. This only succeeds
// if we know the type can be directly invoked.
BorrowedRef<> getMethodObjectFromType(Type receiver_type, BorrowedRef<> name) {
  // This is a list of common builtin types whose methods cannot be overwritten
  // from managed code and for which looking up the methods is guaranteed to
  // not do anything "weird" that needs to happen at runtime, like make a
  // network request.
  // Note that due to the different staticmethod/classmethod/other descriptors,
  // loading and invoking methods off an instance (e.g. {}.fromkeys(...)) is
  // resolved and called differently than from the type (e.g.
  // dict.fromkeys(...)). The code below handles the instance case only.
#if PY_VERSION_HEX < 0x030C0000

  if (!(receiver_type <= TArray || receiver_type <= TBool ||
        receiver_type <= TBytesExact || receiver_type <= TCode ||
        receiver_type <= TDictExact || receiver_type <= TFloatExact ||
        receiver_type <= TListExact || receiver_type <= TLongExact ||
        receiver_type <= TNoneType || receiver_type <= TSetExact ||
        receiver_type <= TTupleExact || receiver_type <= TUnicodeExact)) {
    return nullptr;
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
    return nullptr;
  }
  return _PyType_Lookup(type, name);
#else
  if (!receiver_type.hasTypeExactSpec()) {
    return nullptr;
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
    return nullptr;
  }

  BorrowedRef<> method_obj = nullptr;
  // In 3.12 we can't do PyType_Lookup because for built-in types it needs
  // access to the current runtime, and in multi-threaded compile we don't
  // have it. So we instead have a cache of all of the builtin types that we
  // support this for.
  auto& builtins = cinderx::getModuleState()->builtinMembers();

  if (PyType_HasFeature(type, _Py_TPFLAGS_STATIC_BUILTIN)) {
    auto it = builtins.find(receiver_type.runtimePyType());
    if (it != builtins.end()) {
      method_obj = PyDict_GetItemWithError(it->second, name);
    }
  } else if (
      PyType_HasFeature(type, Py_TPFLAGS_IMMUTABLETYPE) &&
      PyType_CheckExact(type) && type->tp_dictoffset == 0) {
    method_obj = immutableMultithreadedTypeLookup(type, name);
    if (Py_TYPE(method_obj) != &PyClassMethodDescr_Type &&
        Py_TYPE(method_obj) != &PyMethodDescr_Type &&
        Py_TYPE(method_obj) != &PyWrapperDescr_Type &&
        Py_TYPE(method_obj) != &PyFunction_Type) {
      method_obj = nullptr;
    }
  }
  return method_obj;
#endif
}

// Returns true if LoadMethod/CallMethod/GetSecondOutput were removed.
// Returns false if they could not be removed.
bool tryEliminateLoadMethod(Function& irfunc, MethodInvoke& invoke) {
  ThreadedCompileSerialize guard;
  PyCodeObject* code = invoke.load_method->frameState()->code;
  PyObject* names = code->co_names;
  PyObject* name = PyTuple_GetItem(names, invoke.load_method->name_idx());
  JIT_DCHECK(name != nullptr, "name must not be null");

  Register* receiver = invoke.load_method->receiver();
  Type receiver_type = receiver->type();
  BorrowedRef<> method_obj = getMethodObjectFromType(receiver_type, name);
  if (method_obj == nullptr) {
    // No such method. Let the LoadMethod fail at runtime. _PyType_Lookup does
    // not raise an exception.
    return false;
  }
  if (Py_TYPE(method_obj) == &PyStaticMethod_Type) {
    // This is slightly tricky and nobody uses this except for
    // bytearray/bytes/str.maketrans. Not worth optimizing.
    return false;
  }
  Register* method_reg = invoke.load_method->output();
  auto load_const = LoadConst::create(
      method_reg, Type::fromObject(irfunc.env.addReference(method_obj.get())));
  auto call_static = VectorCall::create(
      invoke.call_method->NumOperands(),
      invoke.call_method->output(),
      invoke.call_method->flags() | CallFlags::Static,
      *invoke.call_method->frameState());
  call_static->SetOperand(0, method_reg);
  if (Py_TYPE(method_obj) == &PyClassMethodDescr_Type) {
    // Pass the type as the first argument (e.g. dict.fromkeys).
    Register* type_reg = irfunc.env.AllocateRegister();
    auto load_type = LoadConst::create(
        type_reg,
        Type::fromObject(
            reinterpret_cast<PyObject*>(receiver_type.runtimePyType())));
    load_type->setBytecodeOffset(invoke.load_method->bytecodeOffset());
    load_type->InsertBefore(*invoke.call_method);
    call_static->SetOperand(1, type_reg);
  } else {
    JIT_DCHECK(
        Py_TYPE(method_obj) == &PyMethodDescr_Type ||
            Py_TYPE(method_obj) == &PyWrapperDescr_Type ||
            Py_TYPE(method_obj) == &PyFunction_Type,
        "unexpected type");
    // Pass the instance as the first argument (e.g. str.join, str.__mod__).
    call_static->SetOperand(1, receiver);
  }
  for (std::size_t i = 2; i < invoke.call_method->NumOperands(); i++) {
    call_static->SetOperand(i, invoke.call_method->GetOperand(i));
  }
  auto use_type = UseType::create(receiver, receiver_type.unspecialized());
  invoke.load_method->ExpandInto({use_type, load_const});
  invoke.get_instance->ReplaceWith(
      *Assign::create(invoke.get_instance->output(), receiver));
  invoke.call_method->ReplaceWith(*call_static);
  delete invoke.load_method;
  delete invoke.get_instance;
  delete invoke.call_method;
  return true;
}

} // namespace

void BuiltinLoadMethodElimination::Run(Function& irfunc) {
  bool changed = true;
  while (changed) {
    changed = false;
    UnorderedMap<LoadMethodBase*, MethodInvoke> invokes;
    for (auto& block : irfunc.cfg.blocks) {
      for (auto& instr : block) {
        if (!instr.IsCallMethod()) {
          continue;
        }
        auto cm = static_cast<CallMethod*>(&instr);
        auto func_instr = cm->func()->instr();
        if (func_instr->IsLoadMethodSuper()) {
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
            cm->self()->instr()->IsGetSecondOutput(),
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

} // namespace jit::hir

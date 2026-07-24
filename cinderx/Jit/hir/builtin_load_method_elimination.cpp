// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/builtin_load_method_elimination.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/type.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/module_state.h"

namespace cinderx::jit::hir {

namespace {

struct MethodInvoke {
  LoadMethodBase* load_method{nullptr};
  GetSecondOutput* get_instance{nullptr};
  CallMethod* call_method{nullptr};
};

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
  }
  return method_obj;
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
      invoke.call_method->numOperands(),
      invoke.call_method->output(),
      invoke.call_method->flags() | CallFlags::Static,
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
  auto use_type = UseType::create(receiver, receiver_type.unspecialized());
  invoke.load_method->expandInto({use_type, load_const});
  invoke.get_instance->replaceWith(
      *Assign::create(invoke.get_instance->output(), receiver));
  invoke.call_method->replaceWith(*call_static);
  delete invoke.load_method;
  delete invoke.get_instance;
  delete invoke.call_method;
  return true;
}

} // namespace

void BuiltinLoadMethodElimination::run(Function& irfunc) {
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

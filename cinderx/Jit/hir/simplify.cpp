// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/simplify.h"

#include "cinderx/python.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/property.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/module_state.h"

#include <fmt/ostream.h>

namespace jit::hir {

// This file contains the Simplify pass, which is a collection of
// strength-reduction optimizations. An optimization should be added as a case
// in Simplify rather than a standalone pass if and only if it meets these
// criteria:
// - It operates on one instruction at a time, with no global analysis or
//   state.
// - Optimizable instructions are replaced with 0 or more new instructions that
//   define an equivalent value while doing less work.
//
// To add support for a new instruction Foo, add a function simplifyFoo(Env&
// env, const Foo* instr) (env can be left out if you don't need it) containing
// the optimization and call it from a new case in
// simplifyInstr(). simplifyFoo() should analyze the given instruction, then do
// one of the following:
// - If the instruction is not optimizable, return nullptr and do not call any
//   functions on env.
// - If the instruction is redundant and can be elided, return the existing
//   value that should replace its output (this is often one of the
//   instruction's inputs).
// - If the instruction can be replaced with a cheaper sequence of
//   instructions, emit those instructions using env.emit<T>(...). For
//   instructions that define an output, emit<T> will allocate and return an
//   appropriately-typed Register* for you, to ease chaining multiple
//   instructions. As with the previous case, return the Register* that should
//   replace the current output of the instruction.
// - If the instruction can be elided but does not produce an output, set
//   env.optimized = true and return nullptr.
//
// Do not modify, unlink, or delete the existing instruction; all of those
// details are handled by existing code outside of the individual optimization
// functions.

namespace {

struct Env {
  explicit Env(Function& f)
      : func{f},
        type_object(
            Type::fromObject(reinterpret_cast<PyObject*>(&PyType_Type))) {}

  // The current function.
  Function& func;

  // The current block being emitted into. Might not be the block originally
  // containing the instruction being optimized, if more blocks have been
  // inserted by the simplify function.
  BasicBlock* block{nullptr};

  // Insertion cursor for new instructions. Must belong to block's Instr::List,
  // and except for brief critical sections during emit functions on Env,
  // should always point to the original, unoptimized instruction.
  Instr::List::iterator cursor;

  // Bytecode instruction of the instruction being optimized, automatically set
  // on all replacement instructions.
  BCOffset bc_off{-1};

  // Set to true by emit<T>() to indicate that the original instruction should
  // be removed.
  bool optimized{false};

  // The object that corresponds to "type".
  Type type_object{TTop};

  // Number of new basic blocks added by the simplifier.
  size_t new_blocks{0};

  // Create and insert the specified instruction. If the instruction has an
  // output, a new Register* will be created and returned.
  template <typename T, typename... Args>
  Register* emit(Args&&... args) {
    return emitInstr<T>(std::forward<Args>(args)...)->output();
  }

  // Similar to emit(), but returns the instruction itself. Useful for
  // instructions with no output, when you need to manipulate the instruction
  // after creation.
  template <typename T, typename... Args>
  T* emitInstr(Args&&... args) {
    if constexpr (T::has_output) {
      return emitRawInstr<T>(
          func.env.AllocateRegister(), std::forward<Args>(args)...);
    } else {
      return emitRawInstr<T>(std::forward<Args>(args)...);
    }
  }

  // Similar to emitRawInstr<T>(), but does not automatically create an output
  // Create and insert the specified instruction. If the instruction has an
  // output, a new Register* will be created and returned.
  template <typename T, typename... Args>
  Register* emitVariadic(std::size_t arity, Args&&... args) {
    if constexpr (T::has_output) {
      return emitRawInstr<T>(
                 arity,
                 func.env.AllocateRegister(),
                 std::forward<Args>(args)...)
          ->output();
    } else {
      return emitRawInstr<T>(arity, std::forward<Args>(args)...)->output();
    }
  }

  // Similar to emit<T>(), but does not automatically create an output
  // register.
  template <typename T, typename... Args>
  T* emitRawInstr(Args&&... args) {
    optimized = true;
    T* instr = T::create(std::forward<Args>(args)...);
    instr->setBytecodeOffset(bc_off);
    block->insert(instr, cursor);

    if constexpr (T::has_output) {
      Register* output = instr->output();
      switch (instr->opcode()) {
        case Opcode::kVectorCall:
          // We don't know the exact output type until its operands are
          // populated.
          output->set_type(TObject);
          break;
        default:
          output->set_type(outputType(*instr));
          break;
      }
    }

    return instr;
  }

  // Create and return a conditional value. Expects three callables:
  // - do_branch is given two BasicBlock* and should emit a conditional branch
  //   instruction using them.
  // - do_bb1 should emit code for the first successor, returning the computed
  //   value.
  // - do_bb2 should do the same for the second successor.
  template <typename BranchFn, typename Bb1Fn, typename Bb2Fn>
  Register* emitCond(BranchFn do_branch, Bb1Fn do_bb1, Bb2Fn do_bb2) {
    // bb1, bb2, and the new tail block that's split from the original.
    new_blocks += 3;

    BasicBlock* bb1 = func.cfg.AllocateBlock();
    BasicBlock* bb2 = func.cfg.AllocateBlock();
    do_branch(bb1, bb2);
    JIT_CHECK(
        cursor != block->begin(),
        "block should not be empty after calling do_branch()");
    BasicBlock* tail = block->splitAfter(*std::prev(cursor));

    block = bb1;
    cursor = bb1->end();
    Register* bb1_reg = do_bb1();
    emit<Branch>(tail);

    block = bb2;
    cursor = bb2->end();
    Register* bb2_reg = do_bb2();
    emit<Branch>(tail);

    block = tail;
    cursor = tail->begin();
    std::unordered_map<BasicBlock*, Register*> phi_srcs{
        {bb1, bb1_reg},
        {bb2, bb2_reg},
    };
    return emit<Phi>(phi_srcs);
  }

  // Create and return a conditional value that could go through a slow path if
  // it matches a certain condition. Expects two callables:
  //
  // - do_branch is given a BasicBlock* and it is expected that it will
  //   conditionally branch to that block if it needs to. The true_bb will be
  //   patched after the fast path is split. It should return the branch
  //   instruction so that it can be patched.
  // - do_slow_path should emit code for the slow path, returning the computed
  //   value.
  //
  // It is expected that the slow path will jump back to the default path at the
  // end of its block.
  template <typename BranchFn, typename SlowPathFn>
  Phi* emitCondSlowPath(
      Register* output,
      Register* previous_path_value,
      BranchFn do_branch,
      SlowPathFn do_slow_path) {
    new_blocks += 2;

    BasicBlock* previous_path = block;
    BasicBlock* slow_path = func.cfg.AllocateBlock();

    auto branch = do_branch(slow_path);
    BasicBlock* fast_path = block->splitAfter(*branch);
    branch->set_true_bb(fast_path);

    block = slow_path;
    cursor = slow_path->begin();
    auto slow_path_value = do_slow_path();
    emit<Branch>(fast_path);

    block = fast_path;
    cursor = fast_path->begin();
    std::unordered_map<BasicBlock*, Register*> args{
        {previous_path, previous_path_value},
        {slow_path, slow_path_value},
    };

    return emitRawInstr<Phi>(output, args);
  }
};

Register* simplifyCheck(const CheckBase* instr) {
  // These all check their input for null.
  if (instr->GetOperand(0)->isA(TObject)) {
    // No UseType is necessary because we never guard potentially-null values.
    return instr->GetOperand(0);
  }
  return nullptr;
}

Register* simplifyCheckSequenceBounds(
    Env& env,
    const CheckSequenceBounds* instr) {
  Register* sequence = instr->GetOperand(0);
  Register* idx = instr->GetOperand(1);
  if (sequence->isA(TTupleExact) && sequence->instr()->IsMakeTuple() &&
      idx->isA(TCInt) && idx->type().hasIntSpec()) {
    size_t length = static_cast<const MakeTuple*>(sequence->instr())->nvalues();
    intptr_t idx_value = idx->type().intSpec();
    bool adjusted = false;
    if (idx_value < 0) {
      idx_value += length;
      adjusted = true;
    }
    if (static_cast<size_t>(idx_value) < length) {
      env.emit<UseType>(sequence, sequence->type());
      env.emit<UseType>(idx, idx->type());
      if (adjusted) {
        return env.emit<LoadConst>(Type::fromCInt(idx_value, TCInt64));
      } else {
        return idx;
      }
    }
  }
  return nullptr;
}

Register* simplifyGuardType(Env& env, const GuardType* instr) {
  Register* input = instr->GetOperand(0);
  Type type = instr->target();
  if (input->isA(type)) {
    // We don't need a UseType: If an instruction cares about the type of this
    // GuardType's output, it will express that through its operand type
    // constraints. Once this GuardType is removed, those constraints will
    // apply to input's instruction rather than this GuardType, and any
    // downstream instructions will still be satisfied.
    return input;
  }
  if (type == TNoneType) {
    return env.emit<GuardIs>(Py_None, input);
  }
  return nullptr;
}

Register* simplifyRefineType(const RefineType* instr) {
  Register* input = instr->GetOperand(0);
  if (input->isA(instr->type())) {
    // No UseType for the same reason as GuardType above: RefineType itself
    // doesn't care about the input's type, only users of its output do, and
    // they're unchanged.
    return input;
  }
  return nullptr;
}

Register* simplifyCast(const Cast* instr) {
  Register* input = instr->GetOperand(0);
  Type type = instr->exact() ? Type::fromTypeExact(instr->pytype())
                             : Type::fromType(instr->pytype());
  if (instr->optional()) {
    type |= TNoneType;
  }
  if (input->isA(type)) {
    // No UseType for the same reason as GuardType above: Cast itself
    // doesn't care about the input's type, only users of its output do, and
    // they're unchanged.
    return input;
  }
  return nullptr;
}

Register* emitGetLengthInt64(Env& env, Register* obj) {
  Type ty = obj->type();
  if (ty <= TListExact || ty <= TTupleExact || ty <= TArray) {
    env.emit<UseType>(obj, ty.unspecialized());
    return env.emit<LoadField>(
        obj, "ob_size", offsetof(PyVarObject, ob_size), TCInt64);
  }
  if (ty <= TDictExact || ty <= TSetExact || ty <= TUnicodeExact) {
    std::size_t offset = 0;
    const char* name = nullptr;
    if (ty <= TDictExact) {
      offset = offsetof(PyDictObject, ma_used);
      name = "ma_used";
    } else if (ty <= TSetExact) {
      offset = offsetof(PySetObject, used);
      name = "used";
    } else if (ty <= TUnicodeExact) {
      // Note: In debug mode, the interpreter has an assert that ensures the
      // string is "ready", check PyUnicode_GET_LENGTH for strings.
      offset = offsetof(PyASCIIObject, length);
      name = "length";
    } else {
      JIT_ABORT("unexpected type");
    }
    env.emit<UseType>(obj, ty.unspecialized());
    return env.emit<LoadField>(obj, name, offset, TCInt64);
  }
  return nullptr;
}

Register* simplifyGetLength(Env& env, const GetLength* instr) {
  Register* obj = instr->GetOperand(0);
  if (Register* size = emitGetLengthInt64(env, obj)) {
    return env.emit<PrimitiveBox>(size, TCInt64, *instr->frameState());
  }
  return nullptr;
}

Register* simplifyIntConvert(Env& env, const IntConvert* instr) {
  Register* src = instr->GetOperand(0);
  if (src->isA(instr->type())) {
    env.emit<UseType>(src, instr->type());
    return instr->GetOperand(0);
  }
  return nullptr;
}

Register* simplifyCompare(Env& env, const Compare* instr) {
  Register* left = instr->GetOperand(0);
  Register* right = instr->GetOperand(1);
  CompareOp op = instr->op();

  if (left->isA(TNoneType) && right->isA(TNoneType)) {
    if (op == CompareOp::kEqual || op == CompareOp::kNotEqual) {
      env.emit<UseType>(left, TNoneType);
      env.emit<UseType>(right, TNoneType);
      return env.emit<LoadConst>(
          Type::fromObject(op == CompareOp::kEqual ? Py_True : Py_False));
    }
  }

  // Can compare booleans for equality with primitive operations.
  if (left->isA(TBool) && right->isA(TBool) &&
      (op == CompareOp::kEqual || op == CompareOp::kNotEqual)) {
    if (auto prim_op = toPrimitiveCompareOp(op)) {
      env.emit<UseType>(left, TBool);
      env.emit<UseType>(right, TBool);
      Register* result = env.emit<PrimitiveCompare>(*prim_op, left, right);
      return env.emit<PrimitiveBoxBool>(result);
    }
  }

  // Emit FloatCompare if both args are FloatExact and the op is supported
  // between two longs.
  if (left->isA(TFloatExact) && right->isA(TFloatExact) &&
      !(op == CompareOp::kIn || op == CompareOp::kNotIn ||
        op == CompareOp::kExcMatch)) {
    return env.emit<FloatCompare>(instr->op(), left, right);
  }

  // Emit LongCompare if both args are LongExact and the op is supported between
  // two longs.
  if (left->isA(TLongExact) && right->isA(TLongExact) &&
      !(op == CompareOp::kIn || op == CompareOp::kNotIn ||
        op == CompareOp::kExcMatch)) {
    return env.emit<LongCompare>(instr->op(), left, right);
  }

  // Emit UnicodeCompare if both args are UnicodeExact and the op is supported
  // between two strings.
  if (left->isA(TUnicodeExact) && right->isA(TUnicodeExact) &&
      !(op == CompareOp::kIn || op == CompareOp::kNotIn ||
        op == CompareOp::kExcMatch)) {
    return env.emit<UnicodeCompare>(instr->op(), left, right);
  }

  return nullptr;
}

Register* simplifyCondBranch(Env& env, const CondBranch* instr) {
  Register* cond = instr->GetOperand(0);
  Type cond_type = cond->type();
  // Constant condition folds into an unconditional jump.
  if (cond_type.hasIntSpec()) {
    auto spec = cond_type.intSpec();
    return env.emit<Branch>(spec ? instr->true_bb() : instr->false_bb());
  }
  // Common pattern of CondBranch getting its condition from an IntConvert,
  // which had been simplified down from an IsTruthy.  Can forward the value
  // only if it's being widened.  Narrowing an integer might change it from
  // non-zero to zero.
  if (cond->instr()->IsIntConvert()) {
    auto convert = static_cast<IntConvert*>(cond->instr());
    Register* src = convert->src();
    if (convert->type().sizeInBytes() >= src->type().sizeInBytes()) {
      return env.emit<CondBranch>(src, instr->true_bb(), instr->false_bb());
    }
  }
  return nullptr;
}

Register* simplifyCondBranchCheckType(
    Env& env,
    const CondBranchCheckType* instr) {
  Register* value = instr->GetOperand(0);
  Type actual_type = value->type();
  Type expected_type = instr->type();
  if (actual_type <= expected_type) {
    env.emit<UseType>(value, actual_type);
    return env.emit<Branch>(instr->true_bb());
  }
  if (!actual_type.couldBe(expected_type)) {
    env.emit<UseType>(value, actual_type);
    return env.emit<Branch>(instr->false_bb());
  }
  return nullptr;
}

Register* simplifyIsTruthy(Env& env, const IsTruthy* instr) {
  Type ty = instr->GetOperand(0)->type();
  PyObject* obj = ty.asObject();
  if (obj != nullptr) {
    // Should only consider immutable Objects
    static const std::unordered_set<PyTypeObject*> kTrustedTypes{
        &PyBool_Type,
        &PyFloat_Type,
        &PyLong_Type,
        &PyFrozenSet_Type,
        &PySlice_Type,
        &PyTuple_Type,
        &PyUnicode_Type,
        Py_TYPE(Py_None),
    };
    if (kTrustedTypes.contains(Py_TYPE(obj))) {
      int res = PyObject_IsTrue(obj);
      JIT_CHECK(res >= 0, "PyObject_IsTrue failed on trusted type");
      // Since we no longer use instr->GetOperand(0), we need to make sure that
      // we don't lose any associated type checks
      env.emit<UseType>(instr->GetOperand(0), ty);
      Type output_type = instr->output()->type();
      return env.emit<LoadConst>(Type::fromCInt(res, output_type));
    }
  }
  if (ty <= TBool) {
    Register* left = instr->GetOperand(0);
    env.emit<UseType>(left, TBool);
    Register* right = env.emit<LoadConst>(Type::fromObject(Py_True));
    Register* result =
        env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, left, right);
    return env.emit<IntConvert>(result, TCInt32);
  }
  if (Register* size = emitGetLengthInt64(env, instr->GetOperand(0))) {
    return env.emit<IntConvert>(size, TCInt32);
  }
  if (ty <= TLongExact) {
    Register* left = instr->GetOperand(0);
    env.emit<UseType>(left, ty);
    // Zero is canonical as a "small int" in CPython.
    auto zero = cinderx::getModuleState()->runtime()->zero();
    Register* right = env.emit<LoadConst>(Type::fromObject(zero));
    Register* result =
        env.emit<PrimitiveCompare>(PrimitiveCompareOp::kNotEqual, left, right);
    return env.emit<IntConvert>(result, TCInt32);
  }
  return nullptr;
}

Register* simplifyLoadTupleItem(Env& env, const LoadTupleItem* instr) {
  Register* src = instr->GetOperand(0);
  Type src_ty = src->type();
  if (!src_ty.hasValueSpec(TTuple)) {
    return nullptr;
  }
  env.emit<UseType>(src, src_ty);
  BorrowedRef<> item = PyTuple_GET_ITEM(src_ty.objectSpec(), instr->idx());
  return env.emit<LoadConst>(Type::fromObject(env.func.env.addReference(item)));
}

Register* simplifyLoadArrayItem(Env& env, const LoadArrayItem* instr) {
  Register* src = instr->seq();
  if (!instr->idx()->type().hasIntSpec()) {
    return nullptr;
  }
  intptr_t idx_signed = instr->idx()->type().intSpec();
  JIT_CHECK(idx_signed >= 0, "LoadArrayItem should not have negative index");
  uintptr_t idx = static_cast<uintptr_t>(idx_signed);
  // We can only do this for tuples because lists and arrays, the other
  // sequence types, are mutable. A more general LoadElimination pass could
  // accomplish that, though.
  if (src->instr()->IsMakeTuple()) {
    size_t length = static_cast<const MakeTuple*>(src->instr())->nvalues();
    if (idx < length) {
      env.emit<UseType>(src, TTupleExact);
      env.emit<UseType>(instr->idx(), instr->idx()->type());
      return src->instr()->GetOperand(idx);
    }
  }
  if (src->type().hasValueSpec(TTupleExact)) {
    if (idx_signed < PyTuple_GET_SIZE(src->type().objectSpec())) {
      env.emit<UseType>(src, src->type());
      env.emit<UseType>(instr->idx(), instr->idx()->type());
      BorrowedRef<> item = PyTuple_GET_ITEM(src->type().objectSpec(), idx);
      return env.emit<LoadConst>(
          Type::fromObject(env.func.env.addReference(item)));
    }
  }
  return nullptr;
}

Register* simplifyLoadVarObjectSize(Env& env, const LoadVarObjectSize* instr) {
  Register* obj_reg = instr->GetOperand(0);
  Type type = obj_reg->type();
  // We can only do this for tuples because lists and arrays, the other
  // sequence types, are mutable. A more general LoadElimination pass could
  // accomplish that, though.
  if (obj_reg->instr()->IsMakeTuple()) {
    env.emit<UseType>(obj_reg, type);
    size_t size = static_cast<const MakeTuple*>(obj_reg->instr())->nvalues();
    Type output_type = instr->output()->type();
    return env.emit<LoadConst>(Type::fromCInt(size, output_type));
  }
  if (type.hasValueSpec(TTupleExact) || type.hasValueSpec(TBytesExact)) {
    PyVarObject* obj = reinterpret_cast<PyVarObject*>(type.asObject());
    Py_ssize_t size = obj->ob_size;
    env.emit<UseType>(obj_reg, type);
    Type output_type = instr->output()->type();
    return env.emit<LoadConst>(Type::fromCInt(size, output_type));
  }
  return nullptr;
}

Register* simplifyLoadModuleMethodCached(
    Env& env,
    const LoadMethod* load_meth) {
  Register* receiver = load_meth->GetOperand(0);
  int name_idx = load_meth->name_idx();
  return env.emit<LoadModuleMethodCached>(
      receiver, name_idx, *load_meth->frameState());
}

Register* simplifyLoadTypeMethodCached(Env& env, const LoadMethod* load_meth) {
  Register* receiver = load_meth->GetOperand(0);
  const int cache_id = env.func.env.allocateLoadTypeMethodCache();
  env.emit<UseType>(receiver, TType);
  Register* guard = env.emit<LoadTypeMethodCacheEntryType>(cache_id);
  Register* type_matches =
      env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, guard, receiver);
  return env.emitCond(
      [&](BasicBlock* fast_path, BasicBlock* slow_path) {
        env.emit<CondBranch>(type_matches, fast_path, slow_path);
      },
      [&] { // Fast path
        return env.emit<LoadTypeMethodCacheEntryValue>(cache_id, receiver);
      },
      [&] { // Slow path
        int name_idx = load_meth->name_idx();
        return env.emit<FillTypeMethodCache>(
            receiver, name_idx, cache_id, *load_meth->frameState());
      });
}

Register* simplifyLoadMethod(Env& env, const LoadMethod* load_meth) {
  if (!getConfig().attr_caches) {
    return nullptr;
  }
  Register* receiver = load_meth->GetOperand(0);
  Type ty = receiver->type();
  if (receiver->isA(TType)) {
    return simplifyLoadTypeMethodCached(env, load_meth);
  }
  BorrowedRef<PyTypeObject> type{ty.runtimePyType()};
  if (type == &PyModule_Type || type == &Ci_StrictModule_Type) {
    return simplifyLoadModuleMethodCached(env, load_meth);
  }
  return env.emit<LoadMethodCached>(
      load_meth->GetOperand(0),
      load_meth->name_idx(),
      *load_meth->frameState());
}

Register* simplifyBinaryOp(Env& env, const BinaryOp* instr) {
  BinaryOpKind op = instr->op();
  Register* lhs = instr->left();
  Register* rhs = instr->right();

  if (op == BinaryOpKind::kSubscript) {
    if (lhs->isA(TDictExact)) {
      return env.emit<DictSubscr>(lhs, rhs, *instr->frameState());
    }
    if (!rhs->isA(TLongExact)) {
      return nullptr;
    }
    Type lhs_type = lhs->type();
    Type rhs_type = rhs->type();
    if (lhs_type <= TTupleExact && lhs_type.hasObjectSpec() &&
        rhs_type.hasObjectSpec()) {
      int overflow;
      Py_ssize_t index =
          PyLong_AsLongAndOverflow(rhs_type.objectSpec(), &overflow);
      if (!overflow) {
        PyObject* lhs_obj = lhs_type.objectSpec();
        if (index >= 0 && index < PyTuple_GET_SIZE(lhs_obj)) {
          BorrowedRef<> item = PyTuple_GET_ITEM(lhs_obj, index);
          env.emit<UseType>(lhs, lhs_type);
          env.emit<UseType>(rhs, rhs_type);
          return env.emit<LoadConst>(
              Type::fromObject(env.func.env.addReference(item)));
        }
        // Fallthrough
      }
      // Fallthrough
    }
    if (lhs->isA(TListExact) || lhs->isA(TTupleExact)) {
      // TASK(T93509109): Replace TCInt64 with a less platform-specific
      // representation of the type, which should be analagous to Py_ssize_t.
      env.emit<UseType>(lhs, lhs->isA(TListExact) ? TListExact : TTupleExact);
      env.emit<UseType>(rhs, TLongExact);
      Register* right_index = env.emit<IndexUnbox>(rhs);
      env.emit<IsNegativeAndErrOccurred>(right_index, *instr->frameState());
      Register* adjusted_idx =
          env.emit<CheckSequenceBounds>(lhs, right_index, *instr->frameState());
      ssize_t offset = offsetof(PyTupleObject, ob_item);
      Register* array = lhs;
      // Lists carry a nested array of ob_item whereas tuples are variable-sized
      // structs.
      if (lhs->isA(TListExact)) {
        array = env.emit<LoadField>(
            lhs, "ob_item", offsetof(PyListObject, ob_item), TCPtr);
        offset = 0;
      }
      return env.emit<LoadArrayItem>(array, adjusted_idx, lhs, offset, TObject);
    }
    if (lhs_type <= TUnicodeExact && rhs_type <= TLongExact) { // Unicode subscr
      if (lhs_type.hasObjectSpec() && rhs_type.hasObjectSpec()) {
        // This isn't safe in the multi-threaded compilation on 3.12 because
        // we don't hold the GIL which is required for
        // PyUnicode_InternInPlace.
        RETURN_MULTITHREADED_COMPILE(nullptr);

        // Constant propagation
        Py_ssize_t idx = PyLong_AsSsize_t(rhs_type.objectSpec());
        if (idx == -1 && PyErr_Occurred()) {
          PyErr_Clear();
          return nullptr;
        }
        Py_ssize_t n = PyUnicode_GetLength(lhs_type.objectSpec());

        if (idx < -n || idx >= n) {
          return nullptr;
        }

        if (idx < 0) {
          idx += n;
        }

        ThreadedCompileSerialize guard;
        Py_UCS4 c = PyUnicode_ReadChar(lhs_type.objectSpec(), idx);
        PyObject* substr =
            PyUnicode_FromKindAndData(PyUnicode_4BYTE_KIND, &c, 1);
        if (substr == nullptr) {
          return nullptr;
        }
        PyUnicode_InternInPlace(&substr);
        Ref<> result = Ref<>::steal(substr);

        // Use exact types since we're relying on the object specializations.
        env.emit<UseType>(lhs, lhs_type);
        env.emit<UseType>(rhs, rhs_type);
        return env.emit<LoadConst>(
            Type::fromObject(env.func.env.addReference(result)));
      } else {
        env.emit<UseType>(lhs, TUnicodeExact);
        env.emit<UseType>(rhs, TLongExact);
        Register* unboxed_idx = env.emit<IndexUnbox>(rhs);
        env.emit<IsNegativeAndErrOccurred>(unboxed_idx, *instr->frameState());
        Register* adjusted_idx = env.emit<CheckSequenceBounds>(
            lhs, unboxed_idx, *instr->frameState());
        return env.emit<UnicodeSubscr>(lhs, adjusted_idx, *instr->frameState());
      }
    }
  }

  if (lhs->isA(TLongExact) && rhs->isA(TLongExact)) {
    // All binary ops on TLong's return mutable so can be freely simplified with
    // no explicit check.
    if (op == BinaryOpKind::kMatrixMultiply || op == BinaryOpKind::kSubscript) {
      // These will generate an error at runtime.
      return nullptr;
    }
    env.emit<UseType>(lhs, TLongExact);
    env.emit<UseType>(rhs, TLongExact);
    return env.emit<LongBinaryOp>(op, lhs, rhs, *instr->frameState());
  }

  if (lhs->isA(TFloatExact) && rhs->isA(TFloatExact) &&
      ((instr->op() == BinaryOpKind::kPower) ||
       FloatBinaryOp::slotMethod(instr->op()))) {
    env.emit<UseType>(lhs, TFloatExact);
    env.emit<UseType>(rhs, TFloatExact);
    return env.emit<FloatBinaryOp>(instr->op(), lhs, rhs, *instr->frameState());
  }

  if ((lhs->isA(TUnicodeExact) && rhs->isA(TLongExact)) &&
      (op == BinaryOpKind::kMultiply)) {
    Register* unboxed_rhs = env.emit<IndexUnbox>(rhs, PyExc_OverflowError);
    env.emit<IsNegativeAndErrOccurred>(unboxed_rhs, *instr->frameState());
    return env.emit<UnicodeRepeat>(lhs, unboxed_rhs, *instr->frameState());
  }

  if ((lhs->isA(TUnicodeExact) && rhs->isA(TUnicodeExact)) &&
      (op == BinaryOpKind::kAdd)) {
    return env.emit<UnicodeConcat>(lhs, rhs, *instr->frameState());
  }

  // Unsupported case.
  return nullptr;
}

Register* simplifyInPlaceOp(Env& env, const InPlaceOp* instr) {
  Register* lhs = instr->left();
  Register* rhs = instr->right();
  if (lhs->isA(TLongExact) && rhs->isA(TLongExact)) {
    // All binary ops on TLong's return mutable so can be freely simplified with
    // no explicit check.
    switch (instr->op()) {
      case InPlaceOpKind::kAdd:
      case InPlaceOpKind::kAnd:
      case InPlaceOpKind::kFloorDivide:
      case InPlaceOpKind::kLShift:
      case InPlaceOpKind::kModulo:
      case InPlaceOpKind::kMultiply:
      case InPlaceOpKind::kOr:
      case InPlaceOpKind::kRShift:
      case InPlaceOpKind::kSubtract:
      case InPlaceOpKind::kXor:
      case InPlaceOpKind::kPower:
      case InPlaceOpKind::kTrueDivide:
        env.emit<UseType>(lhs, TLongExact);
        env.emit<UseType>(rhs, TLongExact);
        return env.emit<LongInPlaceOp>(
            instr->op(), lhs, rhs, *instr->frameState());
      case InPlaceOpKind::kMatrixMultiply:
        // These will generate an error at runtime.
        break;
    }
  }
  return nullptr;
}

Register* simplifyLongBinaryOp(Env& env, const LongBinaryOp* instr) {
  // This isn't safe in the multi-threaded compilation on 3.12 because
  // we don't hold the GIL which is required for allocation.
  RETURN_MULTITHREADED_COMPILE(nullptr);

  Type left_type = instr->left()->type();
  Type right_type = instr->right()->type();
  if (left_type.hasObjectSpec() && right_type.hasObjectSpec()) {
    ThreadedCompileSerialize guard;
    Ref<> result;
    if (instr->op() == BinaryOpKind::kPower) {
      result = Ref<>::steal(PyLong_Type.tp_as_number->nb_power(
          left_type.objectSpec(), right_type.objectSpec(), Py_None));
    } else {
      binaryfunc helper = instr->slotMethod();
      result = Ref<>::steal(
          (*helper)(left_type.objectSpec(), right_type.objectSpec()));
    }
    if (result == nullptr) {
      PyErr_Clear();
      return nullptr;
    }
    env.emit<UseType>(instr->left(), left_type);
    env.emit<UseType>(instr->right(), right_type);
    return env.emit<LoadConst>(
        Type::fromObject(env.func.env.addReference(std::move(result))));
  }
  return nullptr;
}

Register* simplifyFloatBinaryOp(Env& env, const FloatBinaryOp* instr) {
  // This isn't safe in the multi-threaded compilation on 3.12 because
  // we don't hold the GIL which is required for allocation.
  RETURN_MULTITHREADED_COMPILE(nullptr);

  Type left_type = instr->left()->type();
  Type right_type = instr->right()->type();

  if (!left_type.hasObjectSpec() || !right_type.hasObjectSpec()) {
    return nullptr;
  }

  ThreadedCompileSerialize guard;
  Ref<> result;

  if (instr->op() == BinaryOpKind::kPower) {
    result = Ref<>::steal(PyFloat_Type.tp_as_number->nb_power(
        left_type.objectSpec(), right_type.objectSpec(), Py_None));
  } else {
    binaryfunc helper = instr->slotMethod();
    result = Ref<>::steal(
        (*helper)(left_type.objectSpec(), right_type.objectSpec()));
  }

  if (result == nullptr) {
    PyErr_Clear();
    return nullptr;
  }

  env.emit<UseType>(instr->left(), left_type);
  env.emit<UseType>(instr->right(), right_type);
  return env.emit<LoadConst>(
      Type::fromObject(env.func.env.addReference(result)));
}

Register* simplifyUnaryOp(Env& env, const UnaryOp* instr) {
  Register* operand = instr->operand();

  if (instr->op() == UnaryOpKind::kNot && operand->isA(TBool)) {
    env.emit<UseType>(operand, TBool);
    Register* unboxed = env.emit<PrimitiveUnbox>(operand, TCBool);
    Register* negated =
        env.emit<PrimitiveUnaryOp>(PrimitiveUnaryOpKind::kNotInt, unboxed);
    return env.emit<PrimitiveBoxBool>(negated);
  }

  return nullptr;
}

Register* simplifyPrimitiveCompare(Env& env, const PrimitiveCompare* instr) {
  Register* left = instr->GetOperand(0);
  Register* right = instr->GetOperand(1);
  if (instr->op() == PrimitiveCompareOp::kEqual ||
      instr->op() == PrimitiveCompareOp::kNotEqual) {
    auto do_cbool = [&](bool value) {
      env.emit<UseType>(left, left->type());
      env.emit<UseType>(right, right->type());
      return env.emit<LoadConst>(Type::fromCBool(
          instr->op() == PrimitiveCompareOp::kNotEqual ? !value : value));
    };
    if (!left->type().couldBe(right->type())) {
      return do_cbool(false);
    }
    if (left->type().hasIntSpec() && right->type().hasIntSpec()) {
      return do_cbool(left->type().intSpec() == right->type().intSpec());
    }
    if (left->type().hasObjectSpec() && right->type().hasObjectSpec()) {
      return do_cbool(left->type().objectSpec() == right->type().objectSpec());
    }
  }
  // box(b) == True --> b
  if (instr->op() == PrimitiveCompareOp::kEqual &&
      left->instr()->IsPrimitiveBoxBool() &&
      right->type().asObject() == Py_True) {
    return left->instr()->GetOperand(0);
  }
  return nullptr;
}

Register* simplifyPrimitiveBoxBool(Env& env, const PrimitiveBoxBool* instr) {
  Register* input = instr->GetOperand(0);
  if (input->type().hasIntSpec()) {
    env.emit<UseType>(input, input->type());
    auto bool_obj = input->type().intSpec() ? Py_True : Py_False;
    return env.emit<LoadConst>(Type::fromObject(bool_obj));
  }
  return nullptr;
}

Register* simplifyUnbox(Env& env, const Instr* instr) {
  Register* input_value = instr->GetOperand(0);
  Type output_type = instr->output()->type();
  if (input_value->instr()->IsPrimitiveBox()) {
    // Simplify unbox(box(x)) -> x
    const auto box = static_cast<PrimitiveBox*>(input_value->instr());
    if (box->type() == output_type) {
      // We can't optimize away the potential overflow in unboxing.
      return box->GetOperand(0);
    }
  }
  // Ensure that we are dealing with either a integer or a double.
  Type input_value_type = input_value->type();
  if (!(input_value_type.hasObjectSpec())) {
    return nullptr;
  }
  PyObject* value = input_value_type.objectSpec();
  if (output_type <= (TCSigned | TCUnsigned)) {
    if (!PyLong_Check(value)) {
      return nullptr;
    }
    int overflow = 0;
    long number =
        PyLong_AsLongAndOverflow(input_value_type.objectSpec(), &overflow);
    if (overflow != 0) {
      return nullptr;
    }
    if (output_type <= TCSigned) {
      if (!Type::CIntFitsType(number, output_type)) {
        return nullptr;
      }
      return env.emit<LoadConst>(Type::fromCInt(number, output_type));
    } else {
      if (!Type::CUIntFitsType(number, output_type)) {
        return nullptr;
      }
      return env.emit<LoadConst>(Type::fromCUInt(number, output_type));
    }
  } else if (output_type <= TCDouble) {
    if (!PyFloat_Check(value)) {
      return nullptr;
    }
    double number = PyFloat_AS_DOUBLE(input_value_type.objectSpec());
    return env.emit<LoadConst>(Type::fromCDouble(number));
  }
  return nullptr;
}

// Attempt to simplify the given LoadAttr to a split dict load. Assumes various
// sanity checks have already passed:
// - The receiver has a known, exact type.
// - The type has a valid version tag.
// - The type doesn't have a descriptor at the attribute name.
Register* simplifyLoadAttrSplitDict(
    Env& env,
    const LoadAttr* load_attr,
    BorrowedRef<PyTypeObject> type,
    BorrowedRef<PyUnicodeObject> name) {
#if PY_VERSION_HEX >= 0x030C0000
  if (!PyType_HasFeature(type, Py_TPFLAGS_MANAGED_DICT)) {
    return nullptr;
  }
#else
  if (!PyType_HasFeature(type, Py_TPFLAGS_HEAPTYPE) ||
      type->tp_dictoffset < 0) {
    return nullptr;
  }
#endif
  BorrowedRef<PyHeapTypeObject> ht(type);
  if (ht->ht_cached_keys == nullptr) {
    return nullptr;
  }
  PyDictKeysObject* keys = ht->ht_cached_keys;
  Py_ssize_t attr_idx = getDictKeysIndex(keys, name);
  if (attr_idx == -1) {
    return nullptr;
  }

  Register* receiver = load_attr->GetOperand(0);
  auto patchpoint = env.emitInstr<DeoptPatchpoint>(
      Runtime::get()->allocateDeoptPatcher<SplitDictDeoptPatcher>(
          type, name, keys));
  patchpoint->setGuiltyReg(receiver);
  patchpoint->setDescr("SplitDictDeoptPatcher");
  env.emit<UseType>(receiver, receiver->type());

#if PY_VERSION_HEX >= 0x030C0000
  // PyDictOrValues is stored at -3 per _PyObject_DictOrValuesPointer
  Register* obj_dict = env.emit<LoadField>(
      receiver, "__dict__", -3 * sizeof(PyObject*), TOptDict);
#else
  Register* obj_dict =
      env.emit<LoadField>(receiver, "__dict__", type->tp_dictoffset, TOptDict);
#endif
  // We pass the attribute's name to this CheckField (not "__dict__") because
  // ultimately it means that the attribute we're trying to load is missing,
  // and the AttributeError to be raised should contain the attribute's name.
  Register* checked_dict =
      env.emit<CheckField>(obj_dict, name, *load_attr->frameState());
  static_cast<CheckField*>(checked_dict->instr())->setGuiltyReg(receiver);

#if PY_VERSION_HEX >= 0x030C0000
  Register* one = env.emit<LoadConst>(Type::fromCUInt(1, TCUInt64));
  Register* dict_ptr = env.emit<BitCast>(checked_dict, TCUInt64);
  Register* is_values =
      env.emit<IntBinaryOp>(BinaryOpKind::kAnd, dict_ptr, one);
  auto guard = env.emitInstr<Guard>(is_values);
  guard->setGuiltyReg(receiver);
  guard->setDescr("dict values check");
  Register* values = env.emit<IntBinaryOp>(BinaryOpKind::kAdd, dict_ptr, one);
  Register* values_obj = env.emit<BitCast>(values, TOptObject);
  Register* attr = env.emit<LoadField>(
      values_obj, "attr", attr_idx * sizeof(PyObject*), TOptObject);
#else
  Register* dict_keys = env.emit<LoadField>(
      checked_dict, "ma_keys", offsetof(PyDictObject, ma_keys), TCPtr);
  Register* expected_keys = env.emit<LoadConst>(Type::fromCPtr(keys));
  Register* equal = env.emit<PrimitiveCompare>(
      PrimitiveCompareOp::kEqual, dict_keys, expected_keys);
  auto guard = env.emitInstr<Guard>(equal);
  guard->setGuiltyReg(receiver);
  guard->setDescr("ht_cached_keys comparison");
  Register* attr = env.emit<LoadSplitDictItem>(checked_dict, attr_idx);
#endif

  Register* checked_attr =
      env.emit<CheckField>(attr, name, *load_attr->frameState());
  static_cast<CheckField*>(checked_attr->instr())->setGuiltyReg(receiver);

  return checked_attr;
}

// For LoadAttr instructions that resolve to a descriptor, DescrInfo holds
// unpacked state that's used by a number of different simplification cases.
struct DescrInfo {
  FrameState* frame_state;
  Register* receiver;
  Type type;
  BorrowedRef<PyTypeObject> py_type;
  BorrowedRef<PyUnicodeObject> attr_name;
  BorrowedRef<> descr;
};

void emitTypeAttrDeoptPatcher(
    Env& env,
    const DescrInfo& info,
    const char* description) {
  if (_PyClassLoader_IsImmutable(info.py_type)) {
    return;
  }

  // The descriptor could be from a base type, but PyType_Modified() also
  // notifies subtypes of the modified type, so we only have to watch the
  // object's type.
  auto patchpoint = env.emitInstr<DeoptPatchpoint>(
      Runtime::get()->allocateDeoptPatcher<TypeAttrDeoptPatcher>(
          info.py_type, info.attr_name, info.descr));
  patchpoint->setGuiltyReg(info.receiver);
  patchpoint->setDescr(description);
}

Register* simplifyLoadAttrMemberDescr(Env& env, const DescrInfo& info) {
  if (Py_TYPE(info.descr) != &PyMemberDescr_Type) {
    return nullptr;
  }

  // PyMemberDescrs are data descriptors, so we don't need to check if the
  // instance dictionary overrides the descriptor.
  PyMemberDef* def =
      reinterpret_cast<PyMemberDescrObject*>(info.descr.get())->d_member;
  if (def->flags & READ_RESTRICTED) {
    // This should be rare and requires raising an audit event; see
    // Objects/descrobject.c:member_get().
    return nullptr;
  }

  if (def->type == T_OBJECT || def->type == T_OBJECT_EX) {
    const char* name_cstr = PyUnicode_AsUTF8(info.attr_name);
    if (name_cstr == nullptr) {
      PyErr_Clear();
      name_cstr = "<unknown>";
    }
    emitTypeAttrDeoptPatcher(env, info, "member descriptor attribute");
    env.emit<UseType>(info.receiver, info.type);
    Register* field =
        env.emit<LoadField>(info.receiver, name_cstr, def->offset, TOptObject);
    if (def->type == T_OBJECT_EX) {
      auto check_field =
          env.emitInstr<CheckField>(field, info.attr_name, *info.frame_state);
      check_field->setGuiltyReg(info.receiver);
      return check_field->output();
    }

    return env.emitCond(
        [&](BasicBlock* bb1, BasicBlock* bb2) {
          env.emit<CondBranch>(field, bb1, bb2);
        },
        [&] { // Field is set
          return env.emit<RefineType>(TObject, field);
        },
        [&] { // Field is nullptr
          return env.emit<LoadConst>(TNoneType);
        });
  }
  return nullptr;
}

Register* simplifyLoadAttrProperty(Env& env, const DescrInfo& info) {
  if (Py_TYPE(info.descr) != &PyProperty_Type) {
    return nullptr;
  }
  auto property = reinterpret_cast<Ci_propertyobject*>(info.descr.get());
  BorrowedRef<> getter = property->prop_get;
  if (getter == nullptr) {
    return nullptr;
  }

  emitTypeAttrDeoptPatcher(env, info, "property attribute");
  env.emit<UseType>(info.receiver, info.type);
  Register* getter_obj = env.emit<LoadConst>(Type::fromObject(getter));
  auto call = env.emitRawInstr<VectorCall>(
      2, env.func.env.AllocateRegister(), CallFlags::None, *info.frame_state);
  call->SetOperand(0, getter_obj);
  call->SetOperand(1, info.receiver);
  return call->output();
}

Register* simplifyLoadAttrGenericDescriptor(Env& env, const DescrInfo& info) {
  BorrowedRef<PyTypeObject> descr_type = Py_TYPE(info.descr);
  descrgetfunc descr_get = descr_type->tp_descr_get;
  descrsetfunc descr_set = descr_type->tp_descr_set;
  if (descr_get == nullptr || descr_set == nullptr) {
    return nullptr;
  }

  emitTypeAttrDeoptPatcher(env, info, "generic descriptor attribute");
  if (!_PyClassLoader_IsImmutable(descr_type)) {
    // We unfortunately have to use a generic TypeDeoptPatcher here that
    // patches on any changes to the type, since type_setattro() calls
    // PyType_Modified() before updating tp_descr_{get,set}.
    auto patchpoint = env.emitInstr<DeoptPatchpoint>(
        Runtime::get()->allocateDeoptPatcher<TypeDeoptPatcher>(descr_type));
    patchpoint->setGuiltyReg(info.receiver);
    patchpoint->setDescr("tp_descr_get/tp_descr_set");
  }
  env.emit<UseType>(info.receiver, info.type);
  Register* descr_reg = env.emit<LoadConst>(Type::fromObject(info.descr));
  Register* type_reg = env.emit<LoadConst>(Type::fromObject(info.py_type));
  auto call = env.emitRawInstr<CallStatic>(
      3,
      env.func.env.AllocateRegister(),
      reinterpret_cast<void*>(descr_get),
      TOptObject);
  call->SetOperand(0, descr_reg);
  call->SetOperand(1, info.receiver);
  call->SetOperand(2, type_reg);
  return env.emit<CheckExc>(call->output(), *info.frame_state);
}

// Attempt to handle LOAD_ATTR cases where the load is a common case for object
// instances (not types).
Register* simplifyLoadAttrInstanceReceiver(
    Env& env,
    const LoadAttr* load_attr) {
  Register* receiver = load_attr->GetOperand(0);
  Type type = receiver->type();
  BorrowedRef<PyTypeObject> py_type{type.runtimePyType()};

  if (!type.isExact() || py_type == nullptr ||
      !PyType_HasFeature(py_type, Py_TPFLAGS_READY) ||
      py_type->tp_getattro != PyObject_GenericGetAttr) {
    return nullptr;
  }
  if (getThreadedCompileContext().compileRunning()) {
    // Calling ensureVersionTag() in 3.12+ doesn't work during multi-threaded
    // compile as it wants to access tstate.
    if (!PyType_HasFeature(py_type, Py_TPFLAGS_VALID_VERSION_TAG)) {
      return nullptr;
    }
  } else if (!ensureVersionTag(py_type)) {
    return nullptr;
  }

  BorrowedRef<PyUnicodeObject> attr_name{load_attr->name()};
  if (!PyUnicode_CheckExact(attr_name)) {
    return nullptr;
  }

  BorrowedRef<> descr{typeLookupSafe(py_type, attr_name)};
  if (descr == nullptr) {
    return simplifyLoadAttrSplitDict(env, load_attr, py_type, attr_name);
  }

  DescrInfo info{
      load_attr->frameState(), receiver, type, py_type, attr_name, descr};
  auto descr_funcs = {
      simplifyLoadAttrMemberDescr,
      simplifyLoadAttrProperty,
      simplifyLoadAttrGenericDescriptor,
  };
  for (auto func : descr_funcs) {
    if (Register* reg = func(env, info)) {
      return reg;
    }
  }
  return nullptr;
}

Register* simplifyLoadAttrTypeReceiver(Env& env, const LoadAttr* load_attr) {
  Register* receiver = load_attr->GetOperand(0);
  if (!receiver->isA(TType)) {
    return nullptr;
  }

  const int cache_id = env.func.env.allocateLoadTypeAttrCache();
  env.emit<UseType>(receiver, TType);
  Register* guard = env.emit<LoadTypeAttrCacheEntryType>(cache_id);
  Register* type_matches =
      env.emit<PrimitiveCompare>(PrimitiveCompareOp::kEqual, guard, receiver);
  return env.emitCond(
      [&](BasicBlock* fast_path, BasicBlock* slow_path) {
        env.emit<CondBranch>(type_matches, fast_path, slow_path);
      },
      [&] { // Fast path
        return env.emit<LoadTypeAttrCacheEntryValue>(cache_id);
      },
      [&] { // Slow path
        int name_idx = load_attr->name_idx();
        return env.emit<FillTypeAttrCache>(
            receiver, name_idx, cache_id, *load_attr->frameState());
      });
}

Register* simplifyLoadAttr(Env& env, const LoadAttr* load_attr) {
  if (Register* reg = simplifyLoadAttrInstanceReceiver(env, load_attr)) {
    return reg;
  }
  if (getConfig().attr_caches) {
    Register* receiver = load_attr->GetOperand(0);
    Type ty = receiver->type();
    BorrowedRef<PyTypeObject> type{ty.runtimePyType()};

    if (type == &PyModule_Type || type == &Ci_StrictModule_Type) {
      return env.emit<LoadModuleAttrCached>(
          load_attr->GetOperand(0),
          load_attr->name_idx(),
          *load_attr->frameState());
    }

    if (Register* reg = simplifyLoadAttrTypeReceiver(env, load_attr)) {
      return reg;
    }
    return env.emit<LoadAttrCached>(
        load_attr->GetOperand(0),
        load_attr->name_idx(),
        *load_attr->frameState());
  }
  return nullptr;
}

// If we're loading ob_fval from a known float into a double, this can be
// simplified into a LoadConst.
Register* simplifyLoadField(Env& env, const LoadField* instr) {
  Register* loadee = instr->GetOperand(0);
  Type load_output_type = instr->output()->type();
  // Ensure that we are dealing with either a integer or a double.
  Type loadee_type = loadee->type();
  if (!loadee_type.hasObjectSpec()) {
    return nullptr;
  }
  PyObject* value = loadee_type.objectSpec();
  if (PyFloat_Check(value) && load_output_type <= TCDouble &&
      instr->offset() == offsetof(PyFloatObject, ob_fval)) {
    double number = PyFloat_AS_DOUBLE(loadee_type.objectSpec());
    env.emit<UseType>(loadee, loadee_type);
    return env.emit<LoadConst>(Type::fromCDouble(number));
  }
  return nullptr;
}

Register* simplifyIsNegativeAndErrOccurred(
    Env& env,
    const IsNegativeAndErrOccurred* instr) {
  if (!instr->GetOperand(0)->instr()->IsLoadConst()) {
    return nullptr;
  }
  // Other optimizations might reduce the strength of global loads, etc. to load
  // consts. If this is the case, we know that there can't be an active
  // exception. In this case, the IsNegativeAndErrOccurred instruction has a
  // known result. Instead of deleting it, we replace it with load of false -
  // the idea is that if there are other downstream consumers of it, they will
  // still have access to the result. Otherwise, DCE will take care of this.
  Type output_type = instr->output()->type();
  return env.emit<LoadConst>(Type::fromCInt(0, output_type));
}

Register* simplifyStoreAttr(Env& env, const StoreAttr* store_attr) {
  if (getConfig().attr_caches) {
    return env.emit<StoreAttrCached>(
        store_attr->GetOperand(0),
        store_attr->GetOperand(1),
        store_attr->name_idx(),
        *store_attr->frameState());
  }
  return nullptr;
}

static bool isBuiltin(PyMethodDef* meth, const char* name) {
  // To make sure we have the right function, look up the PyMethodDef in the
  // fixed builtins. Any joker can make a new C method called "len", for
  // example.
  const Builtins& builtins = Runtime::get()->builtins();
  return builtins.find(meth) == name;
}

static bool isBuiltin(Register* callable, const char* name) {
  Type callable_type = callable->type();
  if (!callable_type.hasObjectSpec()) {
    return false;
  }
  PyObject* callable_obj = callable_type.objectSpec();
  if (Py_TYPE(callable_obj) == &PyCFunction_Type) {
    PyCFunctionObject* func =
        reinterpret_cast<PyCFunctionObject*>(callable_obj);
    return isBuiltin(func->m_ml, name);
  }
  if (Py_TYPE(callable_obj) == &PyMethodDescr_Type) {
    PyMethodDescrObject* meth =
        reinterpret_cast<PyMethodDescrObject*>(callable_obj);
    return isBuiltin(meth->d_method, name);
  }
  return false;
}

// This is inspired by _PyEval_EvalCodeWithName in 3.8's Python/ceval.c
// We have a vector of Register* (resolved_args) that gets populated with
// already-provided arguments from call instructions alongside the function's
// default arguments, when such defaults are needed
static Register* resolveArgs(
    Env& env,
    const VectorCall* instr,
    BorrowedRef<PyFunctionObject> target) {
  BorrowedRef<PyCodeObject> code{target->func_code};
  JIT_CHECK(!(code->co_flags & CO_VARARGS), "can't resolve varargs");
  // number of positional args (including args with default values)
  size_t co_argcount = static_cast<size_t>(code->co_argcount);
  if (instr->numArgs() > co_argcount) {
    // TASK(T143644311): support varargs and check if non-varargs here
    return nullptr;
  }

  size_t num_positional = std::min(co_argcount, instr->numArgs());
  std::vector<Register*> resolved_args(co_argcount, nullptr);

  JIT_CHECK(!(code->co_flags & CO_VARKEYWORDS), "can't resolve varkwargs");

  // grab default positional arguments
  BorrowedRef<PyTupleObject> defaults{target->func_defaults};

  // TASK(T143644350): support kwargs and kwdefaults
  size_t num_defaults =
      defaults == nullptr ? 0 : static_cast<size_t>(PyTuple_GET_SIZE(defaults));

  if (num_positional + num_defaults < co_argcount) {
    // function was called with too few arguments
    return nullptr;
  }
  // TASK(T143644377): support kwonly args
  JIT_CHECK(code->co_kwonlyargcount == 0, " can't resolve kwonly args");
  for (size_t i = 0; i < co_argcount; i++) {
    if (i < num_positional) {
      resolved_args[i] = instr->arg(i);
    } else {
      size_t num_non_defaults = co_argcount - num_defaults;
      size_t default_idx = i - num_non_defaults;

      ThreadedCompileSerialize guard;
      auto def = PyTuple_GET_ITEM(defaults, default_idx);
      JIT_CHECK(def != nullptr, "expected non-null default");
      auto type = Type::fromObject(env.func.env.addReference(def));
      resolved_args[i] = env.emit<LoadConst>(type);
    }
    JIT_CHECK(resolved_args.at(i) != nullptr, "expected non-null arg");
  }

  Register* defaults_obj = env.emit<LoadField>(
      instr->GetOperand(0),
      "func_defaults",
      offsetof(PyFunctionObject, func_defaults),
      TTuple);
  env.emit<GuardIs>(defaults, defaults_obj);
  // creates an instruction VectorCall(arg_size, dest_reg, frame_state)
  // and inserts it to the current block. Returns the output of vectorcall
  auto new_instr = env.emitRawInstr<VectorCall>(
      resolved_args.size() + 1,
      env.func.env.AllocateRegister(), // output register
      CallFlags::None,
      *instr->frameState());
  Register* result = new_instr->output();

  // populate the call arguments of the newly created VectorCall
  // the first arg is the function to call
  new_instr->SetOperand(0, instr->func());
  for (size_t i = 0; i < resolved_args.size(); i++) {
    new_instr->SetOperand(i + 1, resolved_args.at(i));
  }
  result->set_type(outputType(*new_instr));
  return result;
}

Register* simplifyCallMethod(Env& env, const CallMethod* instr) {
  // If this is statically known to be trying to call a function, update to
  // using a VectorCall directly.
  if (instr->func()->type() <= TNullptr) {
    auto call = env.emitRawInstr<VectorCall>(
        instr->NumOperands() - 1,
        env.func.env.AllocateRegister(),
        instr->flags(),
        *instr->frameState());
    for (size_t i = 1; i < instr->NumOperands(); ++i) {
      call->SetOperand(i - 1, instr->GetOperand(i));
    }
    return call->output();
  }

  return nullptr;
}

// Translate VectorCall to CallStatic whenever possible, saving stack
// manipulation costs (pushing args to stack).
static Register* trySpecializeCCall(Env& env, const VectorCall* instr) {
  if (instr->flags() & CallFlags::Awaited) {
    // We can't pass the awaited flag outside of vectorcall.
    return nullptr;
  }
  Register* callable = instr->func();
  Type callable_type = callable->type();
  PyObject* callable_obj = callable_type.asObject();
  if (callable_obj == nullptr) {
    return nullptr;
  }

  // Non METH_STATIC and METH_CLASS tp_methods on types are stored as
  // PyMethodDescr inside tp_dict. Check out:
  // Objects/typeobject.c#type_add_method
  if (Py_TYPE(callable_obj) == &PyMethodDescr_Type) {
    auto meth = reinterpret_cast<PyMethodDescrObject*>(callable_obj);
    PyMethodDef* def = meth->d_method;
    if (def->ml_flags & METH_NOARGS && instr->numArgs() == 1) {
      Register* result = env.emitVariadic<CallStatic>(
          1,
          reinterpret_cast<void*>(def->ml_meth),
          instr->output()->type() | TNullptr,
          /* self */ instr->arg(0));
      return env.emit<CheckExc>(result, *instr->frameState());
    }
    if (def->ml_flags & METH_O && instr->numArgs() == 2) {
      Register* result = env.emitVariadic<CallStatic>(
          2,
          reinterpret_cast<void*>(def->ml_meth),
          instr->output()->type() | TNullptr,
          /* self */ instr->arg(0),
          /* arg */ instr->arg(1));
      return env.emit<CheckExc>(result, *instr->frameState());
    }
  }
  return nullptr;
}

Register* simplifyVectorCallStatic(Env& env, const VectorCall* instr) {
  if (!(instr->flags() & CallFlags::Static)) {
    return nullptr;
  }
  Register* func = instr->func();
  if (isBuiltin(func, "list.append") && instr->numArgs() == 2) {
    env.emit<UseType>(func, func->type());
    env.emit<ListAppend>(instr->arg(0), instr->arg(1), *instr->frameState());
    return env.emit<LoadConst>(TNoneType);
  }

  return trySpecializeCCall(env, instr);
}

// Special case here where we are testing `if isinstance`. In that case we do
// not want to go through the boxing and then unboxing that we are about to do.
// Instead, we want to directly provide the result of the unboxed comparison.
std::optional<std::pair<Instr*, std::vector<Instr*>>> isVectorCallIfIsInstance(
    Env& env,
    const VectorCall* instr) {
  std::vector<Instr*> snapshots;

  LivenessAnalysis::LastUses last_uses;
  Register* output = nullptr;

  enum state { kInitial, kCondBranch, kIsTruthy, kFailed };
  auto state = kInitial;

  auto block = instr->block();
  for (auto current = block->rbegin();
       current != block->rend() && state != kFailed;
       ++current) {
    switch (state) {
      case kInitial: {
        if (!current->IsCondBranch()) {
          state = kFailed;
          break;
        }

        LivenessAnalysis analysis{env.func};
        analysis.Run();

        last_uses = analysis.GetLastUses();
        auto lu_at_condbranch = last_uses.find(&*current);
        if (lu_at_condbranch == last_uses.end() ||
            lu_at_condbranch->second.size() != 1) {
          // If the CondBranch instruction is not the last use of the
          // IsTruthy output, then we cannot perform this optimization.
          state = kFailed;
          break;
        }

        state = kCondBranch;
        output = current->GetOperand(0);
        break;
      }
      case kCondBranch: {
        if (current->IsIsTruthy() && output == current->output() &&
            current->GetOperand(0) == instr->output()) {
          auto lu_at_istruthy = last_uses.find(&*current);
          if (lu_at_istruthy == last_uses.end() ||
              lu_at_istruthy->second.size() != 1) {
            // If the IsTruthy instruction is not the last use of the VectorCall
            // output, then we cannot perform this optimization.
            state = kFailed;
          } else {
            state = kIsTruthy;
          }
          break;
        }

        if (current->IsSnapshot()) {
          snapshots.push_back(&*current);
          break;
        }

        state = kFailed;
        break;
      }
      case kIsTruthy: {
        if (&*current == instr) {
          JIT_CHECK(output != nullptr, "output should have been set");
          return std::make_optional(std::make_pair(output->instr(), snapshots));
        }

        if (current->IsSnapshot()) {
          // Leave these snapshots in place.
          break;
        }

        state = kFailed;
        break;
      }
      case kFailed:
        JIT_ABORT("Hit kFailed state but it should not be reachable");
    }
  }

  // If we found anything else between the VectorCall, IsTruthy, and CondBranch
  // besides the expected instructions and some snapshots, then we cannot
  // perform this optimization.
  return std::nullopt;
}

Register* simplifyVectorCall(Env& env, const VectorCall* instr) {
  if (Register* result = simplifyVectorCallStatic(env, instr)) {
    return result;
  }
  if (instr->flags() & CallFlags::KwArgs) {
    return nullptr;
  }

  Register* target = instr->GetOperand(0);
  Type target_type = target->type();
  if (target_type == env.type_object && instr->NumOperands() == 2) {
    env.emit<UseType>(target, env.type_object);
    return env.emit<LoadField>(
        instr->GetOperand(1), "ob_type", offsetof(PyObject, ob_type), TType);
  }
  if (isBuiltin(target, "len") && instr->numArgs() == 1) {
    env.emit<UseType>(target, target->type());
    return env.emit<GetLength>(instr->arg(0), *instr->frameState());
  }
  if (isBuiltin(target, "isinstance") && instr->numArgs() == 2 &&
      instr->GetOperand(2)->type() <= TType &&
      !(instr->GetOperand(2)->type() <= TTuple)) {
    auto obj_op = instr->GetOperand(1);
    auto type_op = instr->GetOperand(2);

    auto obj_type = env.emit<LoadField>(
        obj_op, "ob_type", offsetof(PyObject, ob_type), TType);

    auto compare_type = env.emit<PrimitiveCompare>(
        PrimitiveCompareOp::kEqual, obj_type, type_op);

    // If this is a VectorCall to isinstance and it's being used as the
    // predicate of an if statement, it will look like:
    //
    //     o1 = VectorCall
    //     o2 = IsTruthy o1
    //     CondBranch o2
    //
    // Below, this would then expand into boxing the bool on both sides of the
    // conditional, then unboxing it again to do another comparison. Instead, we
    // can circumvent that by directly using the result of the primitive
    // compare.
    auto data = isVectorCallIfIsInstance(env, instr);
    if (data.has_value()) {
      auto& [is_truthy, snapshots] = data.value();
      auto result = is_truthy->output();

      // We no longer need the IsTruthy instruction.
      is_truthy->unlink();
      delete is_truthy;

      // We also no longer need the Snapshot instructions contained between the
      // IsTruthy instruction and the CondBranch instruction.
      for (auto snapshot : snapshots) {
        snapshot->unlink();
        delete snapshot;
      }

      env.emitCondSlowPath(
          result,
          compare_type,
          [&](auto slow_path) {
            return env.emitInstr<CondBranch>(compare_type, nullptr, slow_path);
          },
          [&] {
            auto isinstance_call =
                env.emit<IsInstance>(obj_op, type_op, *instr->frameState());
            auto true_output = env.emit<LoadConst>(Type::fromCInt(1, TCInt32));
            return env.emit<PrimitiveCompare>(
                PrimitiveCompareOp::kEqual, isinstance_call, true_output);
          });

      // The output of the VectorCall instruction was previously a TBool, but we
      // are replacing it with a TCBool since we are now doing a primitive
      // compare instead. This works, but requires that we change the
      // instruction's output type to match in order to pass the assertions that
      // come after the call to simplifyInstr.
      instr->output()->set_type(TCBool);

      return result;
    }

    return env.emitCond(
        [&](BasicBlock* fast_path, BasicBlock* slow_path) {
          env.emit<CondBranch>(compare_type, fast_path, slow_path);
        },
        [&] { // Fast path
          return env.emit<PrimitiveBoxBool>(compare_type);
        },
        [&] { // Slow path
          auto isinstance_call =
              env.emit<IsInstance>(obj_op, type_op, *instr->frameState());
          auto true_output = env.emit<LoadConst>(Type::fromCInt(1, TCInt32));
          auto compare_output = env.emit<PrimitiveCompare>(
              PrimitiveCompareOp::kEqual, isinstance_call, true_output);
          return env.emit<PrimitiveBoxBool>(compare_output);
        });
  }
  if (target_type.hasValueSpec(TFunc)) {
    BorrowedRef<PyFunctionObject> func{target_type.objectSpec()};
    BorrowedRef<PyCodeObject> code{func->func_code};
    if (code->co_kwonlyargcount > 0 || (code->co_flags & CO_VARARGS) ||
        (code->co_flags & CO_VARKEYWORDS)) {
      // TASK(T143644854): full argument resolution
      return nullptr;
    }

    JIT_CHECK(
        code->co_argcount >= 0,
        "argcount must be greater than or equal to zero");
    if (instr->numArgs() != static_cast<size_t>(code->co_argcount)) {
      return resolveArgs(env, instr, func);
    }
  }
  return nullptr;
}

Register* simplifyStoreSubscr(Env& env, const StoreSubscr* instr) {
  if (instr->GetOperand(0)->isA(TDictExact)) {
    auto output = env.func.env.AllocateRegister();
    env.emitRawInstr<CallStatic>(
        3,
        output,
        reinterpret_cast<void*>(PyDict_Type.tp_as_mapping->mp_ass_subscript),
        TCInt32,
        instr->GetOperand(0),
        instr->GetOperand(1),
        instr->GetOperand(2));

    env.emit<CheckNeg>(output, *instr->frameState());
    return nullptr;
  }

  return nullptr;
}

Register* simplifyInstr(Env& env, const Instr* instr) {
  switch (instr->opcode()) {
    case Opcode::kCheckVar:
    case Opcode::kCheckExc:
    case Opcode::kCheckField:
      return simplifyCheck(static_cast<const CheckBase*>(instr));
    case Opcode::kCheckSequenceBounds:
      return simplifyCheckSequenceBounds(
          env, static_cast<const CheckSequenceBounds*>(instr));
    case Opcode::kGuardType:
      return simplifyGuardType(env, static_cast<const GuardType*>(instr));
    case Opcode::kRefineType:
      return simplifyRefineType(static_cast<const RefineType*>(instr));
    case Opcode::kCast:
      return simplifyCast(static_cast<const Cast*>(instr));

    case Opcode::kCompare:
      return simplifyCompare(env, static_cast<const Compare*>(instr));

    case Opcode::kCondBranch:
      return simplifyCondBranch(env, static_cast<const CondBranch*>(instr));
    case Opcode::kCondBranchCheckType:
      return simplifyCondBranchCheckType(
          env, static_cast<const CondBranchCheckType*>(instr));

    case Opcode::kGetLength:
      return simplifyGetLength(env, static_cast<const GetLength*>(instr));

    case Opcode::kIntConvert:
      return simplifyIntConvert(env, static_cast<const IntConvert*>(instr));

    case Opcode::kIsTruthy:
      return simplifyIsTruthy(env, static_cast<const IsTruthy*>(instr));

    case Opcode::kLoadAttr:
      return simplifyLoadAttr(env, static_cast<const LoadAttr*>(instr));
    case Opcode::kLoadMethod:
      return simplifyLoadMethod(env, static_cast<const LoadMethod*>(instr));
    case Opcode::kLoadField:
      return simplifyLoadField(env, static_cast<const LoadField*>(instr));
    case Opcode::kLoadTupleItem:
      return simplifyLoadTupleItem(
          env, static_cast<const LoadTupleItem*>(instr));
    case Opcode::kLoadArrayItem:
      return simplifyLoadArrayItem(
          env, static_cast<const LoadArrayItem*>(instr));
    case Opcode::kLoadVarObjectSize:
      return simplifyLoadVarObjectSize(
          env, static_cast<const LoadVarObjectSize*>(instr));

    case Opcode::kBinaryOp:
      return simplifyBinaryOp(env, static_cast<const BinaryOp*>(instr));
    case Opcode::kInPlaceOp:
      return simplifyInPlaceOp(env, static_cast<const InPlaceOp*>(instr));
    case Opcode::kLongBinaryOp:
      return simplifyLongBinaryOp(env, static_cast<const LongBinaryOp*>(instr));
    case Opcode::kFloatBinaryOp:
      return simplifyFloatBinaryOp(
          env, static_cast<const FloatBinaryOp*>(instr));
    case Opcode::kUnaryOp:
      return simplifyUnaryOp(env, static_cast<const UnaryOp*>(instr));

    case Opcode::kPrimitiveCompare:
      return simplifyPrimitiveCompare(
          env, static_cast<const PrimitiveCompare*>(instr));
    case Opcode::kPrimitiveBoxBool:
      return simplifyPrimitiveBoxBool(
          env, static_cast<const PrimitiveBoxBool*>(instr));
    case Opcode::kIndexUnbox:
    case Opcode::kPrimitiveUnbox:
      return simplifyUnbox(env, instr);

    case Opcode::kIsNegativeAndErrOccurred:
      return simplifyIsNegativeAndErrOccurred(
          env, static_cast<const IsNegativeAndErrOccurred*>(instr));

    case Opcode::kStoreAttr:
      return simplifyStoreAttr(env, static_cast<const StoreAttr*>(instr));

    case Opcode::kCallMethod:
      return simplifyCallMethod(env, static_cast<const CallMethod*>(instr));

    case Opcode::kVectorCall:
      return simplifyVectorCall(env, static_cast<const VectorCall*>(instr));

    case Opcode::kStoreSubscr:
      return simplifyStoreSubscr(env, static_cast<const StoreSubscr*>(instr));

    default:
      return nullptr;
  }
}

} // namespace

void Simplify::Run(Function& irfunc) {
  Env env{irfunc};

  const SimplifierConfig& config = getConfig().simplifier;
  size_t new_block_limit = config.new_block_limit;
  size_t iteration_limit = config.iteration_limit;

  // Iterate the simplifier until the CFG stops changing, or we hit limits on
  // total number of iterations or the number of new blocks added.
  bool changed = true;
  for (size_t i = 0;
       changed && i < iteration_limit && env.new_blocks < new_block_limit;
       ++i) {
    changed = false;
    for (auto cfg_it = irfunc.cfg.blocks.begin();
         cfg_it != irfunc.cfg.blocks.end();
         ++cfg_it) {
      BasicBlock& block = *cfg_it;
      env.block = &block;

      for (auto blk_it = block.begin(); blk_it != block.end();) {
        Instr& instr = *blk_it;
        ++blk_it;

        env.optimized = false;
        env.cursor = block.iterator_to(instr);
        env.bc_off = instr.bytecodeOffset();
        Register* new_output = simplifyInstr(env, &instr);
        JIT_CHECK(
            env.cursor == env.block->iterator_to(instr),
            "Simplify functions are expected to leave env.cursor pointing to "
            "the original instruction, with new instructions inserted before "
            "it.");
        if (new_output == nullptr && !env.optimized) {
          continue;
        }

        changed = true;
        JIT_CHECK(
            (new_output == nullptr) == (instr.output() == nullptr),
            "Simplify function should return a new output if and only if the "
            "existing instruction has an output");
        if (new_output != nullptr) {
          JIT_CHECK(
              new_output->type() <= instr.output()->type(),
              "New output type {} isn't compatible with old output type {}",
              new_output->type(),
              instr.output()->type());
          env.emitRawInstr<Assign>(instr.output(), new_output);
        }

        if (instr.IsCondBranch() || instr.IsCondBranchIterNotDone() ||
            instr.IsCondBranchCheckType()) {
          JIT_CHECK(env.cursor != env.block->begin(), "Unexpected empty block");
          Instr& prev_instr = *std::prev(env.cursor);
          JIT_CHECK(
              instr.opcode() == prev_instr.opcode() || prev_instr.IsBranch(),
              "The only supported simplification for CondBranch* is to a "
              "Branch or a different CondBranch, got unexpected '{}'",
              prev_instr);

          // If we've optimized a CondBranchBase into a Branch, we also need to
          // remove any Phi references to the current block from the block that
          // we no longer visit.
          if (prev_instr.IsBranch()) {
            auto cond = static_cast<CondBranchBase*>(&instr);
            BasicBlock* new_dst = prev_instr.successor(0);
            BasicBlock* old_branch_block = cond->false_bb() == new_dst
                ? cond->true_bb()
                : cond->false_bb();
            old_branch_block->removePhiPredecessor(cond->block());
          }
        }

        instr.unlink();
        delete &instr;

        if (env.block != &block) {
          // If we're now in a different block, `block' should only contain the
          // newly-emitted instructions, with no more old instructions to
          // process. Continue to the next block in the list; any newly-created
          // blocks were added to the end of the list and will be processed
          // later.
          break;
        }
      }

      // Check for going past the new block limit only upon leaving a block.  We
      // might go past the limit, but not by too much.
      if (env.new_blocks > new_block_limit) {
        break;
      }
    }

    if (changed) {
      // Perform some simple cleanup between each pass.
      CopyPropagation{}.Run(irfunc);
      reflowTypes(irfunc);
      CleanCFG{}.Run(irfunc);
    }
  }
}

} // namespace jit::hir

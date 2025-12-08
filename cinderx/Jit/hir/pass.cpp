// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/pass.h"

#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/runtime.h"

namespace jit::hir {

namespace {

std::optional<Type> builtinFunctionReturnType(std::string_view name) {
  static const UnorderedMap<std::string_view, Type> kRetTypes = {
      {"dict.copy", TDictExact},      {"hasattr", TBool},
      {"isinstance", TBool},          {"len", TLongExact},
      {"list.copy", TListExact},      {"list.count", TLongExact},
      {"list.index", TLongExact},     {"str.capitalize", TUnicodeExact},
      {"str.center", TUnicodeExact},  {"str.count", TLongExact},
      {"str.endswith", TBool},        {"str.find", TLongExact},
      {"str.format", TUnicodeExact},  {"str.index", TLongExact},
      {"str.isalnum", TBool},         {"str.isalpha", TBool},
      {"str.isascii", TBool},         {"str.isdecimal", TBool},
      {"str.isdigit", TBool},         {"str.isidentifier", TBool},
      {"str.islower", TBool},         {"str.isnumeric", TBool},
      {"str.isprintable", TBool},     {"str.isspace", TBool},
      {"str.istitle", TBool},         {"str.isupper", TBool},
      {"str.join", TUnicodeExact},    {"str.lower", TUnicodeExact},
      {"str.lstrip", TUnicodeExact},  {"str.partition", TTupleExact},
      {"str.replace", TUnicodeExact}, {"str.rfind", TLongExact},
      {"str.rindex", TLongExact},     {"str.rpartition", TTupleExact},
      {"str.rsplit", TListExact},     {"str.split", TListExact},
      {"str.splitlines", TListExact}, {"str.upper", TUnicodeExact},
      {"tuple.count", TLongExact},    {"tuple.index", TLongExact},
  };
  auto return_type = kRetTypes.find(name);
  if (return_type != kRetTypes.end()) {
    return return_type->second;
  }
  return std::nullopt;
}

Type returnType(PyMethodDef* meth) {
  // To make sure we have the right function, look up the PyMethodDef in the
  // fixed builtins. Any joker can make a new C method called "len", for
  // example.
  const Builtins& builtins = Runtime::get()->builtins();
  auto name = builtins.find(meth);
  if (!name.has_value()) {
    return TObject;
  }
  auto return_type = builtinFunctionReturnType(name.value());
  return return_type.has_value() ? *return_type : TObject;
}

Type returnType(Type callable) {
  if (!callable.hasObjectSpec()) {
    return TObject;
  }
  PyObject* callable_obj = callable.objectSpec();
  if (Py_TYPE(callable_obj) == &PyCFunction_Type) {
    PyCFunctionObject* func =
        reinterpret_cast<PyCFunctionObject*>(callable_obj);
    return returnType(func->m_ml);
  }
  if (Py_TYPE(callable_obj) == &PyMethodDescr_Type) {
    PyMethodDescrObject* meth =
        reinterpret_cast<PyMethodDescrObject*>(callable_obj);
    return returnType(meth->d_method);
  }
  if (Py_TYPE(callable_obj) == &PyType_Type) {
    Type result =
        Type::fromTypeExact(reinterpret_cast<PyTypeObject*>(callable_obj));
    if (result <= TBuiltinExact && !(result <= TType)) {
      return result;
    }
  }
  return TObject;
}

} // namespace

Register* chaseAssignOperand(Register* value) {
  while (value->instr()->IsAssign()) {
    value = value->instr()->GetOperand(0);
  }
  return value;
}

RegUses collectDirectRegUses(Function& func) {
  RegUses uses;
  for (auto& block : func.cfg.blocks) {
    for (Instr& instr : block) {
      for (size_t i = 0; i < instr.NumOperands(); ++i) {
        uses[instr.GetOperand(i)].insert(&instr);
      }
    }
  }
  return uses;
}

Type outputType(
    const Instr& instr,
    const std::function<Type(std::size_t)>& get_op_type) {
  switch (instr.opcode()) {
    case Opcode::kCallEx:
      return returnType(static_cast<const CallEx&>(instr).func()->type());
    case Opcode::kVectorCall:
      return returnType(static_cast<const VectorCall&>(instr).func()->type());
    case Opcode::kCallMethod:
      return returnType(static_cast<const CallMethod&>(instr).func()->type());

    case Opcode::kCompare: {
      CompareOp op = static_cast<const Compare&>(instr).op();
      if (op == CompareOp::kIn || op == CompareOp::kNotIn) {
        return TBool;
      }
      return TObject;
    }

    case Opcode::kInPlaceOp: {
      auto& op = static_cast<const InPlaceOp&>(instr);
      if (op.left()->type() <= TLongExact && op.right()->type() <= TLongExact) {
        switch (op.op()) {
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
            return TLongExact;
          case InPlaceOpKind::kMatrixMultiply:
            // Will be an error at runtime
            return TObject;
          case InPlaceOpKind::kPower:
            // Will be floating-point for negative exponents.
            return TLongExact | TFloatExact;
          case InPlaceOpKind::kTrueDivide:
            return TFloatExact;
        }
      }
      return TObject;
    }

    case Opcode::kBinaryOp: {
      auto& op = static_cast<const BinaryOp&>(instr);
      if (op.left()->type() <= TLongExact && op.right()->type() <= TLongExact) {
        switch (op.op()) {
          case BinaryOpKind::kAdd:
          case BinaryOpKind::kAnd:
          case BinaryOpKind::kFloorDivide:
          case BinaryOpKind::kFloorDivideUnsigned:
          case BinaryOpKind::kLShift:
          case BinaryOpKind::kModulo:
          case BinaryOpKind::kModuloUnsigned:
          case BinaryOpKind::kMultiply:
          case BinaryOpKind::kOr:
          case BinaryOpKind::kPowerUnsigned:
          case BinaryOpKind::kRShift:
          case BinaryOpKind::kRShiftUnsigned:
          case BinaryOpKind::kSubtract:
          case BinaryOpKind::kXor:
            return TLongExact;
          case BinaryOpKind::kPower:
            // Will be floating-point for negative exponents.
            return TLongExact | TFloatExact;
          case BinaryOpKind::kTrueDivide:
            return TFloatExact;
          case BinaryOpKind::kSubscript:
          case BinaryOpKind::kMatrixMultiply:
            // Will be an error at runtime
            return TObject;
        }
      }
      return TObject;
    }

    case Opcode::kBuildInterpolation:
    case Opcode::kBuildTemplate:
    case Opcode::kCallIntrinsic:
    case Opcode::kConvertValue:
    case Opcode::kDictSubscr:
    case Opcode::kEagerImportName:
    case Opcode::kFillTypeAttrCache:
    case Opcode::kFillTypeMethodCache:
    case Opcode::kGetAIter:
    case Opcode::kGetANext:
    case Opcode::kGetIter:
    case Opcode::kImportFrom:
    case Opcode::kImportName:
    case Opcode::kInvokeIterNext:
    case Opcode::kLoadAttr:
    case Opcode::kLoadAttrCached:
    case Opcode::kLoadAttrSpecial:
    case Opcode::kLoadAttrSuper:
    case Opcode::kLoadGlobal:
    case Opcode::kLoadMethod:
    case Opcode::kLoadMethodCached:
    case Opcode::kLoadMethodSuper:
    case Opcode::kLoadModuleAttrCached:
    case Opcode::kLoadModuleMethodCached:
    case Opcode::kLoadSpecial:
    case Opcode::kLoadTupleItem:
    case Opcode::kMatchKeys:
    case Opcode::kSend:
    case Opcode::kWaitHandleLoadCoroOrResult:
    case Opcode::kYieldAndYieldFrom:
    case Opcode::kYieldFrom:
    case Opcode::kYieldFromHandleStopAsyncIteration:
    case Opcode::kYieldValue:
      return TObject;
    case Opcode::kBuildString:
      return TMortalUnicode;
    case Opcode::kGetLength:
      return TLongExact;
    case Opcode::kCopyDictWithoutKeys:
      return TDictExact;
    case Opcode::kUnaryOp: {
      auto op = static_cast<const UnaryOp&>(instr).op();
      if (op == UnaryOpKind::kNot) {
        return TBool;
      }
      return TObject;
    }
    // Many opcodes just return a possibly-null PyObject*. Some of these will
    // be further specialized based on the input types in the hopefully near
    // future.
    case Opcode::kCallCFunc:
    case Opcode::kLoadCellItem:
    case Opcode::kLoadGlobalCached:
    case Opcode::kMatchClass:
    case Opcode::kStealCellItem:
    case Opcode::kWaitHandleLoadWaiter:
      return TOptObject;

    case Opcode::kGetSecondOutput: {
      return static_cast<const GetSecondOutput&>(instr).type();
    }

    case Opcode::kFormatValue:
    case Opcode::kFormatWithSpec:
      return TUnicode;

    case Opcode::kLoadVarObjectSize:
      return TCInt64;
    case Opcode::kInvokeStaticFunction:
      return static_cast<const InvokeStaticFunction&>(instr).ret_type();
    case Opcode::kLoadArrayItem:
      return static_cast<const LoadArrayItem&>(instr).type();
    case Opcode::kLoadSplitDictItem:
      return TOptObject;
    case Opcode::kLoadField:
      return static_cast<const LoadField&>(instr).type();
    case Opcode::kLoadFieldAddress:
      return TCPtr;
    case Opcode::kCallStatic: {
      auto& call = static_cast<const CallStatic&>(instr);
      return call.ret_type();
    }
    case Opcode::kCallInd: {
      auto& call = static_cast<const CallInd&>(instr);
      return call.ret_type();
    }
    case Opcode::kIntConvert: {
      auto& conv = static_cast<const IntConvert&>(instr);
      return conv.type();
    }
    case Opcode::kIntBinaryOp: {
      auto& binop = static_cast<const IntBinaryOp&>(instr);
      if (binop.op() == BinaryOpKind::kPower ||
          binop.op() == BinaryOpKind::kPowerUnsigned) {
        return TCDouble;
      }
      return binop.left()->type().unspecialized();
    }
    case Opcode::kDoubleBinaryOp: {
      return TCDouble;
    }
    case Opcode::kPrimitiveCompare:
      return TCBool;
    case Opcode::kPrimitiveUnaryOp:
      if (static_cast<const PrimitiveUnaryOp&>(instr).op() ==
          PrimitiveUnaryOpKind::kNotInt) {
        return TCBool;
      }
      return get_op_type(0).unspecialized();

    // Some return something slightly more interesting.
    case Opcode::kBuildSlice:
      return TMortalSlice;
    case Opcode::kGetTuple:
      return TTupleExact;
    case Opcode::kInitialYield:
      return TOptNoneType;
    case Opcode::kLoadArg: {
      auto& loadarg = static_cast<const LoadArg&>(instr);
      return loadarg.type();
    }
    case Opcode::kLoadCurrentFunc:
      return TFunc;
    case Opcode::kLoadEvalBreaker:
      return PY_VERSION_HEX >= 0x030D0000 ? TCInt64 : TCInt32;
    case Opcode::kMakeCell:
      return TMortalCell;
    case Opcode::kMakeDict:
      return TMortalDictExact;
    case Opcode::kMakeCheckedDict: {
      auto& makechkdict = static_cast<const MakeCheckedDict&>(instr);
      return makechkdict.type();
    }
    case Opcode::kMakeCheckedList: {
      auto& makechklist = static_cast<const MakeCheckedList&>(instr);
      return makechklist.type();
    }
    case Opcode::kMakeFunction:
      return TMortalFunc;
    case Opcode::kMakeSet:
      return TMortalSetExact;
    case Opcode::kLongBinaryOp: {
      auto& binop = static_cast<const LongBinaryOp&>(instr);
      if (binop.op() == BinaryOpKind::kTrueDivide) {
        return TFloatExact;
      }
      if (binop.op() == BinaryOpKind::kPower) {
        // Will be floating-point for negative exponents.
        return TFloatExact | TLongExact;
      }
      return TLongExact;
    }
    case Opcode::kLongInPlaceOp: {
      auto& inplaceop = static_cast<const LongInPlaceOp&>(instr);
      if (inplaceop.op() == InPlaceOpKind::kTrueDivide) {
        return TFloatExact;
      }
      if (inplaceop.op() == InPlaceOpKind::kPower) {
        // Will be floating-point for negative exponents.
        return TFloatExact | TLongExact;
      }
      return TLongExact;
    }
    case Opcode::kFloatBinaryOp:
      return TFloatExact;
    case Opcode::kFloatCompare:
    case Opcode::kLongCompare:
    case Opcode::kUnicodeCompare:
      return TBool;
    case Opcode::kDictUpdate:
    case Opcode::kDictMerge:
    case Opcode::kRunPeriodicTasks:
      return TCInt32;

    // These wrap C functions that return 0 for success and -1 for an error,
    // which is converted into Py_None or nullptr, respectively. At some point
    // we should get rid of this extra layer and deal with the int return value
    // directly.
    case Opcode::kListExtend:
      return TNoneType;

    case Opcode::kListAppend:
    case Opcode::kMergeSetUnpack:
    case Opcode::kSetSetItem:
    case Opcode::kSetUpdate:
    case Opcode::kSetDictItem:
      return TCInt32;

    case Opcode::kIsNegativeAndErrOccurred:
      return TCInt64;

      // Some compute their output type from either their inputs or some other
      // source.

    case Opcode::kLoadTypeAttrCacheEntryType:
      return TOptType;
    case Opcode::kLoadTypeAttrCacheEntryValue:
      // Only valid if guarded by a LoadTypeAttrCacheEntryType, which ensures
      // that this will return a non-null object.
      return TObject;
    case Opcode::kLoadTypeMethodCacheEntryType:
      return TOptType;
    case Opcode::kLoadTypeMethodCacheEntryValue:
      return TObject;
    case Opcode::kAssign:
      return get_op_type(0);
    case Opcode::kBitCast:
      return static_cast<const BitCast&>(instr).type();
    case Opcode::kLoadConst: {
      return static_cast<const LoadConst&>(instr).type();
    }
    case Opcode::kMakeList:
      return TMortalListExact;
    case Opcode::kMakeTuple:
    case Opcode::kMakeTupleFromList:
    case Opcode::kUnpackExToTuple:
      return TMortalTupleExact;
    case Opcode::kPhi: {
      auto ty = TBottom;
      for (std::size_t i = 0, n = instr.NumOperands(); i < n; ++i) {
        ty |= get_op_type(i);
      }
      return ty;
    }
    case Opcode::kCheckSequenceBounds: {
      return TCInt64;
    }

    case Opcode::kIsInstance:
    case Opcode::kCompareBool:
    case Opcode::kCIntToCBool:
    case Opcode::kIsTruthy: {
      return TCBool;
    }

    case Opcode::kLoadFunctionIndirect: {
      return TObject;
    }

    case Opcode::kPrimitiveBoxBool: {
      return TBool;
    }

    case Opcode::kPrimitiveBox: {
      // This duplicates the logic in Type::asBoxed(), but it has enough
      // special cases (for exactness/optionality/nullptr) that it's not worth
      // trying to reuse it here.

      auto& pb = static_cast<const PrimitiveBox&>(instr);
      if (pb.value()->type() <= TCDouble) {
        return TFloatExact;
      }
      if (pb.value()->type() <= (TCUnsigned | TCSigned | TNullptr)) {
        // Special Nullptr case for an uninitialized variable; load zero.
        return TLongExact;
      }
      JIT_ABORT(
          "Only primitive numeric types should be boxed. type verification"
          "missed an unexpected type {}",
          pb.value()->type());
    }

    case Opcode::kPrimitiveUnbox: {
      auto& unbox = static_cast<const PrimitiveUnbox&>(instr);
      return unbox.type();
    }
    case Opcode::kIndexUnbox: {
      return TCInt64;
    }

    // Check opcodes return a copy of their input that is statically known to
    // not be null.
    case Opcode::kCheckExc:
    case Opcode::kCheckField:
    case Opcode::kCheckFreevar:
    case Opcode::kCheckNeg:
    case Opcode::kCheckVar: {
      return get_op_type(0) - TNullptr;
    }

    case Opcode::kGuardIs: {
      auto type = Type::fromObject(static_cast<const GuardIs&>(instr).target());
      return get_op_type(0) & type;
    }

    case Opcode::kCast: {
      auto& cast = static_cast<const Cast&>(instr);
      Type to_type = Type::fromType(cast.pytype()) |
          (cast.optional() ? TNoneType : TBottom);
      return to_type;
    }

    case Opcode::kTpAlloc: {
      auto& tp_alloc = static_cast<const TpAlloc&>(instr);
      Type alloc_type = Type::fromTypeExact(tp_alloc.pytype());
      return alloc_type;
    }

    // Refine type gives us more information about the type of its input.
    case Opcode::kRefineType: {
      auto type = static_cast<const RefineType&>(instr).type();
      return get_op_type(0) & type;
    }

    case Opcode::kGuardType: {
      auto type = static_cast<const GuardType&>(instr).target();
      return get_op_type(0) & type;
    }

    case Opcode::kUnicodeConcat:
    case Opcode::kUnicodeRepeat:
    case Opcode::kUnicodeSubscr: {
      return TUnicodeExact;
    }

      // Finally, some opcodes have no destination.
    case Opcode::kBatchDecref:
    case Opcode::kBeginInlinedFunction:
    case Opcode::kBranch:
    case Opcode::kCallStaticRetVoid:
    case Opcode::kCheckErrOccurred:
    case Opcode::kCondBranch:
    case Opcode::kCondBranchCheckType:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kDecref:
    case Opcode::kDeleteAttr:
    case Opcode::kDeleteSubscr:
    case Opcode::kDeopt:
    case Opcode::kDeoptPatchpoint:
    case Opcode::kEndInlinedFunction:
    case Opcode::kGuard:
    case Opcode::kHintType:
    case Opcode::kIncref:
    case Opcode::kInitFrameCellVars:
    case Opcode::kRaise:
    case Opcode::kRaiseAwaitableError:
    case Opcode::kRaiseStatic:
    case Opcode::kReturn:
    case Opcode::kSetCurrentAwaiter:
    case Opcode::kSetCellItem:
    case Opcode::kSetFunctionAttr:
    case Opcode::kSnapshot:
    case Opcode::kStoreArrayItem:
    case Opcode::kStoreAttr:
    case Opcode::kStoreAttrCached:
    case Opcode::kStoreField:
    case Opcode::kStoreSubscr:
    case Opcode::kUnreachable:
    case Opcode::kUpdatePrevInstr:
    case Opcode::kUseType:
    case Opcode::kWaitHandleRelease:
    case Opcode::kXDecref:
    case Opcode::kXIncref:
      JIT_ABORT("Opcode {} has no output", instr.opname());
  }
  JIT_ABORT("Bad opcode {}", static_cast<int>(instr.opcode()));
}

Type outputType(const Instr& instr) {
  return outputType(
      instr, [&](std::size_t ind) { return instr.GetOperand(ind)->type(); });
}

void reflowTypes(Function& func) {
  reflowTypes(func, func.cfg.entry_block);
}

void reflowTypes(Function& func, BasicBlock* start) {
  // First, reset all types to Bottom so Phi inputs from back edges don't
  // contribute to the output type of the Phi until they've been processed.
  for (auto& pair : func.env.GetRegisters()) {
    pair.second->set_type(TBottom);
  }

  // Next, flow types forward, iterating to a fixed point.
  auto rpo_blocks = CFG::GetRPOTraversal(start);
  for (bool changed = true; changed;) {
    changed = false;
    for (auto block : rpo_blocks) {
      for (auto& instr : *block) {
        if (instr.opcode() == Opcode::kReturn) {
          Type type = static_cast<const Return&>(instr).type();
          hir::Register* value = instr.GetOperand(0);
          JIT_DCHECK(
              value->type() <= type,
              "Function expecting to return a {} but got {}:{}, CFG is:\n{}",
              type,
              instr,
              value->type(),
              func.cfg);
        }

        auto dst = instr.output();
        if (dst == nullptr) {
          continue;
        }

        auto new_ty = outputType(instr);
        if (new_ty == dst->type()) {
          continue;
        }

        dst->set_type(new_ty);
        changed = true;
      }
    }
  }
}

bool removeTrampolineBlocks(CFG* cfg) {
  std::vector<BasicBlock*> trampolines;
  for (auto& block : cfg->blocks) {
    if (!block.IsTrampoline()) {
      continue;
    }
    BasicBlock* succ = block.successor(0);
    // if this is the entry block and its successor has multiple
    // predecessors, don't remove it; it's necessary to maintain isolated
    // entries
    if (&block == cfg->entry_block) {
      if (succ->in_edges().size() > 1) {
        continue;
      } else {
        cfg->entry_block = succ;
      }
    }
    // Update all predecessors to jump directly to our successor
    block.retargetPreds(succ);
    // Finish splicing the trampoline out of the cfg
    block.set_successor(0, nullptr);
    trampolines.emplace_back(&block);
  }
  for (auto& block : trampolines) {
    cfg->RemoveBlock(block);
    delete block;
  }
  simplifyRedundantCondBranches(cfg);
  return trampolines.size() > 0;
}

bool removeUnreachableBlocks(Function& func) {
  auto cfg = &func.cfg;

  std::unordered_set<BasicBlock*> visited;
  std::vector<BasicBlock*> stack;
  stack.emplace_back(cfg->entry_block);
  while (!stack.empty()) {
    BasicBlock* block = stack.back();
    stack.pop_back();
    if (visited.contains(block)) {
      continue;
    }
    visited.insert(block);
    auto term = block->GetTerminator();
    for (std::size_t i = 0, n = term->numEdges(); i < n; ++i) {
      BasicBlock* succ = term->successor(i);
      // This check isn't necessary for correctness but avoids unnecessary
      // pushes to the stack.
      if (!visited.contains(succ)) {
        stack.emplace_back(succ);
      }
    }
  }

  std::vector<BasicBlock*> unreachable;
  for (auto it = cfg->blocks.begin(); it != cfg->blocks.end();) {
    BasicBlock* block = &*it;
    ++it;
    if (!visited.contains(block)) {
      if (Instr* old_term = block->GetTerminator()) {
        for (std::size_t i = 0, n = old_term->numEdges(); i < n; ++i) {
          old_term->successor(i)->removePhiPredecessor(block);
        }
      }
      cfg->RemoveBlock(block);
      block->clear();
      unreachable.emplace_back(block);
    }
  }

  for (BasicBlock* block : unreachable) {
    delete block;
  }

  return unreachable.size() > 0;
}

bool removeUnreachableInstructions(Function& func) {
  auto cfg = &func.cfg;

  bool modified = false;
  std::vector<BasicBlock*> blocks = cfg->GetPostOrderTraversal();
  DominatorAnalysis dom(func);
  RegUses reg_uses = collectDirectRegUses(func);
  auto remove_reg_uses = [&reg_uses](Instr* instr) {
    for (auto op : instr->GetOperands()) {
      auto instrs = reg_uses.find(op);
      if (instrs != reg_uses.end()) {
        instrs->second.erase(instr);
      }
    }
  };
  for (BasicBlock* block : blocks) {
    auto it = block->begin();
    while (it != block->end()) {
      Instr& instr = *it;
      ++it;
      if ((instr.output() == nullptr || !instr.output()->isA(TBottom)) &&
          !instr.IsUnreachable()) {
        continue;
      }
      // 1) Any instruction dominated by a definition of a Bottom value is
      // unreachable, so we delete any such instructions and replace them
      // with a special marker instruction (Unreachable)
      // 2) Any instruction post dominated by Unreachable must deopt if it can
      // deopt, else it is unreachable itself.

      modified = true;
      // Find the last instruction between [block.begin, current instruction]
      // that can deopt. Place the Unreachable marker right after that
      // instruction. If we can't find any instruction that can deopt, the
      // Unreachable marker is placed at the beginning of the block.
      do {
        auto prev_it = std::prev(it);
        Instr& prev_instr = *prev_it;
        if (prev_instr.asDeoptBase() != nullptr) {
          break;
        }
        it = prev_it;
      } while (it != block->begin());

      if (it != block->begin() && std::prev(it)->IsGuardType()) {
        // Everything after this GuardType is unreachable, but only as long as
        // the GuardType fails at runtime. Indicate that the guard is required
        // for correctness with a UseType. This prevents GuardTypeElimination
        // from removing it.
        Instr& guard_type = *std::prev(it);
        block->insert(
            UseType::create(guard_type.output(), guard_type.output()->type()),
            it);
      }

      block->insert(Unreachable::create(), it);
      // Clean up dangling phi references
      if (Instr* old_term = block->GetTerminator()) {
        for (std::size_t i = 0, n = old_term->numEdges(); i < n; ++i) {
          auto bb = old_term->successor(i);
          for (auto& potential_phi : *bb) {
            if (potential_phi.IsPhi()) {
              remove_reg_uses(&potential_phi);
            }
          }

          old_term->successor(i)->removePhiPredecessor(block);
        }
      }
      // Remove all instructions after the Unreachable
      while (it != block->end()) {
        Instr& instrToDelete = *it;
        ++it;
        instrToDelete.unlink();
        remove_reg_uses(&instrToDelete);
        delete &instrToDelete;
      }
    }
    if (block->begin()->IsUnreachable()) {
      std::vector<Instr*> interesting_branches;
      // If one edge of a conditional branch leads to an Unreachable, it can be
      // replaced with a Branch to the other target. If a Branch leads to an
      // Unreachable, it is replaced with an Unreachable.
      for (const Edge* edge : block->in_edges()) {
        BasicBlock* predecessor = edge->from();
        interesting_branches.emplace_back(predecessor->GetTerminator());
      }
      for (Instr* branch : interesting_branches) {
        if (branch->IsBranch()) {
          branch->ReplaceWith(*Unreachable::create());
        } else if (auto cond_branch = dynamic_cast<CondBranchBase*>(branch)) {
          BasicBlock* target;
          if (cond_branch->false_bb() == block) {
            target = cond_branch->true_bb();
          } else {
            JIT_CHECK(
                cond_branch->true_bb() == block,
                "true branch must be unreachable");
            target = cond_branch->false_bb();
          }

          if (branch->IsCondBranchCheckType()) {
            // Before replacing a CondBranchCheckType with a Branch to the
            // reachable block, insert a RefineType to preserve the type
            // information implied by following that path.
            auto check_type_branch = static_cast<CondBranchCheckType*>(branch);
            Register* refined_value = func.env.AllocateRegister();
            Type check_type = check_type_branch->type();
            if (target == cond_branch->false_bb()) {
              check_type = TTop - check_type_branch->type();
            }

            Register* operand = check_type_branch->GetOperand(0);
            RefineType::create(refined_value, check_type, operand)
                ->InsertBefore(*cond_branch);
            auto uses = reg_uses.find(operand);
            if (uses == reg_uses.end()) {
              break;
            }
            std::unordered_set<Instr*>& instrs_using_reg = uses->second;
            const std::unordered_set<const BasicBlock*>& dom_set =
                dom.getBlocksDominatedBy(target);
            for (Instr* instr : instrs_using_reg) {
              if (dom_set.contains(instr->block())) {
                instr->ReplaceUsesOf(operand, refined_value);
              }
            }
          }
          cond_branch->ReplaceWith(*Branch::create(target));
        } else {
          JIT_ABORT("Unexpected branch instruction {}", *branch);
        }
        remove_reg_uses(branch);
        delete branch;
      }
    }
  }
  if (modified) {
    removeUnreachableBlocks(func);
    reflowTypes(func);
  }
  return modified;
}

void simplifyRedundantCondBranches(CFG* cfg) {
  std::vector<BasicBlock*> to_simplify;
  for (auto& block : cfg->blocks) {
    if (block.empty()) {
      continue;
    }
    auto term = block.GetTerminator();
    std::size_t num_edges = term->numEdges();
    if (num_edges < 2) {
      continue;
    }
    JIT_CHECK(num_edges == 2, "only two edges are supported");
    if (term->successor(0) != term->successor(1)) {
      continue;
    }
    switch (term->opcode()) {
      case Opcode::kCondBranch:
      case Opcode::kCondBranchIterNotDone:
      case Opcode::kCondBranchCheckType:
        break;
      default:
        // Can't be sure that it's safe to replace the instruction with a branch
        JIT_ABORT("Unknown side effects of {} instruction", term->opname());
    }
    to_simplify.emplace_back(&block);
  }
  for (auto& block : to_simplify) {
    auto term = block->GetTerminator();
    term->unlink();
    auto branch = block->appendWithOff<Branch>(
        term->bytecodeOffset(), term->successor(0));
    branch->copyBytecodeOffset(*term);
    delete term;
  }
}

} // namespace jit::hir

// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/optimization.h"

#include "internal/pycore_interp.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
#include "cinderx/module_state.h"

#include <fmt/format.h>

#include <memory>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit::hir {

Instr* DynamicComparisonElimination::ReplaceCompare(
    Compare* compare,
    IsTruthy* truthy) {
  return CompareBool::create(
      truthy->output(),
      compare->op(),
      compare->GetOperand(0),
      compare->GetOperand(1),
      *get_frame_state(*truthy));
}

void DynamicComparisonElimination::Run(Function& irfunc) {
  LivenessAnalysis liveness{irfunc};
  liveness.Run();
  auto last_uses = liveness.GetLastUses();

  // Optimize "if x is y" case
  for (auto& block : irfunc.cfg.blocks) {
    auto& instr = block.back();

    // Looking for:
    //   $some_conditional = ...
    //   $truthy = IsTruthy $compare
    //   CondBranch<x, y> $truthy
    // Which we then re-write to a form which doesn't use IsTruthy anymore.
    if (!instr.IsCondBranch()) {
      continue;
    }

    Instr* truthy = instr.GetOperand(0)->instr();
    if (!truthy->IsIsTruthy() || truthy->block() != &block) {
      continue;
    }

    Instr* truthy_target = truthy->GetOperand(0)->instr();
    if (truthy_target->block() != &block ||
        (!truthy_target->IsCompare() && !truthy_target->IsVectorCall())) {
      continue;
    }

    auto& dying_regs = map_get(last_uses, truthy, kEmptyRegSet);

    if (!dying_regs.contains(truthy->GetOperand(0))) {
      // Compare output lives on, we can't re-write...
      continue;
    }

    // Make sure the output of compare isn't getting used between the compare
    // and the branch other than by the truthy instruction.
    std::vector<Instr*> snapshots;
    bool can_optimize = true;
    for (auto it = std::next(block.rbegin()); it != block.rend(); ++it) {
      if (&*it == truthy_target) {
        break;
      } else if (&*it != truthy) {
        if (it->IsSnapshot()) {
          if (it->Uses(truthy_target->output())) {
            snapshots.push_back(&*it);
          }
          continue;
        } else if (!it->isReplayable()) {
          can_optimize = false;
          break;
        }

        if (it->Uses(truthy->GetOperand(0))) {
          can_optimize = false;
          break;
        }
      }
    }
    if (!can_optimize) {
      continue;
    }

    Instr* replacement = nullptr;
    if (truthy_target->IsCompare()) {
      auto compare = static_cast<Compare*>(truthy_target);

      replacement = ReplaceCompare(compare, static_cast<IsTruthy*>(truthy));
    }

    if (replacement != nullptr) {
      replacement->copyBytecodeOffset(instr);
      truthy->ReplaceWith(*replacement);

      truthy_target->unlink();
      delete truthy_target;
      delete truthy;

      // There may be zero or more Snapshots between the Compare and the
      // IsTruthy that uses the output of the Compare (which we want to delete).
      // Since we're fusing the two operations together, the Snapshot and
      // its use of the dead intermediate value should be deleted.
      for (auto snapshot : snapshots) {
        snapshot->unlink();
        delete snapshot;
      }
    }
  }

  reflowTypes(irfunc);
}

#if PY_VERSION_HEX >= 0x030C0000
class BytecodeIndexToLine {
 public:
  explicit BytecodeIndexToLine(PyCodeObject* co) {
    code_ = co;
    size_t num_indices = countIndices(co);
    indexToLine_.reserve(num_indices);
    PyCodeAddressRange range;
    Cix_PyCode_InitAddressRange(co, &range);
    int idx = 0;
    while (Cix_PyLineTable_NextAddressRange(&range)) {
      if (idx >= num_indices) {
        break;
      }
      JIT_DCHECK(
          range.ar_start % sizeof(_Py_CODEUNIT) == 0,
          "offsets should be a multiple of code-units");
      JIT_DCHECK(
          idx == range.ar_start / 2, "Index does not line up with range");
      for (; idx < range.ar_end / 2; idx++) {
        indexToLine_.emplace_back(range.ar_line);
      }
    }
  }

  int lineNoFor(BCIndex index) const {
    if (index.value() < 0) {
      return -1;
    }
    JIT_DCHECK(
        index.value() < indexToLine_.size(),
        "Index out of range {} < {}, {}",
        index.value(),
        indexToLine_.size(),
        PyUnicode_AsUTF8(code_->co_qualname));
    return indexToLine_[index.value()];
  }

  PyCodeObject* code_;

 private:
  std::vector<int> indexToLine_;
};

struct InlineStackState {
  InlineStackState(BasicBlock* block, BeginInlinedFunction* parent) {
    this->block = block;
    this->parent = parent;
  }
  BasicBlock* block;
  BeginInlinedFunction* parent;
};

void InsertUpdatePrevInstr::Run(Function& func) {
  // We can have instructions w/ different code objects when we have
  // inlined functions so we maintain multiple BytecodeIndexToLine based upon
  // the code object
  std::unordered_map<PyCodeObject*, BytecodeIndexToLine> code_bc_idx_map;
  code_bc_idx_map.emplace(func.code, BytecodeIndexToLine(func.code));

  std::stack<InlineStackState> worklist;
  std::unordered_set<BasicBlock*> enqueued;
  std::unordered_map<BeginInlinedFunction*, BeginInlinedFunction*> parents;

  worklist.emplace(func.cfg.entry_block, nullptr);
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  bool inited_once = false;
#endif
  while (!worklist.empty()) {
    auto cur = worklist.top();
    auto block = cur.block;
    auto parent = cur.parent;
    worklist.pop();

    int prev_emitted_lno_or_bc = INT_MAX;
    for (Instr& instr : *block) {
      auto update_one = [&]() {
        auto add_update_prev_instr = [&](int line_no) {
          Instr* update_instr = UpdatePrevInstr::create(line_no, parent);
          update_instr->copyBytecodeOffset(instr);
          update_instr->InsertBefore(instr);
        };
        // If we don't have a valid line table to optimize with, update after
        // every bytecode.
        bool update_every_bc = func.code->co_linetable == nullptr ||
            PyBytes_Size(func.code->co_linetable) == 0;

        if (update_every_bc) {
          int cur_bc_offs = instr.bytecodeOffset().value();
          if (cur_bc_offs != prev_emitted_lno_or_bc) {
            add_update_prev_instr(-1);
            prev_emitted_lno_or_bc = cur_bc_offs;
          }
        } else {
          auto& cur_bc_idx_to_line = code_bc_idx_map.at(
              parent == nullptr ? func.code : parent->code());
          int cur_line_no =
              cur_bc_idx_to_line.lineNoFor(instr.bytecodeOffset());
          if (cur_line_no != prev_emitted_lno_or_bc) {
            add_update_prev_instr(cur_line_no);
            prev_emitted_lno_or_bc = cur_line_no;
          }
        }
      };

      // Inlined functions have a single entry point and a single exit, so we
      // will encounter the exit by following the successor blocks from the
      // entry.
      if (instr.IsBeginInlinedFunction()) {
        // We need to ensure we have emitted a line number update to the outer
        // function before going to the inlined function, otherwise the runtime
        // will see the outer function has having an incomplete frame and skip
        // it in stack traces.
        update_one();

        auto begin = static_cast<BeginInlinedFunction*>(&instr);
        auto code = begin->code();
        if (code_bc_idx_map.find(code) == code_bc_idx_map.end()) {
          code_bc_idx_map.emplace(code, BytecodeIndexToLine(code));
        }
        parents[begin] = parent;
        parent = begin;
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
        inited_once = false;
#endif
      } else if (instr.IsEndInlinedFunction()) {
        parent =
            parents[static_cast<EndInlinedFunction&>(instr).matchingBegin()];
      }

#ifdef ENABLE_LIGHTWEIGHT_FRAMES
      // The first LoadEvalBreaker is emitted for the RESUME instruction which
      // indicates when we should update the line number from the instruction
      // - 1 to the first instruction to indicate that the frame is now
      // complete.
      if (!inited_once && instr.IsLoadEvalBreaker()) {
        auto& cur_bc_idx_to_line =
            code_bc_idx_map.at(parent == nullptr ? func.code : parent->code());
        int line_no = cur_bc_idx_to_line.lineNoFor(
            BCIndex(func.code->_co_firsttraceable));
        Instr* update_instr = UpdatePrevInstr::create(line_no, parent);
        update_instr->setBytecodeOffset(BCIndex(func.code->_co_firsttraceable));
        update_instr->InsertBefore(instr);

        inited_once = true;
      }
#else
      if (hasArbitraryExecution(instr)) {
        update_one();
      }
#endif
    }

    // Add the successors to be processed
    auto term = block->GetTerminator();
    for (std::size_t i = 0, n = term->numEdges(); i < n; ++i) {
      BasicBlock* succ = term->successor(i);
      if (!enqueued.contains(succ)) {
        worklist.emplace(succ, parent);
        enqueued.insert(succ);
      }
    }
  }
}
#endif

static bool
guardNeeded(const RegUses& uses, Register* new_reg, Type relaxed_type) {
  auto it = uses.find(new_reg);
  if (it == uses.end()) {
    // No uses; the guard is dead.
    return false;
  }
  // Stores all Register->Type pairs to consider as the algorithm examines
  // whether a guard is needed across passthrough + Phi instructions
  std::queue<std::pair<Register*, Type>> worklist;
  std::unordered_map<Register*, std::unordered_set<Type>> seen_state;
  worklist.emplace(new_reg, relaxed_type);
  seen_state[new_reg].insert(relaxed_type);
  while (!worklist.empty()) {
    std::pair<Register*, Type> args = worklist.front();
    worklist.pop();
    new_reg = args.first;
    relaxed_type = args.second;
    auto new_reg_uses = uses.find(new_reg);
    if (new_reg_uses == uses.end()) {
      continue;
    }
    for (const Instr* instr : new_reg_uses->second) {
      for (std::size_t i = 0; i < instr->NumOperands(); i++) {
        if (instr->GetOperand(i) == new_reg) {
          if ((instr->output() != nullptr) &&
              (instr->IsPhi() || isPassthrough(*instr))) {
            Register* passthrough_output = instr->output();
            Type passthrough_type = outputType(*instr, [&](std::size_t ind) {
              if (ind == i) {
                return relaxed_type;
              }
              return instr->GetOperand(ind)->type();
            });
            if (seen_state[passthrough_output]
                    .insert(passthrough_type)
                    .second) {
              worklist.emplace(passthrough_output, passthrough_type);
            }
          }
          OperandType expected_type = instr->GetOperandType(i);
          // TASK(T106726658): We should be able to remove GuardTypes if we ever
          // add a matching constraint for non-Primitive types, and our
          // GuardType adds an unnecessary refinement. Since we cannot guard on
          // primitive types yet, this should never happen
          if (operandsMustMatch(expected_type)) {
            JIT_DLOG(
                "'{}' kept alive by primitive '{}'", *new_reg->instr(), *instr);
            return true;
          }
          if (!registerTypeMatches(relaxed_type, expected_type)) {
            JIT_DLOG("'{}' kept alive by '{}'", *new_reg->instr(), *instr);
            return true;
          }
        }
      }
    }
  }
  return false;
}

void GuardTypeRemoval::Run(Function& func) {
  RegUses reg_uses = collectDirectRegUses(func);
  std::vector<std::unique_ptr<Instr>> removed_guards;
  for (auto& block : func.cfg.blocks) {
    for (auto it = block.begin(); it != block.end();) {
      auto& instr = *it;
      ++it;

      if (!instr.IsGuardType()) {
        continue;
      }

      Register* guard_out = instr.output();
      Register* guard_in = instr.GetOperand(0);
      if (!guardNeeded(reg_uses, guard_out, guard_in->type())) {
        auto assign = Assign::create(guard_out, guard_in);
        assign->copyBytecodeOffset(instr);
        instr.ReplaceWith(*assign);
        removed_guards.emplace_back(&instr);
      }
    }
  }

  CopyPropagation{}.Run(func);
  reflowTypes(func);
}

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
static BorrowedRef<> getMethodObjectFromType(
    Type receiver_type,
    BorrowedRef<> name) {
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
static bool tryEliminateLoadMethod(Function& irfunc, MethodInvoke& invoke) {
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

// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/optimization.h"

#include "internal/pycore_interp.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#include <memory>
#include <stack>
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

} // namespace jit::hir

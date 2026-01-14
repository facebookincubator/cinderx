// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/insert_update_prev_instr.h"

#include "cinderx/Common/code.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#include <stack>
#include <vector>

namespace jit::hir {

namespace {

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
    if (index.value() >= indexToLine_.size()) {
      // test.test_exceptions.PEP626Tests.test_missing_lineno_shows_as_none
      // specifically checks that things work when there isn't enough line
      // number information.
      return -1;
    } else {
      return indexToLine_[index.value()];
    }
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

#endif

} // namespace

void InsertUpdatePrevInstr::Run([[maybe_unused]] Function& func) {
#if PY_VERSION_HEX >= 0x030C0000
  // We can have instructions w/ different code objects when we have
  // inlined functions so we maintain multiple BytecodeIndexToLine based upon
  // the code object
  std::unordered_map<PyCodeObject*, BytecodeIndexToLine> code_bc_idx_map;
  code_bc_idx_map.emplace(func.code, BytecodeIndexToLine(func.code));

  std::stack<InlineStackState> worklist;
  std::unordered_set<BasicBlock*> enqueued;
  std::unordered_map<BeginInlinedFunction*, BeginInlinedFunction*> parents;

  worklist.emplace(func.cfg.entry_block, nullptr);
  [[maybe_unused]] bool inited_once = false;
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
        if (getConfig().frame_mode == FrameMode::kLightweight) {
          inited_once = false;
        }
      } else if (instr.IsEndInlinedFunction()) {
        parent =
            parents[static_cast<EndInlinedFunction&>(instr).matchingBegin()];
      }

      if (getConfig().frame_mode == FrameMode::kLightweight) {
        // The first LoadEvalBreaker is emitted for the RESUME instruction which
        // indicates when we should update the line number from the instruction
        // - 1 to the first instruction to indicate that the frame is now
        // complete.
        if (!inited_once && instr.IsLoadEvalBreaker()) {
          auto target_code = parent == nullptr ? func.code : parent->code();
          auto& cur_bc_idx_to_line = code_bc_idx_map.at(target_code);
          int line_no = cur_bc_idx_to_line.lineNoFor(
              BCIndex(target_code->_co_firsttraceable));
          Instr* update_instr = UpdatePrevInstr::create(line_no, parent);
          update_instr->setBytecodeOffset(
              BCIndex(target_code->_co_firsttraceable));
          update_instr->InsertBefore(instr);

          inited_once = true;
        }
      } else if (hasArbitraryExecution(instr)) {
        update_one();
      }
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
#endif
}

} // namespace jit::hir

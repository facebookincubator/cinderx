// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/block_builder.h"
#include "cinderx/Jit/lir/function.h"

#include <memory>
#include <string>

namespace jit::lir {

// XXX: this file needs to be revisited when we optimize HIR-to-LIR translation
// in codegen.cpp/h. Currently, this file is almost an identical copy from
// codegen.h with some interfaces changes so that it works with the new
// LIR.

class BasicBlockBuilder;

class LIRGenerator {
 public:
  explicit LIRGenerator(
      const jit::hir::Function* func,
      jit::codegen::Environ* env);

  std::unique_ptr<jit::lir::Function> TranslateFunction();

  const jit::hir::Function* GetHIRFunction() const {
    return func_;
  }

  BasicBlock* frameSetupBlock() {
    return frame_setup_block_;
  }

 private:
  // The result of translating one HIR block: the entry and exit LIR blocks.
  struct TranslatedBlock {
    BasicBlock* first;
    BasicBlock* last;
  };

  const jit::hir::Function* func_{nullptr};

  jit::codegen::Environ* env_{nullptr};

  BasicBlock* entry_block_{nullptr};
  BasicBlock* frame_setup_block_{nullptr};
  BasicBlock* exit_block_{nullptr};

  std::vector<BasicBlock*> basic_blocks_;

  // Borrowed pointers so the caches can be looked up by index; they're
  // allocated from and owned by Runtime.
  std::vector<LoadTypeAttrCache*> load_type_attr_caches_;
  std::vector<LoadTypeMethodCache*> load_type_method_caches_;
#if PY_VERSION_HEX >= 0x030E0000
  std::unordered_map<BorrowedRef<PyCodeObject>, BorrowedRef<>>
      inline_code_to_reifier_;
#endif

  void AnalyzeCopies();
  BasicBlock* GenerateEntryBlock();
  BasicBlock* GenerateExitBlock();

  void appendGuardAlwaysFail(
      BasicBlockBuilder& bbb,
      const hir::DeoptBase& instr);

  template <class TOperand>
  void appendGuard(
      BasicBlockBuilder& bbb,
      InstrGuardKind kind,
      const hir::DeoptBase& hir_instr,
      TOperand&& guard_var) {
    JIT_CHECK(kind != InstrGuardKind::kAlwaysFail, "Use appendGuardAlwaysFail");
    auto deopt_id = bbb.makeDeoptMetadata();
    auto instr = bbb.appendInstr(
        Instruction::kGuard,
        Imm{static_cast<uint64_t>(kind)},
        Imm{deopt_id},
        std::forward<TOperand>(guard_var));

    if (hir_instr.IsGuardIs()) {
      const auto& guard = static_cast<const hir::GuardIs&>(hir_instr);
      env_->code_rt->addReference(guard.target());
      instr->addOperands(MemImm{guard.target()});
    } else if (hir_instr.IsGuardType()) {
      const auto& guard = static_cast<const hir::GuardType&>(hir_instr);
      // TASK(T101999851): Handle non-Exact types
      JIT_CHECK(
          guard.target().isExact(), "Only exact type guards are supported");
      PyTypeObject* guard_type = guard.target().uniquePyType();
      JIT_CHECK(guard_type != nullptr, "Ensure unique representation exists");
      env_->code_rt->addReference(reinterpret_cast<PyObject*>(guard_type));
      instr->addOperands(MemImm{guard_type});
    } else {
      instr->addOperands(Imm{0});
    }

    addLiveRegOperands(bbb, instr, hir_instr);
  }

  void addLiveRegOperands(
      BasicBlockBuilder& bbb,
      Instruction* instr,
      const hir::DeoptBase& hir_instr);

  void MakeIncref(
      BasicBlockBuilder& bbb,
      lir::Instruction* instr,
      bool xincref = false,
      bool possible_immortal = true);
  void MakeIncref(
      BasicBlockBuilder& bbb,
      const jit::hir::Instr& instr,
      bool xincref);
  void MakeDecref(
      BasicBlockBuilder& bbb,
      lir::Instruction* instr,
      std::optional<destructor> destructor,
      bool xdecref = false,
      bool possible_immortal = true);
  void MakeDecref(
      BasicBlockBuilder& bbb,
      const jit::hir::Instr& instr,
      bool xdecref);
#if defined(CINDER_AARCH64)
  void updateDeoptIndex(
      BasicBlockBuilder& bbb,
      const jit::hir::Instr& i,
      jit::hir::Opcode opcode,
      int& last_deopt_line,
      const jit::hir::FrameState* caller_fs,
      BorrowedRef<PyCodeObject> inlined_code);
#endif

  bool TranslateSpecializedCall(
      BasicBlockBuilder& bbb,
      const hir::VectorCall& instr);

  TranslatedBlock TranslateOneBasicBlock(
      const hir::BasicBlock* bb,
      const hir::FrameState* initial_caller_fs = nullptr,
      BorrowedRef<PyCodeObject> initial_inlined_code = nullptr);

  // Fill in operands for phi instructions.  This is executed after LIR
  // instructions have been generated for all values in the control flow graph.
  void resolvePhiOperands(
      UnorderedMap<const hir::BasicBlock*, TranslatedBlock>& bb_map);

  void emitLoadFrame(BasicBlockBuilder& bbb);

  void emitExceptionCheck(
      const jit::hir::DeoptBase& i,
      jit::lir::BasicBlockBuilder& bbb);

  // Load a string name from an HIR instruction that contains a name index.
  Instruction* getNameFromIdx(
      BasicBlockBuilder& bbb,
      const hir::DeoptBaseWithNameIdx* instr);

  Instruction* getInlinedFrame(
      BasicBlockBuilder& bbb,
      const hir::BeginInlinedFunction* instr);

  Function* lir_func_{nullptr};

#if defined(CINDER_AARCH64)
  // Address of the deopt_idx field in the FrameHeader, computed once per
  // function in the entry block. Used by TranslateOneBasicBlock to store the
  // deopt index before each deoptable instruction.
  Instruction* deopt_idx_addr_{nullptr};
#endif
};

// Build a post-regalloc LIR block for the generator resume entry point.
// This block uses only physical registers and handles: prologue, frame setup,
// saving caller state into GenDataFooter, and dispatching to the yield point's
// resume target.
BasicBlock* GenerateResumeEntryBlock(
    Function* lir_func,
    Py_ssize_t gi_jit_data_offset);

// Populate the entry block with post-regalloc LIR instructions that load
// arguments from the vectorcall args array into their assigned registers.
void PopulateEntryBlock(
    BasicBlock* entry_block,
    const std::vector<PhyLocation>& arg_locations);

// Unresolved jump table for static type check dispatch. After code generation,
// each entry's BasicBlock* must be resolved to a code address via the block
// label map and stored in the table array.
struct UnresolvedJumpTable {
  void** table{nullptr};
  // Maps table index → LIR BasicBlock* target.
  std::vector<std::pair<int, BasicBlock*>> entries;
};

// Build post-regalloc LIR blocks for validating that arguments passed to a
// Static Python function via the generic (vectorcall) entry point have the
// correct types. Creates a dispatch block + per-argument check blocks (with
// optional MRO walk blocks) and inserts them at the front of the LIR function's
// block list so the prologue falls through to the dispatch block.
//
// Returns an UnresolvedJumpTable whose entries must be resolved after code
// generation (when block labels have been bound to addresses).
UnresolvedJumpTable GenerateStaticTypeCheckBlocks(
    Function* lir_func,
    BasicBlock* entry_block,
    const std::vector<hir::TypedArgument>& typed_args,
    int num_args,
    CodeRuntime* code_rt,
    asmjit::Label failure_label);

// Build a post-regalloc LIR block for the function entry prologue
// (push rbp; mov rbp, rsp on x86-64 / stp fp, lr; mov fp, sp on aarch64).
// The block is inserted at the front of the LIR function's block list so the
// vectorcall entry falls through to it.
void GenerateFunctionEntryBlock(Function* lir_func);

// Build a post-regalloc LIR block for the primitive-args prologue.
// Loads the _PyTypedArgsInfo pointer into the 5th argument register (R8/x4),
// calls JITRT_CallStaticallyWithPrimitiveSignature[FP], then branches to
// prologue_exit. The block is inserted at the front of the LIR function's
// block list.
void GeneratePrimitiveArgsPrologueBlock(
    Function* lir_func,
    PyObject* prim_args_info,
    bool returns_primitive_double,
    asmjit::Label prologue_exit);

// Build post-regalloc LIR blocks for the vectorcall argcount check prologue.
// Handles three paths: kwnames present (call keyword helper), wrong argcount
// (call incorrect-argcount helper), correct argcount (fall through to
// next_block). The created blocks are inserted at the front of the LIR
// function's block list so the vectorcall entry falls through to them.
void GenerateArgcountCheckBlocks(
    Function* lir_func,
    const hir::Function* func,
    BasicBlock* next_block,
    asmjit::Label prologue_exit);

} // namespace jit::lir

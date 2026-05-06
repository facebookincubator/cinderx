// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/annotations.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/debug_info.h"

#include <asmjit/asmjit.h>

#include <vector>

namespace jit::codegen {

struct Environ {
  // Metadata for annotated disassembly.
  Annotations annotations;

  // Assembler builder.
  arch::Builder* as{nullptr};

  // Modified registers. Set by VariableManager and read by generatePrologue()
  // and generateEpilogue().
  PhyRegisterSet changed_regs{0};

  // The size of all data stored on the C stack: shadow frames, spilled values,
  // saved callee-saved registers, and space for stack arguments to called
  // functions.
  int stack_frame_size{-1};

  // A subset of stack_frame_size: only the shadow frames and spilled values.
  int shadow_frames_and_spill_size{0};

  // Offset from the base of the frame to the last callee-saved register stored
  // on the stack.
  int last_callee_saved_reg_off{-1};

  // Various Labels that span major sections of the function.
  asmjit::Label static_arg_typecheck_failed_label;
  asmjit::Label hard_exit_label;
  asmjit::Label exit_label;
  asmjit::Label gen_resume_entry_label;
  asmjit::Label finish_frame_setup;
  asmjit::Label correct_arg_count;
  asmjit::Label prologue_exit;
  asmjit::Label wrapper_exit;

  // Static type check jump table, resolved after code generation.
  void** static_typecheck_table{nullptr};
  std::vector<std::pair<int, jit::lir::BasicBlock*>>
      static_typecheck_jt_entries;

  // Resume label shared between StoreGenYieldPoint and ResumeGenYield.
  // Created by translateStoreGenYieldPoint, bound by
  // translateResumeGenYield.
  asmjit::Label pending_yield_resume_label;

  // Map from deopt metadata index to the stage 1 deopt exit LIR block.
  // Populated by GenerateDeoptExitBlocks (post-regalloc), used by
  // TranslateGuard/TranslateDeoptPatchpoint to branch to the correct block.
  UnorderedMap<size_t, jit::lir::BasicBlock*> deopt_exit_blocks;

  // Address of the global deopt trampoline for this function.
  // Set by NativeGenerator before code generation.
  void* deopt_trampoline{nullptr};

  struct PendingDeoptPatcher {
    PendingDeoptPatcher(JumpPatcher* p, asmjit::Label pp, asmjit::Label de)
        : patcher(p), patchpoint(pp), deopt_exit(de) {}
    JumpPatcher* patcher;

    // Location of the patchpoint
    asmjit::Label patchpoint;

    // Location to jump to when the patchpoint is overwritten
    asmjit::Label deopt_exit;
  };
  std::vector<PendingDeoptPatcher> pending_deopt_patchers;

  std::vector<PendingDebugLoc> pending_debug_locs;

  // Call return-address -> post-call guard deopt-exit pairings, resolved to
  // addresses after code finalization.
  struct CallsiteDeoptPending {
    asmjit::Label return_addr_label;
    asmjit::Label deopt_exit_label;
  };
  std::vector<CallsiteDeoptPending> callsite_deopt_pending;

  // Location of incoming arguments
  std::vector<PhyLocation> arg_locations;

  struct IndirectInfo {
    explicit IndirectInfo(void** indirect_ptr) : indirect(indirect_ptr) {}

    void** indirect;
  };
  UnorderedMap<PyFunctionObject*, IndirectInfo> function_indirections;

  UnorderedMap<PyFunctionObject*, std::unique_ptr<_PyTypedArgsInfo>>
      function_typed_args;

  // Global runtime data.
  jit::Context* ctx{nullptr};

  // Runtime data for this function.
  jit::CodeRuntime* code_rt{nullptr};

  template <typename T>
  void addAnnotation(T&& item, asmjit::BaseNode* start_cursor) {
    if (suppress_annotations) {
      return;
    }
    annotations.add(std::forward<T>(item), as, start_cursor);
  }

  // When true, addAnnotation() calls are suppressed. Set by
  // generateAssemblyBody() while a text annotation is active so that
  // translator-internal annotations (e.g. "Set up frame pointer") don't
  // conflict with the higher-level text annotation.
  bool suppress_annotations{false};

  // Map of GenYieldPoints which need their resume_target_ setting after code-
  // gen is complete.
  UnorderedMap<GenYieldPoint*, asmjit::Label> unresolved_gen_entry_labels;

  // Maps HIR values to the LIR instruction that defines them.
  UnorderedMap<const hir::Register*, jit::lir::Instruction*> output_map;

  // Instruction definitions that are pinned to physical registers.
  jit::lir::Instruction* asm_tstate{nullptr};
  jit::lir::Instruction* asm_extra_args{nullptr};
  jit::lir::Instruction* asm_func{nullptr};
  jit::lir::Instruction* asm_interpreter_frame{nullptr};

  // Maps HIR values to the HIR values they were copied from. Used for LIR
  // generation purposes.
  //
  // This is a hack. Need to do the real copy propagation after LIR cleanup is
  // done. Related to jit::lir::LIRGenerator::AnalyzeCopies().
  UnorderedMap<const hir::Register*, hir::Register*> copy_propagation_map;

  UnorderedMap<jit::lir::BasicBlock*, asmjit::Label> block_label_map;

  UnorderedMap<const hir::BeginInlinedFunction*, lir::Instruction*>
      inline_frame_map;

  FrameMode frame_mode;

  int max_arg_buffer_size{0};

  // Size of stack space reserved by ReserveStack instructions. This space
  // is placed above the call argument buffer (at SP+max_arg_buffer_size),
  // so that call args remain at SP+0 where the callee expects them per the
  // ABI, and calls don't clobber the reserved data. The LIR ReserveStack
  // instruction is lowered to a LEA in autogen using max_arg_buffer_size
  // as the offset, and reserve_stack_size is added to the frame's arg
  // buffer in computeFrameInfo.
  int reserve_stack_size{0};

  bool has_inlined_functions{false};

#if defined(CINDER_X86_64) && defined(_WIN32)
  // Offset from RBP to a 16-byte buffer used for receiving struct return
  // values from C++ helper functions via the Windows x64 hidden-pointer ABI.
  // Computed before LIR generation and reserved in the register allocator's
  // frame so it doesn't conflict with spill slots.
  int win_struct_ret_offset{0};
#endif

#if defined(CINDER_AARCH64)
  // Constant pool for large immediate values. translateMovConstPool populates
  // these; gen_asm.cpp emits the pool data after deopt exits.
  UnorderedMap<uint64_t, asmjit::Label> const_pool_labels;
#endif

  // Frame layout computed after register allocation. Read by the kSetupFrame
  // autogen translator for both the normal entry and generator resume entry.
  int resume_frame_total_size{0};
  int resume_header_and_spill_size{0};
  PhyRegisterSet resume_saved_regs{0};

  // Byte offset of gi_jit_data within a generator object, computed per
  // function. Read by the resume entry block builder.
  Py_ssize_t gi_jit_data_offset{0};
};

} // namespace jit::codegen

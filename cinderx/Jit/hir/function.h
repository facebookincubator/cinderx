// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/containers.h"
#include "cinderx/Jit/hir/cfg.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/jit_time_log.h"
#include "cinderx/Jit/type_deopt_patchers.h"
#include "cinderx/StaticPython/typed-args-info.h"

#include <memory>

namespace cinderx::jit::hir {

class DominatorTree;

class Function {
 public:
  using InlineFailureStats =
      UnorderedMap<InlineFailureType, UnorderedSet<std::string>>;
  Function();
  ~Function();

  ThreadedRef<PyCodeObject> code;
  ThreadedRef<PyDictObject> builtins;
  ThreadedRef<PyDictObject> globals;

  // for primitive args only, null if there are none
  ThreadedRef<_PyTypedArgsInfo> prim_args_info;

  // Fully-qualified name of the function
  std::string fullname;

  // Does this function have primitive args?
  bool has_primitive_args{false};

  struct InlineFunctionStats {
    int num_inlined_functions{0};
    // map of {inline_failure_type -> function_names}
    InlineFailureStats failure_stats;
  } inline_function_stats;

  // vector of {locals_idx, type, optional}
  // in argument order, may have gaps for unchecked args
  std::vector<TypedArgument> typed_args;

  // Return type
  Type return_type{TObject};

  CFG cfg;

  Environment env;

  // All the code patchers pointing to patch points in this function.
  //
  // These will be moved over to the CompiledFunction after compilation is
  // complete.
  std::vector<std::unique_ptr<CodePatcher>> code_patchers;

  // Optional property used to track time taken for individual compilation
  // phases
  std::unique_ptr<CompilationPhaseTimer> compilation_phase_timer;

  // Return the total number of arguments (positional + kwonly + varargs +
  // varkeywords)
  int numArgs() const;

  // Return the number of locals + cellvars + freevars
  Py_ssize_t numVars() const;

  // Set code and a number of other members that are derived from it.
  void setCode(BorrowedRef<PyCodeObject> code);

  // Count the number of instructions that match the predicate
  std::size_t countInstrs(InstrPredicate pred) const;

  // Does this function return a primitive type?
  bool returnsPrimitive() const;

  // Does this function return a primitive double?
  bool returnsPrimitiveDouble() const;

  void setCompilationPhaseTimer(std::unique_ptr<CompilationPhaseTimer> cpt);

  bool canDeopt() const;

  template <typename T, typename... Args>
  T* allocateCodePatcher(Args&&... args) {
    code_patchers.emplace_back(
        std::make_unique<T>(std::forward<Args>(args)...));
    return static_cast<T*>(code_patchers.back().get());
  }

  // Get the code object for the given instruction.  Handles inlined functions
  // but assumes that inlined functions have a dominating FrameState from
  // BeginInlinedFunction to use.  If we start optimizing that out for inlined
  // functions that cannot deopt, we will have to do something different.
  //
  // The instruction must be part of this function.
  BorrowedRef<PyCodeObject> codeFor(const Instr& instr) const;

  // Dominator tree over the CFG's entry block, lazily built and cached so
  // optimization passes can share a single tree instead of each recomputing
  // dominance.  Any pass that mutates the CFG in a way that changes dominance
  // (adds/removes blocks, or retargets edges) must call invalidateDomTree().
  // The tree will be rebuilt on the next call to domTree().  Passes that only
  // rewrite instructions within blocks preserve dominance and need not
  // invalidate.
  const DominatorTree& domTree();

  // Discard any cached dominator tree.  Cheap and idempotent, safe to call even
  // when nothing has been cached yet.
  void invalidateDomTree();

  ThreadedRef<> reifier;

 private:
  DISALLOW_COPY_AND_ASSIGN(Function);

  std::unique_ptr<DominatorTree> dom_tree_;
};

using OpcodeCounts = std::array<int, kNumOpcodes>;
OpcodeCounts count_opcodes(const Function& func);

} // namespace cinderx::jit::hir

// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/hir/cfg.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/jit_time_log.h"
#include "cinderx/StaticPython/typed-args-info.h"

namespace jit::hir {

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

  // Does this function need its PyFunctionObject* at runtime?
  // (This is always the case in 3.12 as it is used to quickly access the
  // _PyInterpreterFrame)
  bool uses_runtime_func{
#if PY_VERSION_HEX < 0x030C0000
      false
#else
      true
#endif
  };

  // Does this function have primitive args?
  bool has_primitive_args{false};

  // is the first argument a primitive?
  bool has_primitive_first_arg{false};

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

  FrameMode frameMode{FrameMode::kNormal};

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
  std::size_t CountInstrs(InstrPredicate pred) const;

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

  ThreadedRef<> reifier;

 private:
  DISALLOW_COPY_AND_ASSIGN(Function);
};

using OpcodeCounts = std::array<int, kNumOpcodes>;
OpcodeCounts count_opcodes(const Function& func);

} // namespace jit::hir

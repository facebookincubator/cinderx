// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/preload.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

namespace jit {

using PostPassFunction = std::function<
    void(hir::Function& func, std::string_view pass_name, std::size_t time_ns)>;

// Controls what compiler passes are run.
enum PassConfig : uint64_t {
  // Only run compiler passes that are necessary for correctness, e.g. SSAify.
  kMinimal = 0,

  // Bits to toggle individual optimization passes.

  kBeginInlinedFunctionElim = 1 << 0,
  kBuiltinLoadMethodElim = 1 << 1,
  kCleanCFG = 1 << 2,
  kDeadCodeElim = 1 << 3,
  kDynamicComparisonElim = 1 << 4,
  kGuardTypeRemoval = 1 << 5,
  kInliner = 1 << 6,
  kPhiElim = 1 << 7,
  kSimplify = 1 << 8,
  kInsertUpdatePrevInstr = 1 << 9,

  // Run all the passes.
  kAll = ~uint64_t{0},

  // Run all the passes except for inlining.
  kAllExceptInliner = kAll & ~kInliner,
};

// The high-level interface for translating Python functions into native code.
class Compiler {
 public:
  Compiler() = default;

  // Compile the function / code object preloaded by the given Preloader.
  std::unique_ptr<CompiledFunction> Compile(const hir::Preloader& preloader);

  // Convenience wrapper to create and compile a preloader from a
  // PyFunctionObject.
  std::unique_ptr<CompiledFunction> Compile(BorrowedRef<PyFunctionObject> func);

  // Runs all the compiler passes on the HIR function.
  static void runPasses(hir::Function&, PassConfig config);

  // Runs the compiler passes, calling callback on the HIR function after each
  // pass.
  static void runPasses(
      hir::Function& irfunc,
      PassConfig config,
      PostPassFunction callback);

 private:
  DISALLOW_COPY_AND_ASSIGN(Compiler);
  codegen::NativeGeneratorFactory ngen_factory_;
};

} // namespace jit

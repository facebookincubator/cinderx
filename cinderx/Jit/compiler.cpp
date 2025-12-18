// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiler.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/builtin_load_method_elimination.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/dead_code_elimination.h"
#include "cinderx/Jit/hir/dynamic_comparison_elimination.h"
#include "cinderx/Jit/hir/guard_removal.h"
#include "cinderx/Jit/hir/hir_stats.h"
#include "cinderx/Jit/hir/inliner.h"
#include "cinderx/Jit/hir/insert_update_prev_instr.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/refcount_insertion.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/Jit/jit_time_log.h"

#include <chrono>
#include <iostream>

namespace jit {

template <typename T>
static void runPass(T&& pass, hir::Function& func, PostPassFunction callback) {
  COMPILE_TIMER(func.compilation_phase_timer,
                pass.name(),
                JIT_LOGIF(
                    getConfig().log.dump_hir_passes,
                    "HIR for {} before pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);

                Timer timer;
                pass.Run(func);
                std::size_t time_ns = timer.finish().count();
                callback(func, pass.name(), time_ns);

                JIT_LOGIF(
                    getConfig().log.dump_hir_passes,
                    "HIR for {} after pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);

                JIT_DCHECK(
                    checkFunc(func, std::cerr),
                    "Function {} failed verification after pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);

                JIT_DCHECK(
                    funcTypeChecks(func, std::cerr),
                    "Function {} failed type checking after pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);)
}

void Compiler::runPasses(jit::hir::Function& irfunc, PassConfig config) {
  PostPassFunction callback =
      [](hir::Function&, std::string_view, std::size_t) {};
  runPasses(irfunc, config, callback);
}

void Compiler::runPasses(
    jit::hir::Function& irfunc,
    PassConfig config,
    PostPassFunction callback) {
  // SSAify must come first; nothing but SSAify should ever see non-SSA HIR.
  runPass(jit::hir::SSAify{}, irfunc, callback);

  // Written this way because it's hard to forward the P type variable down to
  // runPass if it's not tied to one of the lambda's arguments.
  auto runPassIf = [&]<typename P>(P&& pass, PassConfig bit) {
    if (config & bit) {
      runPass(pass, irfunc, callback);
    }
  };

  runPassIf(hir::Simplify{}, PassConfig::kSimplify);
  runPassIf(
      hir::DynamicComparisonElimination{}, PassConfig::kDynamicComparisonElim);
  runPassIf(hir::GuardTypeRemoval{}, PassConfig::kGuardTypeRemoval);
  runPassIf(hir::PhiElimination{}, PassConfig::kPhiElim);

  if (config & PassConfig::kInliner) {
    runPass(jit::hir::InlineFunctionCalls{}, irfunc, callback);

    runPassIf(hir::Simplify{}, PassConfig::kSimplify);
    runPassIf(
        hir::BeginInlinedFunctionElimination{},
        PassConfig::kBeginInlinedFunctionElim);
  }

  runPassIf(
      hir::BuiltinLoadMethodElimination{}, PassConfig::kBuiltinLoadMethodElim);
  runPassIf(hir::Simplify{}, PassConfig::kSimplify);
  runPassIf(hir::CleanCFG{}, PassConfig::kCleanCFG);
  runPassIf(hir::DeadCodeElimination{}, PassConfig::kDeadCodeElim);
  runPassIf(hir::CleanCFG{}, PassConfig::kCleanCFG);

  runPass(jit::hir::RefcountInsertion{}, irfunc, callback);

  if (getConfig().dump_hir_stats) {
    jit::hir::HIRStats stats;
    runPass(stats, irfunc, callback);
    stats.dump(irfunc.fullname);
  }

  runPassIf(
      jit::hir::InsertUpdatePrevInstr{}, PassConfig::kInsertUpdatePrevInstr);

  JIT_LOGIF(
      getConfig().log.dump_hir_final,
      "Optimized HIR for {}:\n{}",
      irfunc.fullname,
      irfunc);
}

std::unique_ptr<CompiledFunction> Compiler::Compile(
    BorrowedRef<PyFunctionObject> func) {
  JIT_CHECK(PyFunction_Check(func), "Expected PyFunctionObject");
  JIT_CHECK(
      !getThreadedCompileContext().compileRunning(),
      "multi-thread compile must preload first");
  std::unique_ptr<hir::Preloader> preloader =
      hir::Preloader::makePreloader(func, makeFrameReifier(func->func_code));
  return preloader ? Compile(*preloader) : nullptr;
}

PassConfig createConfig() {
  auto result = static_cast<uint64_t>(PassConfig::kMinimal);

  auto set = [&](bool global, PassConfig pass) {
    if (global) {
      result |= pass;
    }
  };

  auto const& hir_opts = getConfig().hir_opts;
  set(hir_opts.begin_inlined_function_elim,
      PassConfig::kBeginInlinedFunctionElim);
  set(hir_opts.builtin_load_method_elim, PassConfig::kBuiltinLoadMethodElim);
  set(hir_opts.clean_cfg, PassConfig::kCleanCFG);
  set(hir_opts.dynamic_comparison_elim, PassConfig::kDynamicComparisonElim);
  set(hir_opts.guard_type_removal, PassConfig::kGuardTypeRemoval);
  // Inliner currently depends on code objects being stable.
  set(hir_opts.inliner && getConfig().stable_frame, PassConfig::kInliner);
  set(hir_opts.insert_update_prev_instr, PassConfig::kInsertUpdatePrevInstr);
  set(hir_opts.phi_elim, PassConfig::kPhiElim);
  set(hir_opts.simplify, PassConfig::kSimplify);

  return static_cast<PassConfig>(result);
}

std::unique_ptr<CompiledFunction> Compiler::Compile(
    const jit::hir::Preloader& preloader) {
  const std::string& fullname = preloader.fullname();
  if (!PyDict_CheckExact(preloader.globals())) {
    JIT_DLOG(
        "Refusing to compile {}: globals is a {:.200}, not a dict",
        fullname,
        Py_TYPE(preloader.globals())->tp_name);
    return nullptr;
  }

  PyObject* builtins = preloader.builtins();
  if (!PyDict_CheckExact(builtins)) {
    JIT_DLOG(
        "Refusing to compile {}: builtins is a {:.200}, not a dict",
        fullname,
        Py_TYPE(builtins)->tp_name);
    return nullptr;
  }
  JIT_DLOG("Compiling {}", fullname);

  std::unique_ptr<CompilationPhaseTimer> compilation_phase_timer{nullptr};

  if (captureCompilationTimeFor(fullname)) {
    compilation_phase_timer = std::make_unique<CompilationPhaseTimer>(fullname);
    compilation_phase_timer->start("Overall compilation");
    compilation_phase_timer->start("Lowering into HIR");
  }

  Timer timer;
  std::unique_ptr<hir::Function> irfunc(hir::buildHIR(preloader));
  irfunc->reifier = ThreadedRef<>::create(preloader.reifier());
  if (nullptr != compilation_phase_timer) {
    compilation_phase_timer->end();
  }

  if (getConfig().log.dump_hir_initial) {
    JIT_LOG("Initial HIR for {}:\n{}", fullname, *irfunc);
  }

  if (nullptr != compilation_phase_timer) {
    irfunc->setCompilationPhaseTimer(std::move(compilation_phase_timer));
  }

  PassConfig config = createConfig();
  COMPILE_TIMER(
      irfunc->compilation_phase_timer,
      "HIR transformations",
      Compiler::runPasses(*irfunc, config))

  hir::OpcodeCounts hir_opcode_counts = hir::count_opcodes(*irfunc);

  auto ngen = ngen_factory_(irfunc.get());
  if (ngen == nullptr) {
    return nullptr;
  }

  vectorcallfunc entry = nullptr;
  COMPILE_TIMER(
      irfunc->compilation_phase_timer,
      "Native code Generation",
      entry = reinterpret_cast<vectorcallfunc>(ngen->getVectorcallEntry()))
  if (entry == nullptr) {
    JIT_DLOG("Generating native code for {} failed", fullname);
    return nullptr;
  }

  auto compile_time =
      std::chrono::duration_cast<std::chrono::microseconds>(timer.finish());

  JIT_DLOG(
      "Finished compiling {} in {}, code size: {} bytes",
      fullname,
      compile_time,
      ngen->getCodeBuffer().size_bytes());
  if (nullptr != irfunc->compilation_phase_timer) {
    irfunc->compilation_phase_timer->end();
    irfunc->setCompilationPhaseTimer(nullptr);
  }

  int stack_size = ngen->GetCompiledFunctionStackSize();
  int spill_stack_size = ngen->GetCompiledFunctionSpillStackSize();

  // Grab some fields off of irfunc and ngen before moving them.
  hir::Function::InlineFunctionStats inline_stats =
      std::move(irfunc->inline_function_stats);
  std::span<const std::byte> code = ngen->getCodeBuffer();
  void* static_entry = ngen->getStaticEntry();
  auto code_runtime = ngen->codeRuntime();

  auto compiled_func = std::make_unique<CompiledFunction>(
      code,
      entry,
      static_entry,
      stack_size,
      spill_stack_size,
      std::move(inline_stats),
      hir_opcode_counts,
      code_runtime);
  compiled_func->setCompileTime(compile_time);
  compiled_func->setCodePatchers(std::move(irfunc->code_patchers));
  if (getConfig().log.debug) {
    irfunc->setCompilationPhaseTimer(nullptr);
    compiled_func->setHirFunc(std::move(irfunc));
  }
  return compiled_func;
}

} // namespace jit

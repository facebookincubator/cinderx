// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiler.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/optimization.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/Jit/jit_time_log.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>

namespace jit {

template <typename T>
static void runPass(T&& pass, hir::Function& func, PostPassFunction callback) {
  COMPILE_TIMER(func.compilation_phase_timer,
                pass.name(),
                JIT_LOGIF(
                    g_dump_hir_passes,
                    "HIR for {} before pass {}:\n{}",
                    func.fullname,
                    pass.name(),
                    func);

                Timer timer;
                pass.Run(func);
                std::size_t time_ns = timer.finish().count();
                callback(func, pass.name(), time_ns);

                JIT_LOGIF(
                    g_dump_hir_passes,
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

#if PY_VERSION_HEX >= 0x030C0000
  runPassIf(
      jit::hir::InsertUpdatePrevInstr{}, PassConfig::kInsertUpdatePrevInstr);
#endif

  JIT_LOGIF(
      g_dump_final_hir, "Optimized HIR for {}:\n{}", irfunc.fullname, irfunc);
}

std::unique_ptr<CompiledFunction> Compiler::Compile(
    BorrowedRef<PyFunctionObject> func) {
  JIT_CHECK(PyFunction_Check(func), "Expected PyFunctionObject");
  JIT_CHECK(
      !getThreadedCompileContext().compileRunning(),
      "multi-thread compile must preload first");
  std::unique_ptr<hir::Preloader> preloader =
      hir::Preloader::makePreloader(func);
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
  std::unique_ptr<jit::hir::Function> irfunc(jit::hir::buildHIR(preloader));
  std::chrono::nanoseconds hir_build_time = timer.finish();
  if (nullptr != compilation_phase_timer) {
    compilation_phase_timer->end();
  }

  if (g_dump_hir) {
    JIT_LOG("Initial HIR for {}:\n{}", fullname, *irfunc);
  }

  if (nullptr != compilation_phase_timer) {
    irfunc->setCompilationPhaseTimer(std::move(compilation_phase_timer));
  }

  PassConfig config = createConfig();
  std::unique_ptr<nlohmann::json> json{nullptr};
  if (!g_dump_hir_passes_json.empty()) {
    // TODO(emacs): For inlined functions, grab the sources from all the
    // different functions inlined.
    json.reset(new nlohmann::json());
    nlohmann::json passes;
    hir::JSONPrinter hir_printer;
    passes.emplace_back(hir_printer.PrintSource(*irfunc));
    passes.emplace_back(hir_printer.PrintBytecode(*irfunc));
    PostPassFunction dump = [&hir_printer, &passes](
                                hir::Function& func,
                                std::string_view pass_name,
                                std::size_t time_ns) {
      hir_printer.Print(passes, func, pass_name, time_ns);
    };
    dump(*irfunc, "Initial HIR", hir_build_time.count());
    COMPILE_TIMER(
        irfunc->compilation_phase_timer,
        "HIR transformations",
        Compiler::runPasses(*irfunc, config, dump))
    (*json)["fullname"] = fullname;
    (*json)["cols"] = passes;
  } else {
    COMPILE_TIMER(
        irfunc->compilation_phase_timer,
        "HIR transformations",
        Compiler::runPasses(*irfunc, config))
  }
  hir::OpcodeCounts hir_opcode_counts = hir::count_opcodes(*irfunc);

  auto ngen = ngen_factory_(irfunc.get());
  if (ngen == nullptr) {
    return nullptr;
  }

  if (!g_dump_hir_passes_json.empty()) {
    ngen->SetJSONOutput(json.get());
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

  if (!g_dump_hir_passes_json.empty()) {
    std::string filename =
        fmt::format("{}/function_{}.json", g_dump_hir_passes_json, fullname);
    JIT_DLOG("Dumping JSON for {} to {}", fullname, filename);
    std::ofstream json_file;
    json_file.open(
        filename,
        std::ios_base::out | std::ios_base::trunc | std::ios_base::binary);
    json_file << json->dump() << std::endl;
    json_file.close();
  }

  // Grab some fields off of irfunc and ngen before moving them.
  hir::Function::InlineFunctionStats inline_stats =
      std::move(irfunc->inline_function_stats);
  std::span<const std::byte> code = ngen->getCodeBuffer();
  void* static_entry = ngen->getStaticEntry();

  if (g_debug) {
    irfunc->setCompilationPhaseTimer(nullptr);
    return std::make_unique<CompiledFunctionDebug>(
        std::move(irfunc),
        code,
        entry,
        static_entry,
        stack_size,
        spill_stack_size,
        std::move(inline_stats),
        hir_opcode_counts);
  }
  return std::make_unique<CompiledFunction>(
      code,
      entry,
      static_entry,
      stack_size,
      spill_stack_size,
      std::move(inline_stats),
      hir_opcode_counts);
}

} // namespace jit

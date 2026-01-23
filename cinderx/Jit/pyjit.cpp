// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/pyjit.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "cinder/genobject_jit.h"
#include "internal/pycore_ceval.h"
#include "internal/pycore_shadow_frame.h"
#endif

#include "internal/pycore_pystate.h"
#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp_structs.h"
#endif

#include "cinderx/Common/audit.h"
#include "cinderx/Common/code.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/import.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/string.h"
#include "cinderx/Common/type.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/elf/writer.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/annotation_index.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/jit_flag_processor.h"
#include "cinderx/Jit/jit_gdb_support.h"
#include "cinderx/Jit/jit_list.h"
#include "cinderx/Jit/jit_time_log.h"
#include "cinderx/Jit/mmap_file.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/Shadowcode/shadowcode.h"
#include "cinderx/module_state.h"

#include <dlfcn.h>
#include <fmt/std.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_set>
#include <utility>

using namespace jit;

namespace {

// RAII device for disabling GIL checking.
class DisableGilCheck {
 public:
  DisableGilCheck() : old_check_enabled_{_PyRuntime.gilstate.check_enabled} {
    _PyRuntime.gilstate.check_enabled = 0;
  }

  ~DisableGilCheck() {
    _PyRuntime.gilstate.check_enabled = old_check_enabled_;
  }

 private:
  int old_check_enabled_;
};

// Amount of time taken to batch compile everything when disable_jit is called
std::chrono::milliseconds g_batch_compilation_time;

CompilerContext<Compiler>* jitCtx() {
  auto state = cinderx::getModuleState();
  if (state != nullptr) {
    return static_cast<CompilerContext<Compiler>*>(state->jitContext());
  }
  return nullptr;
}

// Only set during preloading. Used to keep track of functions that were
// deleted as a side effect of preloading.
using UnitDeletedCallback = std::function<void(PyObject*)>;
UnitDeletedCallback handle_unit_deleted_during_preload = nullptr;

std::atomic<int> g_compile_workers_attempted;
std::atomic<int> g_compile_workers_retries;

int jit_help = 0;

uint64_t countCalls(PyCodeObject* code) {
#if SHADOWCODE_SUPPORTED
  return code->co_mutable->ncalls;
#else
  auto extra = codeExtra(code);
  return extra != nullptr ? extra->calls : 0;
#endif
}

// If functions in the cinderx module get compiled, they will somehow keep the
// module alive forever and the module will never get finalized on shutdown.
// This breaks many assumptions and has a high chance of use-after-frees or ASAN
// errors on shutdown.
//
// This is a hack around that by preventing the JIT from compiling anything in
// cinderx.
bool isCinderModule(BorrowedRef<> module_name) {
  if (module_name == nullptr || !PyUnicode_Check(module_name)) {
    return false;
  }
  std::string_view name = PyUnicode_AsUTF8(module_name);
  return name == "cinderx";
}

bool shouldAlwaysScheduleCompile(BorrowedRef<PyCodeObject> code) {
  // There's a config option for forcing all Static Python functions to be
  // compiled.
  bool is_static = code->co_flags & CI_CO_STATICALLY_COMPILED;
  return is_static && getConfig().compile_all_static_functions;
}

// Check if a function has been preloaded.
bool isPreloaded(BorrowedRef<PyFunctionObject> func) {
  return hir::preloaderManager().find(func) != nullptr;
}

_PyJIT_Result tryCompile(BorrowedRef<PyFunctionObject> func) {
  _PyJIT_Result result = compileFunction(func);
  // Reset the function back to the interpreter if there was any non-retryable
  // failure.
  if (result != PYJIT_RESULT_OK && result != PYJIT_RESULT_RETRY) {
    func->vectorcall = getInterpretedVectorcall(func);
  }
  return result;
}

void incrementShadowcodeCall([[maybe_unused]] BorrowedRef<PyCodeObject> code) {
#if SHADOWCODE_SUPPORTED
  // The interpreter will only increment up to the shadowcode threshold
  // PYSHADOW_INIT_THRESHOLD. After that, it will stop incrementing. If someone
  // sets -X jit-auto above the PYSHADOW_INIT_THRESHOLD, we still have to keep
  // counting.
  if (code->co_mutable->ncalls > PYSHADOW_INIT_THRESHOLD) {
    code->co_mutable->ncalls++;
  }
#endif
}

// Like jitVectorcall(), but ignores any call count requirements.
PyObject* forcedJitVectorcall(
    PyObject* func_obj,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(
      PyFunction_Check(func_obj),
      "Called JIT wrapper with {} object instead of a function",
      Py_TYPE(func_obj)->tp_name);
  BorrowedRef<PyFunctionObject> func{func_obj};
  BorrowedRef<PyCodeObject> code{func->func_code};

  _PyJIT_Result result = tryCompile(func);
  if (result == PYJIT_RESULT_OK) {
    JIT_DCHECK(
        isJitCompiled(func),
        "JIT succeeded for function {} but it is not recognized as compiled",
        funcFullname(func));
  } else {
    JIT_DCHECK(
        !isJitCompiled(func),
        "JIT failed (error: {}) for function {} but it seems to have been "
        "compiled",
        result,
        funcFullname(func));
  }

  switch (result) {
    case PYJIT_RESULT_OK:
      return func->vectorcall(func_obj, stack, nargsf, kwnames);
    case PYJIT_RESULT_RETRY: {
      incrementShadowcodeCall(code);
      auto entry = getInterpretedVectorcall(func);
      return entry(func_obj, stack, nargsf, kwnames);
    }
    case PYJIT_RESULT_CANNOT_SPECIALIZE:
    case PYJIT_RESULT_NOT_ON_JITLIST:
    case PYJIT_NOT_INITIALIZED:
    case PYJIT_RESULT_NO_PRELOADER:
    case PYJIT_RESULT_UNKNOWN_ERROR:
    case PYJIT_OVER_MAX_CODE_SIZE:
      return func->vectorcall(func_obj, stack, nargsf, kwnames);
    case PYJIT_RESULT_PYTHON_EXCEPTION:
      return nullptr;
    default:
      break;
  }

  JIT_ABORT(
      "Unrecognized JIT result code {} for function {}",
      static_cast<int>(result),
      funcFullname(func));
}

// Python function entry point when the JIT is enabled.
PyObject* jitVectorcall(
    PyObject* func_obj,
    PyObject* const* stack,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(
      PyFunction_Check(func_obj),
      "Called JIT wrapper with {} object instead of a function",
      Py_TYPE(func_obj)->tp_name);
  BorrowedRef<PyFunctionObject> func{func_obj};
  BorrowedRef<PyCodeObject> code{func->func_code};

  // If there's a call count limit, interpret the function as usual until the
  // limit is reached.
  if (auto limit = getConfig().compile_after_n_calls; limit.has_value()) {
    auto const calls = countCalls(code);
    if (calls < *limit) {
      incrementShadowcodeCall(code);
      auto entry = getInterpretedVectorcall(func);
      return entry(func_obj, stack, nargsf, kwnames);
    }
  }

  return forcedJitVectorcall(func_obj, stack, nargsf, kwnames);
}

void setJitLogFile(const std::string& log_filename) {
  // Redirect logging to a file if configured.
  const char* kPidMarker = "{pid}";
  std::string pid_filename = log_filename;
  auto marker_pos = pid_filename.find(kPidMarker);
  if (marker_pos != std::string::npos) {
    pid_filename.replace(
        marker_pos, std::strlen(kPidMarker), fmt::format("{}", getpid()));
  }
  FILE* file = fopen(pid_filename.c_str(), "w");
  if (file == nullptr) {
    JIT_LOG(
        "Couldn't open log file {} ({}), logging to stderr",
        pid_filename,
        strerror(errno));
  } else {
    getMutableConfig().log.output_file = file;
  }
}

void setASMSyntax(const std::string& asm_syntax) {
  if (asm_syntax.compare("intel") == 0) {
    getMutableConfig().asm_syntax = AsmSyntax::Intel;
  } else if (asm_syntax.compare("att") == 0) {
    getMutableConfig().asm_syntax = AsmSyntax::ATT;
  } else {
    JIT_ABORT("Unknown asm syntax '{}'", asm_syntax);
  }
}

size_t parse_sized_argument(const std::string& val) {
  std::string parsed;
  // " 1024 k" should parse OK - so remove the space.
  std::remove_copy_if(
      val.begin(), val.end(), std::back_inserter(parsed), ::isspace);
  JIT_CHECK(!parsed.empty(), "Input string is empty");
  static_assert(
      sizeof(decltype(std::stoull(parsed))) == sizeof(size_t),
      "stoull parses to size_t size");
  size_t scale = 1;
  // "1024k" and "1024K" are the same - so upper case.
  char lastChar = std::toupper(parsed.back());
  switch (lastChar) {
    case 'K':
      scale = 1024;
      parsed.pop_back();
      break;
    case 'M':
      scale = 1024 * 1024;
      parsed.pop_back();
      break;
    case 'G':
      scale = 1024 * 1024 * 1024;
      parsed.pop_back();
      break;
    default:
      JIT_CHECK(
          std::isdigit(lastChar), "Invalid character in input string: {}", val);
  }
  size_t ret_value{0};
  auto p_last = parsed.data() + parsed.size();
  auto int_ok = std::from_chars(parsed.data(), p_last, ret_value);
  JIT_CHECK(
      int_ok.ec == std::errc() && int_ok.ptr == p_last,
      "Invalid unsigned integer in input string: '{}'",
      val);
  JIT_CHECK(
      ret_value <= (std::numeric_limits<size_t>::max() / scale),
      "Unsigned Integer overflow in input string: '{}'",
      val);
  return ret_value * scale;
}

FlagProcessor initFlagProcessor() {
  jit_help = 0;

  FlagProcessor flag_processor;

  // Flags are inspected in order of definition below.

  flag_processor.addOption(
      "jit-dump-hir-stats",
      "PYTHONJITDUMPHIRSTATS",
      getMutableConfig().dump_hir_stats,
      "Dump counts of instructions and types per function");

  flag_processor.addOption(
      "jit-all",
      "PYTHONJITALL",
      [](uint32_t) { getMutableConfig().compile_after_n_calls = 0; },
      "Enable the JIT and set it to compile all functions as soon as they are "
      "called");

  flag_processor.addOption(
      "jit-auto",
      "PYTHONJITAUTO",
      [](uint32_t val) { getMutableConfig().compile_after_n_calls = val; },
      "Enable auto-JIT mode, which compiles functions after the given "
      "threshold");

  flag_processor.addOption(
      "jit-debug",
      "PYTHONJITDEBUG",
      getMutableConfig().log.debug,
      "JIT debug and extra logging");

  flag_processor
      .addOption(
          "jit-log-file",
          "PYTHONJITLOGFILE",
          [](const std::string& log_filename) { setJitLogFile(log_filename); },
          "write log entries to <filename> rather than stderr")
      .withFlagParamName("filename");

  flag_processor
      .addOption(
          "jit-asm-syntax",
          "PYTHONJITASMSYNTAX",
          [](const std::string& asm_syntax) { setASMSyntax(asm_syntax); },
          "set the assembly syntax used in log files")
      .withFlagParamName("intel|att")
      .withDebugMessageOverride("Sets the assembly syntax used in log files");

  flag_processor
      .addOption(
          "jit-debug-refcount",
          "PYTHONJITDEBUGREFCOUNT",
          getMutableConfig().log.debug_refcount,
          "JIT refcount insertion debug mode")
      .withDebugMessageOverride("Enabling");

  flag_processor.addOption(
      "jit-debug-regalloc",
      "PYTHONJITDEBUGREGALLOC",
      getMutableConfig().log.debug_regalloc,
      "Enable or disable debug logging for the register allocator");

  flag_processor.addOption(
      "jit-debug-inliner",
      "PYTHONJITDEBUGINLINER",
      getMutableConfig().log.debug_inliner,
      "Enable or disable debug logging for the JIT's HIR inliner");

  flag_processor
      .addOption(
          "jit-dump-hir",
          "PYTHONJITDUMPHIR",
          getMutableConfig().log.dump_hir_initial,
          "Log the HIR representation of all functions after initial "
          "lowering from bytecode")
      .withDebugMessageOverride("Dump initial HIR of JITed functions");

  flag_processor
      .addOption(
          "jit-dump-hir-passes",
          "PYTHONJITDUMPHIRPASSES",
          getMutableConfig().log.dump_hir_passes,
          "Log the HIR after each optimization pass")
      .withDebugMessageOverride(
          "Dump HIR of JITed functions after each individual optimization "
          "pass");

  flag_processor
      .addOption(
          "jit-dump-final-hir",
          "PYTHONJITDUMPFINALHIR",
          getMutableConfig().log.dump_hir_final,
          "Log the HIR after all optimizations")
      .withDebugMessageOverride(
          "Dump final HIR of JITed functions after all optimizations");

  flag_processor
      .addOption(
          "jit-dump-lir",
          "PYTHONJITDUMPLIR",
          getMutableConfig().log.dump_lir,
          "Log the LIR representation of functions after lowering from HIR")
      .withDebugMessageOverride("Dump initial LIR of JITed functions");

  flag_processor.addOption(
      "jit-dump-lir-origin",
      "PYTHONJITDUMPLIRORIGIN",
      [](bool value) {
        getMutableConfig().log.dump_lir = true;
        getMutableConfig().log.lir_origin = value;
      },
      "Enable or disable whether LIR is displayed with HIR origin data");

  flag_processor.addOption(
      "jit-symbolize",
      "PYTHONJITSYMBOLIZE",
      getMutableConfig().log.symbolize_funcs,
      "Enable or disable symbolization of functions called by JIT code");

  flag_processor
      .addOption(
          "jit-dump-asm",
          "PYTHONJITDUMPASM",
          getMutableConfig().log.dump_asm,
          "log the final compiled code, annotated with HIR instructions")
      .withDebugMessageOverride("Dump asm of JITed functions");

  flag_processor.addOption(
      "jit-enable-inline-cache-stats-collection",
      "PYTHONJITCOLLECTINLINECACHESTATS",
      getMutableConfig().collect_attr_cache_stats,
      "Collect inline cache stats (supported stats are cache misses for load "
      "method inline caches");

  flag_processor.addOption(
      "jit-gdb-support",
      "PYTHONJITGDBSUPPORT",
      [](bool value) {
        getMutableConfig().log.debug = value;
        getMutableConfig().gdb.supported = value;
      },
      "Enable or disable GDB support and JIT debug mode");

  flag_processor.addOption(
      "jit-gdb-write-elf",
      "PYTHONJITGDBWRITEELF",
      [](bool value) {
        getMutableConfig().log.debug = value;
        getMutableConfig().gdb.supported = value;
        getMutableConfig().gdb.write_elf_objects = value;
      },
      "Debugging aid, GDB support with ELF output");

  flag_processor.addOption(
      "jit-dump-stats",
      "PYTHONJITDUMPSTATS",
      getMutableConfig().log.dump_stats,
      "Dump JIT runtime stats at shutdown");

  flag_processor.addOption(
      "jit-huge-pages",
      "PYTHONJITHUGEPAGES",
      getMutableConfig().use_huge_pages,
      "Enable or disable huge pages for compiled functions");

  flag_processor.addOption(
      "jit-enable-jit-list-wildcards",
      "PYTHONJITENABLEJITLISTWILDCARDS",
      getMutableConfig().allow_jit_list_wildcards,
      "allow wildcards in JIT list");

  flag_processor.addOption(
      "jit-all-static-functions",
      "PYTHONJITALLSTATICFUNCTIONS",
      getMutableConfig().compile_all_static_functions,
      "JIT-compile all static functions");

  flag_processor
      .addOption(
          "jit-list-file",
          "PYTHONJITLISTFILE",
          getMutableConfig().jit_list.filename,
          "Load list of functions to compile from <filename>")
      .withFlagParamName("filename");

  flag_processor.addOption(
      "jit-list-fail-on-parse-error",
      "PYTHONJITLISTFAILONPARSEERROR",
      getMutableConfig().jit_list.error_on_parse,
      "Raise a Python exception when a JIT list fails to parse");

  flag_processor.addOption(
      "jit-disable",
      "PYTHONJITDISABLE",
      [](int val) {
        // Only update force_init if it wasn't already set.
        if (val && !getConfig().force_init.has_value()) {
          getMutableConfig().force_init = false;
        }
      },
      "disable the JIT");

  flag_processor.addOption(
      "jit-shadow-frame",
      "PYTHONJITSHADOWFRAME",
      [](int val) {
        // Cinder's shadow frames are not supported in Python versions later
        // than 3.10.
        if constexpr (PY_VERSION_HEX >= 0x030B0000) {
          return;
        }
        getMutableConfig().frame_mode =
            val ? FrameMode::kShadow : FrameMode::kNormal;
      },
      "enable shadow frame mode");

  flag_processor.addOption(
      "jit-lightweight-frame",
      "PYTHONJITLIGHTWEIGHTFRAME",
      [](int val) {
        if constexpr (PY_VERSION_HEX < 0x030C0000) {
          JIT_DLOG(
              "Lightweight frames are not supported in Python versions earlier "
              "than 3.12");
          return;
        }
        getMutableConfig().frame_mode =
            val ? FrameMode::kLightweight : FrameMode::kNormal;
      },
      "Enable/disable JIT lightweight frames");

  flag_processor.addOption(
      "jit-stable-frame",
      "PYTHONJITSTABLEFRAME",
      getMutableConfig().stable_frame,
      "Assume that data found in the Python frame is unchanged across "
      "function calls");

  flag_processor.addOption(
      "jit-preload-dependent-limit",
      "PYTHONJITPRELOADDEPENDENTLIMIT",
      getMutableConfig().preload_dependent_limit,
      "When compiling a function, set the number of dependent functions that "
      "can be compiled along with it.");

  // HIR optimizations.

#define HIR_OPTIMIZATION_OPTION(NAME, OPT, CLI, ENV) \
  flag_processor.addOption(                          \
      (CLI),                                         \
      (ENV),                                         \
      getMutableConfig().hir_opts.OPT,               \
      "Enable the HIR " NAME " optimization pass")

  HIR_OPTIMIZATION_OPTION(
      "BeginInlinedFunction elimination",
      begin_inlined_function_elim,
      "jit-begin-inlined-function-elim",
      "PYTHONJITBEGININLINEDFUNCTIONELIM");
  HIR_OPTIMIZATION_OPTION(
      "builtin LoadMethod elimination",
      builtin_load_method_elim,
      "jit-builtin-load-method-elim",
      "PYTHONJITBUILTINLOADMETHODELIM");
  HIR_OPTIMIZATION_OPTION(
      "CFG cleaning", clean_cfg, "jit-clean-cfg", "PYTHONJITCLEANCFG");
  HIR_OPTIMIZATION_OPTION(
      "dead code elimination",
      dead_code_elim,
      "jit-dead-code-elim",
      "PYTHONJITDEADCODEELIM");
  HIR_OPTIMIZATION_OPTION(
      "dynamic comparison elimination",
      dynamic_comparison_elim,
      "jit-dynamic-comparison-elim",
      "PYTHONJITDYNAMICCOMPARISIONELIM");
  HIR_OPTIMIZATION_OPTION(
      "guard type removal",
      guard_type_removal,
      "jit-guard-type-removal",
      "PYTHONJITGUARDTYPEREMOVAL");
  HIR_OPTIMIZATION_OPTION(
      "inliner",
      inliner,
      "jit-enable-hir-inliner",
      "PYTHONJITENABLEHIRINLINER");
  HIR_OPTIMIZATION_OPTION(
      "phi elimination", phi_elim, "jit-phi-elim", "PYTHONJITPHIELIM");
  HIR_OPTIMIZATION_OPTION(
      "simplify", simplify, "jit-simplify", "PYTHONJITSIMPLIFY");

  flag_processor.addOption(
      "jit-simplify-iteration-limit",
      "PYTHONJITSIMPLIFYITERATIONLIMIT",
      getMutableConfig().simplifier.iteration_limit,
      "Set the maximum number of times the simplifier can run over a "
      "function");
  flag_processor.addOption(
      "jit-simplify-new-block-limit",
      "PYTHONJITSIMPLIFYNEWBLOCKLIMIT",
      getMutableConfig().simplifier.new_block_limit,
      "Set the maximum number of blocks that can be added by the simplifier "
      "to a function");
  flag_processor.addOption(
      "jit-hir-inliner-cost-limit",
      "PYTHONJITHIRINLINERCOSTLIMIT",
      getMutableConfig().inliner_cost_limit,
      "Limit how much the inliner is able to inline. The number's definition "
      "is only relevant to the inliner itself.");

  flag_processor.addOption(
      "jit-lir-inliner",
      "PYTHONJITLIRINLINER",
      getMutableConfig().lir_opts.inliner,
      "Enable the LIR inliner");

  flag_processor
      .addOption(
          "jit-batch-compile-workers",
          "PYTHONJITBATCHCOMPILEWORKERS",
          getMutableConfig().batch_compile_workers,
          "set the number of batch compile workers to <COUNT>")
      .withFlagParamName("COUNT");

  flag_processor
      .addOption(
          "jit-multithreaded-compile-test",
          "PYTHONJITMULTITHREADEDCOMPILETEST",
          getMutableConfig().multithreaded_compile_test,
          "JIT multithreaded compile test")
      .isHiddenFlag(true);

  flag_processor.addOption(
      "jit-list-match-line-numbers",
      "PYTHONJITLISTMATCHLINENUMBERS",
      getMutableConfig().jit_list.match_line_numbers,
      "JIT list match line numbers");

  flag_processor
      .addOption(
          "jit-time",
          "",
          [](const std::string& flag_value) {
            parseAndSetFuncList(flag_value);
          },
          "Measure time taken in compilation phases and output summary to "
          "stderr or approperiate logfile. Only functions in comma separated "
          "<function_list> list will be included. Comma separated list may "
          "include wildcards, * and ?. Wildcards are processed in glob "
          "fashion and not as regex.")
      .withFlagParamName("function_list")
      .withDebugMessageOverride(
          "Will capture time taken in compilation phases and output summary");

  flag_processor.addOption(
      "jit-multiple-code-sections",
      "PYTHONJITMULTIPLECODESECTIONS",
      getMutableConfig().multiple_code_sections,
      "Enable emitting code into multiple code sections.");

  flag_processor.addOption(
      "jit-hot-code-section-size",
      "PYTHONJITHOTCODESECTIONSIZE",
      getMutableConfig().hot_code_section_size,
      "Enable emitting code into multiple code sections.");

  flag_processor.addOption(
      "jit-cold-code-section-size",
      "PYTHONJITCOLDCODESECTIONSIZE",
      getMutableConfig().cold_code_section_size,
      "Enable emitting code into multiple code sections.");

  flag_processor.addOption(
      "jit-attr-caches",
      "PYTHONJITATTRCACHES",
      getMutableConfig().attr_caches,
      "Use inline caches for attribute access instructions");

  flag_processor.addOption(
      "jit-attr-cache-size",
      "PYTHONJITATTRCACHESIZE",
      [](uint32_t entries) {
        JIT_CHECK(
            entries > 0 && entries <= 16,
            "Using {} entries for attribute access inline "
            "caches is not within the appropriate range",
            entries);
        getMutableConfig().attr_cache_size = entries;
      },
      "Set the number of entries in the JIT's attribute access inline "
      "caches");

  flag_processor.addOption(
      "jit-refine-static-python",
      "PYTHONJITREFINESTATICPYTHON",
      getMutableConfig().refine_static_python,
      "Add RefineType instructions to coerce Static Python types to be "
      "valid");

  flag_processor.addOption(
      "jit-perfmap",
      "JIT_PERFMAP",
      perf::jit_perfmap,
      "write out /tmp/perf-<pid>.map for JIT symbols");

  flag_processor
      .addOption(
          "jit-perf-dumpdir",
          "JIT_DUMPDIR",
          perf::perf_jitdump_dir,
          "absolute path to a <DIRECTORY> that exists. A perf jitdump file "
          "will be written to this directory")
      .withFlagParamName("DIRECTORY");

  flag_processor.addOption(
      "jit-help", "", jit_help, "print all available JIT flags and exits");

  flag_processor.addOption(
      "perf-trampoline-prefork-compilation",
      "PERFTRAMPOLINEPREFORKCOMPILATION",
      getMutableConfig().compile_perf_trampoline_prefork,
      "Compile perf trampoline pre-fork");

  flag_processor.addOption(
      "jit-max-code-size",
      "PYTHONJITMAXCODESIZE",
      [](const std::string& val) {
        getMutableConfig().max_code_size = parse_sized_argument(val);
      },
      "Set the maximum code size for JIT in bytes (no suffix). For kilobytes "
      "use k or K as a suffix. "
      "Megabytes is m or M and gigabytes is g or G. 0 implies no limit.");

  flag_processor.addOption(
      "jit-emit-type-annotation-guards",
      "PYTHONJITTYPEANNOTATIONGUARDS",
      getMutableConfig().emit_type_annotation_guards,
      "Generate runtime checks that validate type annotations to specialize "
      "generated code.");

  flag_processor.addOption(
      "jit-specialized-opcodes",
      "PYTHONJITSPECIALIZEDOPCODES",
      getMutableConfig().specialized_opcodes,
      "JIT specialized opcodes or to fall back to their generic counterparts.");

#if PY_VERSION_HEX >= 0x030C0000
  flag_processor.addOption(
      "jit-support-instrumentation",
      "PYTHONJITSUPPORTINSTRUMENTATION",
      getMutableConfig().support_instrumentation,
      "Support instrumentation (e.g. monitoring/tracing/profiling)");
#endif

  flag_processor.setFlags(PySys_GetXOptions());

  // T198250666: Bit of a hack but this makes other things easier.  In 3.12 all
  // functions need access to the runtime PyFunctionObject, which prevents
  // inlining.  Our tests check `is_hir_inliner_enabled()` to see if the inliner
  // is functional and make assumptions based on that.  This is only available
  // when we have lightweight frames enabled as we need cooperation w/ the
  // runtime to let us reify the frame.
  //
  // Inlining is only compatible w/ lightweight frames because we need our
  // reifier to cooperate with restoring the frame object into something usable
  // when CPython wants it.
  if (PY_VERSION_HEX >= 0x030C0000 &&
      getConfig().frame_mode != FrameMode::kLightweight) {
    getMutableConfig().hir_opts.inliner = false;
  }

  return flag_processor;
}

void finalizeFunc(
    BorrowedRef<PyFunctionObject> func,
    const CompiledFunction& compiled) {
  ThreadedCompileSerialize guard;
  if (!jitCtx()->addCompiledFunc(func)) {
    // Someone else compiled the function between when our caller checked and
    // called us.
    return;
  }

  // In case the function had previously been deopted.
  jitCtx()->removeDeoptedFunc(func);

  func->vectorcall = compiled.vectorcallEntry();
  Runtime* rt = Runtime::get();
  if (rt->hasFunctionEntryCache(func)) {
    void** indirect = rt->findFunctionEntryCache(func);
    *indirect = compiled.staticEntry();
  }
  return;
}

/*
 * Re-optimize a function by setting it to use JIT-compiled code if there's a
 * matching compiled code object.
 *
 * Intended for functions that have been explicitly deopted and for nested
 * functions.  Nested functions are created and destroyed multiple times but
 * have the same underlying code object.
 *
 * Return true if the function was successfully reopted, false if nothing
 * happened.
 */
bool reoptFunc(BorrowedRef<PyFunctionObject> func) {
  if (jitCtx() == nullptr) {
    return false;
  } else if (jitCtx()->didCompile(func)) {
    return true;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    return false;
  }

  // Might be a nested function that was never explicitly deopted, so ignore the
  // result of this.
  jitCtx()->removeDeoptedFunc(func);

  if (CompiledFunction* compiled = jitCtx()->lookupFunc(func)) {
    finalizeFunc(func, *compiled);
    return true;
  }
  return false;
}

// Check if we have exceeded the max code size limit.
bool isOverMaxCodeSize() {
  auto max_code_size = getConfig().max_code_size;
  ICodeAllocator* code_allocator = cinderx::getModuleState()->codeAllocator();
  return max_code_size && code_allocator->usedBytes() >= max_code_size;
}

Context::CompilationResult compilePreloader(
    BorrowedRef<PyFunctionObject> func,
    const hir::Preloader& preloader) {
  if (isOverMaxCodeSize()) {
    return {nullptr, PYJIT_OVER_MAX_CODE_SIZE};
  }
  jit::Context::CompilationResult result =
      compilePreloaderImpl(jitCtx(), preloader);
  if (result.compiled == nullptr) {
    return result;
  }
  if (func != nullptr) {
    finalizeFunc(func, *result.compiled);
  }
  return result;
}

// Convert a registered translation unit into a pair of a Python function and
// its code object.  When the translation unit only refers to a code object
// (e.g. it's a nested function), the function will be a nullptr.
std::pair<BorrowedRef<PyFunctionObject>, BorrowedRef<PyCodeObject>> splitUnit(
    BorrowedRef<> unit) {
  if (PyFunction_Check(unit)) {
    BorrowedRef<PyFunctionObject> func{unit};
    BorrowedRef<PyCodeObject> code{func->func_code};
    return {func, code};
  }
  JIT_CHECK(
      PyCode_Check(unit),
      "Translation units must be functions or code objects, got '{}'",
      Py_TYPE(unit)->tp_name);

  BorrowedRef<PyCodeObject> code{unit};
  return {nullptr, code};
}

std::string unitFullname(BorrowedRef<> unit) {
  if (unit == nullptr) {
    return "<nullptr>";
  }
  auto [func, code] = splitUnit(unit);
  if (func != nullptr) {
    return funcFullname(func);
  }
  auto& jit_code_outer_funcs = cinderx::getModuleState()->codeOuterFunctions();
  auto iter = jit_code_outer_funcs.find(code);
  if (iter == jit_code_outer_funcs.end()) {
    return fmt::format(
        "<Unknown code object {}>", static_cast<void*>(code.get()));
  }
  return codeFullname(iter->second->func_module, code);
}

// Load the preloader for a given function or code object.  If it doesn't exist
// yet, then preload the function and return the new preloader.
//
// Can potentially hit a Python exception, if so, will forward that along and
// return nullptr.
hir::Preloader* preload(BorrowedRef<> unit) {
  auto [func, code] = splitUnit(unit);
  if (hir::Preloader* existing = hir::preloaderManager().find(code)) {
    return existing;
  }

  // Make a new preloader. Note that this will run Python code so a lot of
  // assumptions are broken after this.
  std::unique_ptr<hir::Preloader> preloader;
  if (func != nullptr) {
    preloader =
        hir::Preloader::makePreloader(func, makeFrameReifier(func->func_code));
  } else {
    auto& jit_code_outer_funcs =
        cinderx::getModuleState()->codeOuterFunctions();
    auto it = jit_code_outer_funcs.find(code);
    if (it == jit_code_outer_funcs.end()) {
      PyErr_Format(
          PyExc_RuntimeError,
          "failed to find code object for preloading: %U",
          code->co_qualname);
      return nullptr;
    }
    BorrowedRef<PyFunctionObject>& outer_func = it->second;
    // Assuming the builtins + globals will always be a dictionary goes way back
    // in the JIT's history. I'm not sure what guarantees this though. Tread
    // carefully but try not to blow things up if this happens in production
    // code.
    JIT_DCHECK(
        PyDict_CheckExact(outer_func->func_builtins),
        "Unexpected type for builtins ({}) on function {}",
        Py_TYPE(outer_func->func_builtins)->tp_name,
        funcFullname(outer_func));
    JIT_DCHECK(
        PyDict_CheckExact(outer_func->func_globals),
        "Unexpected type for globals ({}) on function {}",
        Py_TYPE(outer_func->func_globals)->tp_name,
        funcFullname(outer_func));
    preloader = hir::Preloader::makePreloader(
        code,
        outer_func->func_builtins,
        outer_func->func_globals,
        hir::AnnotationIndex::from_function(outer_func),
        codeFullname(outer_func->func_module, code),
        makeFrameReifier(code));
  }

  if (preloader == nullptr) {
    JIT_CHECK(
        PyErr_Occurred(), "Expect a Python exception when preloading fails");
    return nullptr;
  }

  // Have to check again for an existing preloader, because the preloader might
  // have re-entered itself when running Python code.
  if (hir::Preloader* existing = hir::preloaderManager().find(code)) {
    return existing;
  }

  // Grab a copy of the raw pointer before it gets moved away.
  auto copy = preloader.get();
  hir::preloaderManager().add(code, std::move(preloader));
  return copy;
}

// JIT compile func or code object, only if a preloader is available.
//
// Re-entrant compile that is safe to call from within compilation, because it
// will only use an already-created preloader, it will not preload, and
// therefore it cannot raise a Python exception.
//
// Returns PYJIT_RESULT_NO_PRELOADER if no preloader is available.
_PyJIT_Result tryCompilePreloaded(BorrowedRef<> unit) {
  auto [func, code] = splitUnit(unit);
  hir::Preloader* preloader = hir::preloaderManager().find(code);
  return preloader ? compilePreloader(func, *preloader).result
                   : PYJIT_RESULT_NO_PRELOADER;
}

void compile_worker_thread() {
  JIT_DLOG("Started compile worker in thread {}", std::this_thread::get_id());

  size_t attempts = 0;
  size_t retries = 0;

  while (BorrowedRef<> unit = getThreadedCompileContext().nextUnit()) {
    attempts++;
    _PyJIT_Result res = tryCompilePreloaded(unit);
    if (res == PYJIT_RESULT_RETRY) {
      retries++;
      getThreadedCompileContext().retryUnit(unit);
    }
    JIT_CHECK(
        res != PYJIT_RESULT_NO_PRELOADER,
        "Cannot find a JIT preloader for {}",
        unitFullname(unit));
  }

  g_compile_workers_attempted.fetch_add(attempts);
  g_compile_workers_retries.fetch_add(retries);

  JIT_DLOG(
      "Finished compile worker in thread {}. Compile attempts: {}, scheduled "
      "retries: {}",
      std::this_thread::get_id(),
      attempts,
      retries);
}

void compile_units_preloaded(std::vector<BorrowedRef<>>&& units) {
  for (auto unit : units) {
    tryCompilePreloaded(unit);
  }
}

void multithread_compile_units_preloaded(
    std::vector<BorrowedRef<>>&& units,
    size_t worker_count) {
  JIT_CHECK(worker_count > 1, "Expecting >1 workers but got {}", worker_count);

  JIT_DLOG(
      "Running multithread_compile_units_preloaded for {} units with {} "
      "workers",
      units.size(),
      worker_count);

  // Disable checks for using GIL protected data across threads.
  // Conceptually what we're doing here is saying we're taking our own
  // responsibility for managing locking of CPython runtime data structures.
  // Instead of holding the GIL to serialize execution to one thread, we're
  // holding the GIL for a group of co-operating threads which are aware of each
  // other. We still need the GIL as this protects the cooperating threads from
  // unknown other threads. Within our group of cooperating threads we can
  // safely do any read-only operations in parallel, but we grab our own lock if
  // we do a write (e.g. an incref).
  DisableGilCheck gil_check_guard;

  getThreadedCompileContext().startCompile(std::move(units));
  std::vector<std::thread> worker_threads;
  {
    // Ensure that no worker threads start compiling until they are all created,
    // in case something else in the process has hooked thread creation to run
    // arbitrary code.
    ThreadedCompileSerialize guard;
    for (size_t i = 0; i < worker_count; i++) {
      worker_threads.emplace_back(compile_worker_thread);
    }
  }
  for (std::thread& worker_thread : worker_threads) {
    worker_thread.join();
  }

  auto retry_list = getThreadedCompileContext().endCompile();

  Runtime::get()->fixupFunctionEntryCachePostMultiThreadedCompile();
  Runtime::get()->watchPendingTypes();

  JIT_DLOG(
      "multithread_compile_units_preloaded retrying {} units serially",
      retry_list.size());
  compile_units_preloaded(std::move(retry_list));
}

// Compile all functions registered via a JIT list that haven't been executed
// yet.
bool compile_all(size_t workers = 0) {
  JIT_CHECK(jitCtx(), "JIT not initialized");
  std::chrono::time_point start = std::chrono::steady_clock::now();

  if (workers == 0) {
    workers = std::max(getConfig().batch_compile_workers, 1ul);
  }

  std::vector<BorrowedRef<>> compilation_units;
  // units that were deleted during preloading
  std::unordered_set<PyObject*> deleted_units;

  auto error_cleanup = [&]() {
    hir::preloaderManager().clear();
    handle_unit_deleted_during_preload = nullptr;
  };

  auto& jit_reg_units = cinderx::getModuleState()->registeredCompilationUnits();
  JIT_DLOG(
      "Starting compile_all with {} workers for {} registered units",
      workers,
      jit_reg_units.size());

  // First we have to preload everything we are going to compile.
  while (jit_reg_units.size() > 0) {
    auto preload_units = std::move(jit_reg_units);
    jit_reg_units.clear();
    JIT_DLOG(
        "compile_all preloading a batch of {} units", preload_units.size());

    for (auto unit : preload_units) {
      if (deleted_units.contains(unit)) {
        continue;
      }
      handle_unit_deleted_during_preload = [&](PyObject* deleted_unit) {
        deleted_units.emplace(deleted_unit);
      };
      hir::Preloader* preloader = preload(unit);
      if (!preloader) {
        error_cleanup();
        return false;
      }
      compilation_units.push_back(unit);
    }
  }
  handle_unit_deleted_during_preload = nullptr;

  // Filter out any units that were deleted as a side effect of preloading.
  std::erase_if(compilation_units, [&](BorrowedRef<> unit) {
    return deleted_units.contains(unit);
  });

  JIT_DLOG(
      "compile_all finished preloading {} units, {} were deleted",
      compilation_units.size(),
      deleted_units.size());

  if (workers > 1) {
    multithread_compile_units_preloaded(std::move(compilation_units), workers);
  } else {
    compile_units_preloaded(std::move(compilation_units));
  }

  hir::preloaderManager().clear();

  std::chrono::time_point end = std::chrono::steady_clock::now();
  g_batch_compilation_time =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  auto& jit_code_outer_funcs = cinderx::getModuleState()->codeOuterFunctions();
  jit_code_outer_funcs.clear();

  return true;
}

// Recursively search the given co_consts tuple for any code objects that are
// on the current jit-list, using the given module name to form a
// fully-qualified function name.
std::vector<BorrowedRef<PyCodeObject>> findNestedCodes(
    BorrowedRef<> module,
    BorrowedRef<> root_consts) {
  std::queue<PyObject*> consts_tuples;
  std::unordered_set<PyCodeObject*> visited;
  std::vector<BorrowedRef<PyCodeObject>> result;

  consts_tuples.push(root_consts);
  while (!consts_tuples.empty()) {
    PyObject* consts = consts_tuples.front();
    consts_tuples.pop();

    for (size_t i = 0, size = PyTuple_GET_SIZE(consts); i < size; ++i) {
      BorrowedRef<PyCodeObject> code = PyTuple_GET_ITEM(consts, i);
      if (!PyCode_Check(code) || !visited.insert(code).second ||
          code->co_qualname == nullptr ||
          !shouldScheduleCompile(module, code)) {
        continue;
      }

      result.emplace_back(code);
      consts_tuples.emplace(code->co_consts);
    }
  }

  return result;
}

// Register a function with the JIT to be compiled in the future.
//
// The JIT will run compileFunction() before the function executes on its next
// call.  The JIT can still choose to **not** compile the function at that
// point.
//
// The JIT will not keep the function alive, instead it will be informed that
// the function is being de-allocated via funcDestroyed() before the function
// goes away.
//
// Return true if the function is registered with JIT or is already compiled,
// and false otherwise.
bool registerFunction(BorrowedRef<PyFunctionObject> func) {
  // Attempt to attach already-compiled code even if the JIT is disabled, as
  // long as it hasn't been finalized.
  if (reoptFunc(func)) {
    return true;
  }

  if (!isJitUsable()) {
    return false;
  }

  if (isOverMaxCodeSize()) {
    return false;
  }

  JIT_CHECK(
      !getThreadedCompileContext().compileRunning(),
      "Not intended for using during threaded compilation");
  auto& jit_reg_units = cinderx::getModuleState()->registeredCompilationUnits();
  jit_reg_units.emplace(func.getObj());

  // If we have an active jit-list, scan this function's code object for any
  // nested functions that might be on the jit-list, and register them as well.
  if (cinderx::getModuleState()->jitList() != nullptr) {
    PyObject* module = func->func_module;
    BorrowedRef<PyCodeObject> top_code{func->func_code};
    BorrowedRef<> top_consts{top_code->co_consts};
    for (BorrowedRef<PyCodeObject> code : findNestedCodes(module, top_consts)) {
      jit_reg_units.emplace(code.getObj());
      auto& jit_code_outer_funcs =
          cinderx::getModuleState()->codeOuterFunctions();
      jit_code_outer_funcs.emplace(code, func);
    }
  }

  return true;
}

PyObject* multithreaded_compile_test(PyObject*, PyObject*) {
  if (!getConfig().multithreaded_compile_test) {
    PyErr_SetString(
        PyExc_NotImplementedError, "multithreaded_compile_test not enabled");
    return nullptr;
  }
  g_compile_workers_attempted = 0;
  g_compile_workers_retries = 0;
  auto& jit_reg_units = cinderx::getModuleState()->registeredCompilationUnits();
  JIT_LOG("(Re)compiling {} units", jit_reg_units.size());
  jitCtx()->clearCache();
  if (!compile_all()) {
    return nullptr;
  }
  JIT_LOG(
      "Took {} ms, compiles attempted: {}, compiles retried: {}",
      g_batch_compilation_time.count(),
      g_compile_workers_attempted,
      g_compile_workers_retries);
  Py_RETURN_NONE;
}

PyObject* is_multithreaded_compile_test_enabled(PyObject*, PyObject*) {
  if (getConfig().multithreaded_compile_test) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

bool deoptFuncImpl(BorrowedRef<PyFunctionObject> func) {
  // There appear to be instances where the runtime is finalizing and goes to
  // destroy the cinderjit module and deopt all compiled functions, only to find
  // that some of the compiled functions have already been zeroed out and
  // possibly deallocated.  In theory this should be covered by funcDestroyed()
  // but somewhere that isn't being triggered.  This is not a good solution but
  // it fixes some shutdown crashes for now.
  if (func->func_module == nullptr && func->func_qualname == nullptr) {
    JIT_CHECK(
        Py_IsFinalizing(),
        "Trying to deopt destroyed function at {} when runtime is not "
        "finalizing",
        reinterpret_cast<void*>(func.get()));
    return false;
  }

  if (!jitCtx()->removeCompiledFunc(func)) {
    return false;
  }
  func->vectorcall = getInterpretedVectorcall(func);
  return true;
}

void uncompile(BorrowedRef<PyFunctionObject> func) {
  deoptFuncImpl(func);
  jitCtx()->forgetCode(func);
}

/*
 * De-optimize a function by setting it to run through the interpreter if it
 * had been previously JIT-compiled.
 *
 * Return true if the function was previously JIT-compiled, false otherwise.
 */
bool deoptFunc(BorrowedRef<PyFunctionObject> func) {
  if (jitCtx() && deoptFuncImpl(func)) {
    jitCtx()->addDeoptedFunc(func);
    return true;
  }
  return false;
}

void disable_jit_impl(bool deopt_all) {
  if (jitCtx() == nullptr) {
    return;
  }

  if (deopt_all) {
    JIT_DLOG(
        "Deopting {} compiled functions", jitCtx()->compiledFuncs().size());
    size_t success = 0;
    for (BorrowedRef<PyFunctionObject> func : jitCtx()->compiledFuncs()) {
      if (deoptFunc(func)) {
        success++;
      } else {
        JIT_DLOG("Failed to deopt compiled function '{}'", funcFullname(func));
      }
    }
    JIT_DLOG("Deopted {} compiled functions", success);
  }

  if (isJitUsable()) {
    getMutableConfig().state = State::kPaused;
    JIT_DLOG("Disabled the JIT");
  }
}

PyObject* disable_jit(PyObject* /* self */, PyObject* args, PyObject* kwargs) {
  int deopt_all = 0;

  const char* keywords[] = {"deopt_all", nullptr};

  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "|p", const_cast<char**>(keywords), &deopt_all)) {
    return nullptr;
  }

  disable_jit_impl(deopt_all);

  Py_RETURN_NONE;
}

bool enable_jit_impl() {
  if (jitCtx() == nullptr) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "Trying to re-enable the JIT but the JIT context is missing");
    return false;
  }
  if (isJitUsable()) {
    return true;
  }

  size_t count = 0;
  for (BorrowedRef<PyFunctionObject> func : jitCtx()->deoptedFuncs()) {
    reoptFunc(func);
    count++;
  }

  getMutableConfig().state = State::kRunning;

  JIT_DLOG("Re-enabled the JIT and re-optimized {} functions", count);

  return true;
}

PyObject* enable_jit(PyObject* /* self */, PyObject* /* arg */) {
  if (!enable_jit_impl()) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

#if PY_VERSION_HEX >= 0x030C0000

// Check if there are any active callback registered through
// sys.monitoring.register_callback()
bool hasRegisteredMonitoringCallbacks() {
  auto is = PyInterpreterState_Get();
  for (int tool_id = 0; tool_id < PY_MONITORING_TOOL_IDS; ++tool_id) {
    // Skip the internal Python tool IDs used by sys.setprofile and
    // sys.settrace as these are internal (users can't call register_callback()
    // with these tool IDs) and their registered callbacks are never cleared
    if (tool_id == PY_MONITORING_SYS_PROFILE_ID ||
        tool_id == PY_MONITORING_SYS_TRACE_ID) {
      continue;
    }
    for (int event_id = 0; event_id < _PY_MONITORING_EVENTS; ++event_id) {
      BorrowedRef<> entry = is->monitoring_callables[tool_id][event_id];
      if (entry != nullptr && !Py_IsNone(entry)) {
        return true;
      }
    }
  }
  return false;
}

// Check if sys.setprofile or sys.settrace have active callbacks registered.
bool hasActiveLegacyTracing() {
  auto is = PyInterpreterState_Get();
  return is->sys_profiling_threads > 0 || is->sys_tracing_threads > 0;
}

bool isInstrumentationActive() {
  return hasRegisteredMonitoringCallbacks() || hasActiveLegacyTracing();
}

// Returns false only if enable_jit_impl() fails (with Python exception set).
bool toggleJitBasedOnInstrumentationState() {
  if (isInstrumentationActive()) {
    disable_jit_impl(true /* deopt_all */);
    return true;
  }
  return enable_jit_impl();
}

// Patched version of sys.monitoring.register_callback().
// Intercepts callback registration/deregistration to disable/enable the JIT
// This is to handle debuggers/profilers attaching or detaching.
PyObject* patched_sys_monitoring_register_callback(
    PyObject* /* self */,
    PyObject* const* args,
    Py_ssize_t nargs) {
  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> original =
      mod_state->getOriginalSysMonitoringRegisterCallback();
  JIT_CHECK(
      original != nullptr,
      "Expecting to have sys.monitoring.register_callback already saved");

  // Run the original function first
  PyObject* result =
      PyObject_Vectorcall(original, args, nargs, nullptr /* kwnames */);
  if (result == nullptr) {
    return nullptr;
  }

  if (!toggleJitBasedOnInstrumentationState()) {
    Py_DECREF(result);
    return nullptr;
  }

  return result;
}

// Patched version of sys.setprofile().
// Intercepts profiler registration/deregistration to disable/enable the JIT.
PyObject* patched_sys_setprofile(
    PyObject* /* self */,
    PyObject* const* args,
    Py_ssize_t nargs) {
  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> original = mod_state->getOriginalSysSetProfile();
  JIT_CHECK(
      original != nullptr, "Expecting to have sys.setprofile already saved");

  PyObject* result =
      PyObject_Vectorcall(original, args, nargs, nullptr /* kwnames */);
  if (result == nullptr) {
    return nullptr;
  }

  if (!toggleJitBasedOnInstrumentationState()) {
    Py_DECREF(result);
    return nullptr;
  }

  return result;
}

// Patched version of sys.settrace().
// Intercepts tracer registration/deregistration to disable/enable the JIT.
PyObject* patched_sys_settrace(
    PyObject* /* self */,
    PyObject* const* args,
    Py_ssize_t nargs) {
  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> original = mod_state->getOriginalSysSetTrace();
  JIT_CHECK(
      original != nullptr, "Expecting to have sys.settrace already saved");

  PyObject* result =
      PyObject_Vectorcall(original, args, nargs, nullptr /* kwnames */);
  if (result == nullptr) {
    return nullptr;
  }

  if (!toggleJitBasedOnInstrumentationState()) {
    Py_DECREF(result);
    return nullptr;
  }

  return result;
}

#endif // PY_VERSION_HEX >= 0x030C0000

void compile_after_n_calls_impl(uint32_t calls) {
  getMutableConfig().compile_after_n_calls = calls;

  // Schedule all pre-existing functions for compilation.
  walkFunctionObjects(
      [](BorrowedRef<PyFunctionObject> func) { scheduleJitCompile(func); });

  JIT_DLOG("Configuring JIT to compile functions after {} calls", calls);
}

PyObject* compile_after_n_calls(PyObject* /* self */, PyObject* arg) {
  Py_ssize_t calls = -1;
  if (!PyArg_Parse(arg, "n:compile_after_n_calls", &calls)) {
    return nullptr;
  }
  if (calls < 0 || calls > std::numeric_limits<uint32_t>::max()) {
    PyErr_Format(
        PyExc_ValueError,
        "Cannot configure JIT to compile functions after '%zd' calls",
        calls);
    return nullptr;
  }

  compile_after_n_calls_impl(calls);

  Py_RETURN_NONE;
}

PyObject* auto_jit(PyObject* /* self */, PyObject* /* arg */) {
  // Default value that works well for most applications.
  compile_after_n_calls_impl(1000);

  Py_RETURN_NONE;
}

PyObject* get_batch_compilation_time_ms(PyObject*, PyObject*) {
  return PyLong_FromLong(g_batch_compilation_time.count());
}

BorrowedRef<PyFunctionObject> get_func_arg(
    const char* method_name,
    BorrowedRef<> arg) {
  if (PyFunction_Check(arg)) {
    return BorrowedRef<PyFunctionObject>{arg};
  }
  PyErr_Format(
      PyExc_TypeError,
      "%s expected a Python function, received '%s' object",
      method_name,
      Py_TYPE(arg)->tp_name);
  return nullptr;
}

PyObject*
precompile_all(PyObject* /* self */, PyObject* args, PyObject* kwargs) {
  if (!isJitUsable()) {
    Py_RETURN_FALSE;
  }

  // Default value of 0 means to read the value from the jit::Config if it
  // exists, otherwise do it all inline on the same thread.
  Py_ssize_t workers = 0;
  const char* keywords[] = {"workers", nullptr};
  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "|n", const_cast<char**>(keywords), &workers)) {
    return nullptr;
  }
  if (workers < 0) {
    PyErr_Format(
        PyExc_ValueError,
        "Cannot call precompile_all with %ld workers",
        workers);
    return nullptr;
  }
  if (workers > 1000) {
    PyErr_Format(
        PyExc_ValueError,
        "Trying to call precompile_all with %ld workers which seems like too "
        "much",
        workers);
    return nullptr;
  }

  if (!compile_all(workers)) {
    return nullptr;
  }

  JIT_DLOG("precompile_all completed in {}", g_batch_compilation_time);
  Py_RETURN_TRUE;
}

PyObject* force_compile(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("force_compile", arg);
  if (func == nullptr) {
    return nullptr;
  }

  if (!isJitUsable() || isJitCompiled(func)) {
    Py_RETURN_FALSE;
  }

  switch (compileFunction(func)) {
    case PYJIT_RESULT_OK:
      Py_RETURN_TRUE;
    case PYJIT_RESULT_CANNOT_SPECIALIZE:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_CANNOT_SPECIALIZE");
      return nullptr;
    case PYJIT_RESULT_NOT_ON_JITLIST:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_NOT_ON_JITLIST");
      return nullptr;
    case PYJIT_RESULT_RETRY:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_RETRY");
      return nullptr;
    case PYJIT_RESULT_UNKNOWN_ERROR:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_UNKNOWN_ERROR");
      return nullptr;
    case PYJIT_NOT_INITIALIZED:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_NOT_INITIALIZED");
      return nullptr;
    case PYJIT_RESULT_NO_PRELOADER:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_RESULT_NO_PRELOADER");
      return nullptr;
    case PYJIT_OVER_MAX_CODE_SIZE:
      PyErr_SetString(PyExc_RuntimeError, "PYJIT_OVER_MAX_CODE_SIZE");
      return nullptr;
    case PYJIT_RESULT_PYTHON_EXCEPTION:
      return nullptr;
  }
  PyErr_SetString(PyExc_RuntimeError, "Unhandled compilation result");
  return nullptr;
}

PyObject* lazy_compile(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("lazy_compile", arg);
  if (func == nullptr) {
    return nullptr;
  }

  if (!isJitUsable() || isJitCompiled(func)) {
    Py_RETURN_FALSE;
  }

  func->vectorcall = forcedJitVectorcall;
  if (!registerFunction(func)) {
    func->vectorcall = getInterpretedVectorcall(func);
    Py_RETURN_FALSE;
  }

  Py_RETURN_TRUE;
}

// Uncompile a function by returning it to its non-jitted version.
PyObject* force_uncompile(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("force_uncompile", arg);
  if (func == nullptr) {
    return nullptr;
  }

  if (!isJitCompiled(func)) {
    Py_RETURN_FALSE;
  }

  // Replace the function entrypoint with the interpreter entrypoint, so that it
  // can properly be called again.
  func->vectorcall = getInterpretedVectorcall(func);

  // "Destroy" the function from the perspective of the JIT, effectively erasing
  // all traces of it from the metadata.
  funcDestroyed(func);
  if (jitCtx() != nullptr) {
    uncompile(func);
  }

  Py_RETURN_TRUE;
}

int aot_func_visitor(PyObject* obj, void* arg) {
  constexpr int kGcVisitContinue = 1;

  auto aot_ctx = reinterpret_cast<AotContext*>(arg);
  if (!PyFunction_Check(obj)) {
    return kGcVisitContinue;
  }

  BorrowedRef<PyFunctionObject> func{obj};
  auto func_state = aot_ctx->lookupFuncState(func);
  if (func_state != nullptr) {
    func->vectorcall = func_state->normalEntry();
  }
  return kGcVisitContinue;
}

PyObject* load_aot_bundle(PyObject* /* self */, PyObject* arg) {
  JIT_CHECK(
      jitCtx() != nullptr,
      "Loading an AOT bundle currently requires the JIT to be enabled");

  if (!PyUnicode_Check(arg)) {
    PyErr_SetString(
        PyExc_ValueError, "load_aot_bundle expects a filename string");
    return nullptr;
  }

  const char* filename = PyUnicode_AsUTF8(arg);

  // TASK(T193992967): Verify these options are what we want.
  auto handle = dlopen(filename, RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    std::string msg = fmt::format(
        "Failed to dlopen() the AOT bundle at {}\n{}", filename, dlerror());
    PyErr_SetString(PyExc_RuntimeError, msg.c_str());
    return nullptr;
  }

  g_aot_ctx.init(handle);

  // TASK(T193608222): It would be great to do something other than mmapping the
  // entire file into memory, especially since we just loaded it in via
  // dlopen().
  MmapFile file;
  try {
    file.open(filename);
  } catch (const std::exception& exn) {
    PyErr_SetString(PyExc_RuntimeError, exn.what());
    return nullptr;
  }

  // Find the function metadata section.
  std::span<const std::byte> note_span;
  try {
    note_span = elf::findSection(file.data(), elf::kFuncNoteSectionName);
  } catch (const std::exception& exn) {
    PyErr_SetString(PyExc_RuntimeError, exn.what());
    return nullptr;
  }
  if (note_span.empty()) {
    PyErr_SetString(
        PyExc_RuntimeError, "Cannot find note section for function metadata");
    return nullptr;
  }

  elf::NoteArray note_array = elf::readNoteSection(note_span);

  // Populate AotContext with data from the note section.
  for (const elf::Note& note : note_array.notes()) {
    g_aot_ctx.registerFunc(note);
  }

  // Now map compiled functions to existing PyFunctionObjects.
  //
  // TASK(T193992967): This is terrible and we should be going the other way,
  // mapping read notes over to function objects.
  PyUnstable_GC_VisitObjects(aot_func_visitor, &g_aot_ctx);

  Py_RETURN_NONE;
}

PyObject* get_compile_after_n_calls(PyObject* /* self */, PyObject*) {
  auto limit = getConfig().compile_after_n_calls;
  if (limit.has_value()) {
    return PyLong_FromLong(*limit);
  }
  Py_RETURN_NONE;
}

PyObject* is_enabled(PyObject* /* self */, PyObject* /* args */) {
  return PyBool_FromLong(isJitUsable());
}

PyObject* count_interpreted_calls(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func =
      get_func_arg("count_interpreted_calls", arg);
  if (func == nullptr) {
    return nullptr;
  }
  BorrowedRef<PyCodeObject> code{func->func_code};
  return PyLong_FromLong(static_cast<long>(countCalls(code)));
}

PyObject* is_jit_compiled(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("is_jit_compiled", arg);
  return func != nullptr ? PyBool_FromLong(isJitCompiled(func)) : nullptr;
}

PyObject* set_max_code_size(PyObject* /* self */, PyObject* arg) {
  Py_ssize_t new_size;
  if (!PyArg_Parse(arg, "n:set_max_code_size", &new_size)) {
    return nullptr;
  }
  if (new_size < 0) {
    PyErr_Format(
        PyExc_ValueError, "max_code_size cannot be negative: %zd", new_size);
    return nullptr;
  }
  getMutableConfig().max_code_size = static_cast<size_t>(new_size);
  Py_RETURN_NONE;
}

PyObject* print_hir(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "JIT is not initialized");
    return nullptr;
  }
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return nullptr;
  }

  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "function is not jit compiled");
    return nullptr;
  }

  compiled_func->printHIR();
  Py_RETURN_NONE;
}

PyObject* disassemble(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "JIT is not initialized");
    return nullptr;
  }
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return nullptr;
  }

  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "function is not jit compiled");
    return nullptr;
  }

  compiled_func->disassemble();
  Py_RETURN_NONE;
}

PyObject* dump_elf(PyObject* /* self */, PyObject* arg) {
  JIT_CHECK(
      jitCtx() != nullptr,
      "JIT context not initialized despite cinderjit module having been "
      "loaded");
  if (!PyUnicode_Check(arg)) {
    PyErr_SetString(PyExc_ValueError, "dump_elf expects a filename string");
    return nullptr;
  }

  Py_ssize_t filename_size = 0;
  const char* filename = PyUnicode_AsUTF8AndSize(arg, &filename_size);

  std::vector<elf::CodeEntry> entries;
  for (BorrowedRef<PyFunctionObject> func : jitCtx()->compiledFuncs()) {
    BorrowedRef<PyCodeObject> code{func->func_code};
    CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);

    elf::CodeEntry entry;
    entry.code = code;
    entry.compiled_code = compiled_func->codeBuffer();
    entry.normal_entry =
        reinterpret_cast<void*>(compiled_func->vectorcallEntry());
    entry.static_entry = compiled_func->staticEntry();
    entry.func_name = funcFullname(func);
    if (code->co_filename != nullptr && PyUnicode_Check(code->co_filename)) {
      entry.file_name = unicodeAsString(code->co_filename);
    }
    entry.lineno = code->co_firstlineno;

    entries.emplace_back(std::move(entry));
  }

  std::ofstream out{filename};
  elf::writeEntries(out, entries);

  Py_RETURN_NONE;
}

PyObject* get_jit_list(PyObject* /* self */, PyObject*) {
  if (auto jit_list = cinderx::getModuleState()->jitList()) {
    return jit_list->getList().release();
  }

  Py_RETURN_NONE;
}

// Create a new JIT list if one doesn't exist yet, returning true if a new list
// was made.
bool ensureJitList() {
  if (cinderx::getModuleState()->jitList() != nullptr) {
    return false;
  }
  std::unique_ptr<JITList> jit_list;
  if (getConfig().allow_jit_list_wildcards) {
    jit_list = WildcardJITList::create();
  } else {
    jit_list = JITList::create();
  }
  cinderx::getModuleState()->setJitList(std::move(jit_list));
  return true;
}

void deleteJitList() {
  cinderx::getModuleState()->setJitList(nullptr);
}

PyObject* append_jit_list(PyObject* /* self */, PyObject* arg) {
  if (!PyUnicode_Check(arg)) {
    PyErr_Format(
        PyExc_TypeError,
        "append_jit_list expected a file path string, received '%s' object",
        Py_TYPE(arg)->tp_name);
    return nullptr;
  }

  Py_ssize_t line_len;
  const char* line_str = PyUnicode_AsUTF8AndSize(arg, &line_len);
  if (line_str == nullptr) {
    return nullptr;
  }
  std::string_view line{
      line_str, static_cast<std::string::size_type>(line_len)};

  bool new_list = ensureJitList();

  // Parse in the new line.  If that fails and a new list was created, delete
  // it.
  auto jit_list = cinderx::getModuleState()->jitList();
  if (!jit_list->parseLine(line)) {
    if (new_list) {
      deleteJitList();
    }
    PyErr_Format(
        PyExc_RuntimeError, "Failed to parse new JIT list line %U", arg);
    return nullptr;
  }

  // Reset existing functions to have the JIT vectorcall entrypoint again if
  // they're now on the JIT list.
  walkFunctionObjects([](BorrowedRef<PyFunctionObject> func) {
    auto jit_list = cinderx::getModuleState()->jitList();
    if (jit_list->lookupFunc(func)) {
      scheduleJitCompile(func);
    }
  });

  Py_RETURN_NONE;
}

PyObject* read_jit_list(PyObject* /* self */, PyObject* arg) {
  if (!PyUnicode_Check(arg)) {
    PyErr_Format(
        PyExc_TypeError,
        "read_jit_list expected a file path string, received '%s' object",
        Py_TYPE(arg)->tp_name);
    return nullptr;
  }

  const char* path = PyUnicode_AsUTF8(arg);
  if (path == nullptr) {
    return nullptr;
  }

  // Create a new JIT list if one doesn't exist yet.
  bool new_list = ensureJitList();

  // Parse in the new file.  If that fails and a new list was created, delete
  // it.
  auto jit_list = cinderx::getModuleState()->jitList();
  try {
    jit_list->parseFile(path);
  } catch (const std::exception& exn) {
    if (new_list) {
      deleteJitList();
    }

    PyErr_SetString(PyExc_RuntimeError, exn.what());
    return nullptr;
  }

  // Reset existing functions to have the JIT vectorcall entrypoint again if
  // they're now on the JIT list.
  walkFunctionObjects([](BorrowedRef<PyFunctionObject> func) {
    auto jit_list = cinderx::getModuleState()->jitList();
    if (jit_list->lookupFunc(func)) {
      scheduleJitCompile(func);
    }
  });

  Py_RETURN_NONE;
}

PyObject* get_compiled_functions(PyObject* /* self */, PyObject*) {
  auto funcs = Ref<>::steal(PyList_New(0));
  if (funcs == nullptr) {
    return nullptr;
  }
  for (BorrowedRef<PyFunctionObject> func : jitCtx()->compiledFuncs()) {
    if (PyList_Append(funcs, func) < 0) {
      return nullptr;
    }
  }
  return funcs.release();
}

PyObject* get_compilation_time(PyObject* /* self */, PyObject*) {
  auto time = jitCtx() != nullptr ? jitCtx()->totalCompileTime()
                                  : std::chrono::milliseconds::zero();
  return PyLong_FromLong(time.count());
}

PyObject* get_function_compilation_time(PyObject* /* self */, PyObject* arg) {
  if (arg == nullptr || !PyFunction_Check(arg) || jitCtx() == nullptr) {
    Py_RETURN_NONE;
  }

  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    Py_RETURN_NONE;
  }

  auto compile_time = std::chrono::duration_cast<std::chrono::milliseconds>(
      compiled_func->compileTime());
  return PyLong_FromLong(compile_time.count());
}

PyObject* get_inlined_functions_stats(PyObject* /* self */, PyObject* arg) {
  if (jitCtx() == nullptr || !PyFunction_Check(arg)) {
    Py_RETURN_NONE;
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    Py_RETURN_NONE;
  }

  auto const& stats = compiled_func->inlinedFunctionsStats();
  auto py_stats = Ref<>::steal(PyDict_New());
  if (py_stats == nullptr) {
    Py_RETURN_NONE;
  }
  auto num_inlined_functions =
      Ref<>::steal(PyLong_FromSize_t(stats.num_inlined_functions));
  if (num_inlined_functions == nullptr) {
    Py_RETURN_NONE;
  }
  if (PyDict_SetItemString(
          py_stats, "num_inlined_functions", num_inlined_functions) < 0) {
    Py_RETURN_NONE;
  }
  auto failure_stats = Ref<>::steal(PyDict_New());
  if (failure_stats == nullptr) {
    Py_RETURN_NONE;
  }
  for (const auto& [reason, functions] : stats.failure_stats) {
    auto py_failure_reason =
        Ref<>::steal(PyUnicode_InternFromString(getInlineFailureName(reason)));
    if (py_failure_reason == nullptr) {
      Py_RETURN_NONE;
    }
    auto py_functions_set = Ref<>::steal(PySet_New(nullptr));
    if (py_functions_set == nullptr) {
      Py_RETURN_NONE;
    }
    if (PyDict_SetItem(failure_stats, py_failure_reason, py_functions_set) <
        0) {
      Py_RETURN_NONE;
    }
    for (const auto& function : functions) {
      auto py_function = Ref<>::steal(PyUnicode_FromString(function.c_str()));
      if (PySet_Add(py_functions_set, py_function) < 0) {
        Py_RETURN_NONE;
      }
    }
  }
  if (PyDict_SetItemString(py_stats, "failure_stats", failure_stats) < 0) {
    Py_RETURN_NONE;
  }
  return py_stats.release();
}

PyObject* get_num_inlined_functions(PyObject* /* self */, PyObject* arg) {
  if (jitCtx() == nullptr || !PyFunction_Check(arg)) {
    return PyLong_FromLong(0);
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  int size = compiled_func != nullptr
      ? compiled_func->inlinedFunctionsStats().num_inlined_functions
      : 0;
  return PyLong_FromLong(size);
}

PyObject* get_function_hir_opcode_counts(PyObject* /* self */, PyObject* arg) {
  if (jitCtx() == nullptr || !PyFunction_Check(arg)) {
    Py_RETURN_NONE;
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  if (compiled_func == nullptr) {
    Py_RETURN_NONE;
  }

  const hir::OpcodeCounts& counts = compiled_func->hirOpcodeCounts();
  Ref<> dict = Ref<>::steal(PyDict_New());
  if (dict == nullptr) {
    return nullptr;
  }
#define HIR_OP(OPNAME)                                                 \
  {                                                                    \
    const size_t idx = static_cast<size_t>(hir::Opcode::k##OPNAME);    \
    if (int count = counts.at(idx); count != 0) {                      \
      Ref<> count_obj = Ref<>::steal(PyLong_FromLong(count));          \
      if (count_obj == nullptr) {                                      \
        return nullptr;                                                \
      }                                                                \
      auto opname = Ref<>::steal(PyUnicode_InternFromString(#OPNAME)); \
      if (opname == nullptr) {                                         \
        return nullptr;                                                \
      }                                                                \
      if (PyDict_SetItem(dict, opname, count_obj) < 0) {               \
        return nullptr;                                                \
      }                                                                \
    }                                                                  \
  }
  FOREACH_OPCODE(HIR_OP)
#undef HIR_OP
  return dict.release();
}

PyObject* mlock_profiler_dependencies(PyObject* /* self */, PyObject*) {
  if (jitCtx() == nullptr) {
    Py_RETURN_NONE;
  }
  Runtime::get()->mlockProfilerDependencies();
  Py_RETURN_NONE;
}

PyObject* page_in_profiler_dependencies(PyObject*, PyObject*) {
  Ref<> qualnames = Runtime::get()->pageInProfilerDependencies();
  return qualnames.release();
}

// Simple wrapper functions to turn nullptr or -1 return values from C-API
// functions into a thrown exception. Meant for repetitive runs of C-API calls
// and not intended for use in public APIs.
class CAPIError : public std::exception {};

PyObject* check(PyObject* obj) {
  if (obj == nullptr) {
    throw CAPIError();
  }
  return obj;
}

int check(int ret) {
  if (ret < 0) {
    throw CAPIError();
  }
  return ret;
}

Ref<> make_deopt_stats() {
  DEFINE_STATIC_STRING(count);
  DEFINE_STATIC_STRING(description);
  DEFINE_STATIC_STRING(filename);
  DEFINE_STATIC_STRING(func_qualname);
  DEFINE_STATIC_STRING(guilty_type);
  DEFINE_STATIC_STRING(lineno);
  DEFINE_STATIC_STRING(normal);
  DEFINE_STATIC_STRING(int);
  DEFINE_STATIC_STRING(reason);

  auto runtime = Runtime::get();
  auto stats = Ref<>::steal(check(PyList_New(0)));

  for (auto& pair : jitCtx()->compiledCodes()) {
    const CompiledFunction& compiled_func = *pair.second;
    const CodeRuntime* code_runtime = compiled_func.runtime();

    auto const& deopt_metadatas = code_runtime->deoptMetadatas();
    for (size_t deopt_idx = 0; deopt_idx < deopt_metadatas.size();
         ++deopt_idx) {
      const DeoptMetadata& meta = deopt_metadatas[deopt_idx];

      auto stat_ptr = runtime->deoptStat(code_runtime, deopt_idx);
      if (stat_ptr == nullptr) {
        continue;
      }
      const DeoptStat& stat = *stat_ptr;

      const DeoptFrameMetadata& frame_meta = meta.innermostFrame();
      BorrowedRef<PyCodeObject> code = frame_meta.code;

      auto func_qualname = code->co_qualname;
      BCOffset line_offset = frame_meta.cause_instr_idx;
      int lineno_raw = code->co_linetable != nullptr
          ? PyCode_Addr2Line(code, line_offset.value())
          : -1;
      auto lineno = Ref<>::steal(check(PyLong_FromLong(lineno_raw)));
      auto reason = Ref<>::steal(
          check(PyUnicode_FromString(deoptReasonName(meta.reason))));
      auto description = Ref<>::steal(check(PyUnicode_FromString(meta.descr)));

      // Helper to create an event dict with a given count value.
      auto append_event = [&](size_t count_raw, const char* type_name) {
        auto event = Ref<>::steal(check(PyDict_New()));
        auto normals = Ref<>::steal(check(PyDict_New()));
        auto ints = Ref<>::steal(check(PyDict_New()));

        check(PyDict_SetItem(event, s_normal, normals));
        check(PyDict_SetItem(event, s_int, ints));
        check(PyDict_SetItem(normals, s_func_qualname, func_qualname));
        check(PyDict_SetItem(normals, s_filename, code->co_filename));
        check(PyDict_SetItem(ints, s_lineno, lineno));
        check(PyDict_SetItem(normals, s_reason, reason));
        check(PyDict_SetItem(normals, s_description, description));

        auto count = Ref<>::steal(check(PyLong_FromSize_t(count_raw)));
        check(PyDict_SetItem(ints, s_count, count));
        auto type_str =
            Ref<>::steal(check(PyUnicode_InternFromString(type_name)));
        check(PyDict_SetItem(normals, s_guilty_type, type_str) < 0);
        check(PyList_Append(stats, event));
      };

      // For deopts with type profiles, add a copy of the dict with counts for
      // each type, including "other".
      if (!stat.types.empty()) {
        for (size_t i = 0;
             i < stat.types.size && stat.types.types[i] != nullptr;
             ++i) {
          append_event(
              stat.types.counts[i], typeFullname(stat.types.types[i]).c_str());
        }
        if (stat.types.other > 0) {
          append_event(stat.types.other, "<other>");
        }
      } else {
        append_event(stat.count, "<none>");
      }
    }
  }

  runtime->clearDeoptStats();

  return stats;
}

PyObject* get_and_clear_runtime_stats(PyObject* /* self */, PyObject*) {
  auto stats = Ref<>::steal(PyDict_New());
  if (stats == nullptr) {
    return nullptr;
  }

  try {
    Ref<> deopt_stats = make_deopt_stats();
    check(PyDict_SetItemString(stats, "deopt", deopt_stats));
  } catch (const CAPIError&) {
    return nullptr;
  }

  return stats.release();
}

PyObject* clear_runtime_stats(PyObject* /* self */, PyObject*) {
  Runtime::get()->clearDeoptStats();
  Py_RETURN_NONE;
}

PyObject* get_compiled_size(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->codeSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* get_compiled_stack_size(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->stackSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* get_compiled_spill_stack_size(PyObject* /* self */, PyObject* func) {
  if (jitCtx() == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jitCtx()->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->spillStackSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* jit_frame_mode(PyObject* /* self */, PyObject*) {
  return PyLong_FromLong(static_cast<int>(getConfig().frame_mode));
}

PyObject* get_and_clear_inline_cache_stats(PyObject* /* self */, PyObject*) {
  auto stats = Ref<>::steal(PyDict_New());
  if (stats == nullptr) {
    return nullptr;
  }

  auto make_inline_cache_stats = [](PyObject* stats, CacheStats& cache_stats) {
    auto result = Ref<>::steal(check(PyDict_New()));
    check(PyDict_SetItemString(
        result,
        "filename",
        PyUnicode_InternFromString(cache_stats.filename.c_str())));
    check(PyDict_SetItemString(
        result,
        "method",
        PyUnicode_InternFromString(cache_stats.method_name.c_str())));
    auto cache_misses_dict = Ref<>::steal(check(PyDict_New()));
    check(PyDict_SetItemString(result, "cache_misses", cache_misses_dict));
    for (auto& [key, miss] : cache_stats.misses) {
      auto py_key = Ref<>::steal(check(PyUnicode_FromString(key.c_str())));
      auto miss_dict = Ref<>::steal(check(PyDict_New()));
      check(PyDict_SetItemString(
          miss_dict, "count", PyLong_FromLong(miss.count)));
      check(PyDict_SetItemString(
          miss_dict,
          "reason",
          PyUnicode_InternFromString(
              std::string(cacheMissReason(miss.reason)).c_str())));

      check(PyDict_SetItem(cache_misses_dict, py_key, miss_dict));
    }
    check(PyList_Append(stats, result));
  };
  auto load_method_stats = Ref<>::steal(check(PyList_New(0)));
  check(PyDict_SetItemString(stats, "load_method_stats", load_method_stats));
  for (auto& cache_stats : Runtime::get()->getAndClearLoadMethodCacheStats()) {
    make_inline_cache_stats(load_method_stats, cache_stats);
  }

  auto load_type_method_stats = Ref<>::steal(check(PyList_New(0)));
  check(PyDict_SetItemString(
      stats, "load_type_method_stats", load_type_method_stats));
  for (auto& cache_stats :
       Runtime::get()->getAndClearLoadTypeMethodCacheStats()) {
    make_inline_cache_stats(load_type_method_stats, cache_stats);
  }

  return stats.release();
}

PyObject* jit_suppress(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("jit_suppress", arg);
  if (func == nullptr) {
    return nullptr;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  code->co_flags |= CI_CO_SUPPRESS_JIT;

  Py_INCREF(arg);
  return arg;
}

// Unsuppress a function that was suppressed by jit_suppress. This will allow it
// to be compiled in the future.
PyObject* jit_unsuppress(PyObject* /* self */, PyObject* arg) {
  BorrowedRef<PyFunctionObject> func = get_func_arg("jit_unsuppress", arg);
  if (func == nullptr) {
    return nullptr;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  code->co_flags &= ~CI_CO_SUPPRESS_JIT;

  Py_INCREF(arg);
  return arg;
}

PyObject* get_allocator_stats(PyObject*, PyObject*) {
  auto base_allocator = cinderx::getModuleState()->codeAllocator();
  if (base_allocator == nullptr) {
    Py_RETURN_NONE;
  }

  auto stats = Ref<>::steal(PyDict_New());
  if (stats == nullptr) {
    return nullptr;
  }

  auto used_bytes = Ref<>::steal(PyLong_FromLong(base_allocator->usedBytes()));
  if (used_bytes == nullptr ||
      PyDict_SetItemString(stats, "used_bytes", used_bytes) < 0) {
    return nullptr;
  }
  auto max_bytes = Ref<>::steal(PyLong_FromLong(getConfig().max_code_size));
  if (max_bytes == nullptr ||
      PyDict_SetItemString(stats, "max_bytes", max_bytes) < 0) {
    return nullptr;
  }

  auto allocator = dynamic_cast<CodeAllocatorCinder*>(base_allocator);
  if (allocator == nullptr) {
    return stats.release();
  }

  auto lost_bytes = Ref<>::steal(PyLong_FromLong(allocator->lostBytes()));
  if (lost_bytes == nullptr ||
      PyDict_SetItemString(stats, "lost_bytes", lost_bytes) < 0) {
    return nullptr;
  }
  auto fragmented_allocs =
      Ref<>::steal(PyLong_FromLong(allocator->fragmentedAllocs()));
  if (fragmented_allocs == nullptr ||
      PyDict_SetItemString(stats, "fragmented_allocs", fragmented_allocs) < 0) {
    return nullptr;
  }
  auto huge_allocs = Ref<>::steal(PyLong_FromLong(allocator->hugeAllocs()));
  if (huge_allocs == nullptr ||
      PyDict_SetItemString(stats, "huge_allocs", huge_allocs) < 0) {
    return nullptr;
  }
  return stats.release();
}

PyObject* is_hir_inliner_enabled(PyObject* /* self */, PyObject*) {
  if (getConfig().hir_opts.inliner) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyObject* is_inline_cache_stats_collection_enabled(
    PyObject* /* self */,
    PyObject* /* arg */) {
  return PyBool_FromLong(getConfig().collect_attr_cache_stats);
}

PyObject* enable_hir_inliner(PyObject* /* self */, PyObject*) {
  getMutableConfig().hir_opts.inliner = true;
  Py_RETURN_NONE;
}

PyObject* disable_hir_inliner(PyObject* /* self */, PyObject*) {
  getMutableConfig().hir_opts.inliner = false;
  Py_RETURN_NONE;
}

// Enable the emit_type_annotation_guards configuration option.
PyObject* enable_emit_type_annotation_guards(PyObject* /* self */, PyObject*) {
  getMutableConfig().emit_type_annotation_guards = true;
  Py_RETURN_NONE;
}

// Disable the emit_type_annotation_guards configuration option.
PyObject* disable_emit_type_annotation_guards(PyObject* /* self */, PyObject*) {
  getMutableConfig().emit_type_annotation_guards = false;
  Py_RETURN_NONE;
}

PyObject* enable_specialized_opcodes(PyObject* /* self */, PyObject*) {
  getMutableConfig().specialized_opcodes = true;
  Py_RETURN_NONE;
}

PyObject* disable_specialized_opcodes(PyObject* /* self */, PyObject*) {
  getMutableConfig().specialized_opcodes = false;
  Py_RETURN_NONE;
}

// If the given generator-like object is a suspended JIT generator, deopt it
// and return 1. Otherwise, return 0.
int deopt_gen_impl(PyGenObject* gen) {
#if PY_VERSION_HEX >= 0x030C0000
  // deopt_jit_gen optimistically succeeds when the generator isn't a JIT
  // generator.
  return JitGenObject::cast(gen) != nullptr && deopt_jit_gen(gen);
#else
  GenDataFooter* footer = genDataFooter(gen);
  if (footer == nullptr || Ci_GenIsCompleted(gen)) {
    return 0;
  }
  JIT_CHECK(!Ci_GenIsExecuting(gen), "Trying to deopt a running generator");
  JIT_CHECK(
      footer->yieldPoint != nullptr,
      "Suspended JIT generator has nullptr yieldPoint");
  const DeoptMetadata& deopt_meta =
      footer->code_rt->getDeoptMetadata(footer->yieldPoint->deoptIdx());
  JIT_CHECK(
      deopt_meta.frame_meta.size() == 1,
      "Generators with inlined calls are not supported (T109706798)");

  _PyJIT_GenMaterializeFrame(gen);
  _PyShadowFrame_SetOwner(&gen->gi_shadow_frame, PYSF_INTERP);
  reifyGeneratorFrame(
      gen->gi_frame, deopt_meta, deopt_meta.outermostFrame(), footer);
  gen->gi_frame->f_state = FRAME_SUSPENDED;
  releaseRefs(deopt_meta, footer);
  jitgen_data_free(gen);
  return 1;
#endif
}

PyObject* deopt_gen(PyObject*, PyObject* op) {
  if (!PyGen_Check(op) && !PyCoro_CheckExact(op) &&
      !PyAsyncGen_CheckExact(op) && !JitGen_CheckAny(op)) {
    PyErr_Format(
        PyExc_TypeError,
        "Exected generator-like object, got %.200s",
        Py_TYPE(op)->tp_name);
    return nullptr;
  }
  auto gen = reinterpret_cast<PyGenObject*>(op);
  if (
#if PY_VERSION_HEX < 0x030C0000
      gen->gi_frame && _PyFrame_IsExecuting(gen->gi_frame)
#else
      gen->gi_frame_state == FRAME_EXECUTING
#endif
  ) {
    PyErr_SetString(PyExc_RuntimeError, "generator is executing");
    return nullptr;
  }
  if (deopt_gen_impl(gen)) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

int deopt_gen_visitor(PyObject* obj, void*) {
  if (PyGen_Check(obj) || PyCoro_CheckExact(obj) ||
      PyAsyncGen_CheckExact(obj) || JitGen_CheckAny(obj)) {
    deopt_gen_impl(reinterpret_cast<PyGenObject*>(obj));
  }
  return 1;
}

PyObject* after_fork_child(PyObject*, PyObject*) {
  perf::afterForkChild();
  Py_RETURN_NONE;
}

// Patch sys.monitoring.register_callback to intercept debugger/profiler
// attachment.
void patchSysMonitoringFunctions(PyObject* cinderjit_module) {
#if PY_VERSION_HEX >= 0x030C0000

  BorrowedRef<> monitoring = PySys_GetObject("monitoring");
  if (monitoring == nullptr) {
    JIT_DLOG("sys.monitoring not found, skipping JIT monitoring integration");
    return;
  }

  Ref<> original =
      Ref<>::steal(PyObject_GetAttrString(monitoring, "register_callback"));
  if (original == nullptr) {
    PyErr_Clear();
    JIT_DLOG(
        "sys.monitoring.register_callback not found, skipping JIT monitoring "
        "integration");
    return;
  }

  auto mod_state = cinderx::getModuleState();
  mod_state->setOriginalSysMonitoringRegisterCallback(original);

  Ref<> patched_func = Ref<>::steal(PyObject_GetAttrString(
      cinderjit_module, "patched_sys_monitoring_register_callback"));
  if (patched_func == nullptr) {
    JIT_LOG(
        "Failed to get patched_sys_monitoring_register_callback from cinderjit "
        "module");
    PyErr_Clear();
    return;
  }

  if (PyObject_SetAttrString(monitoring, "register_callback", patched_func) <
      0) {
    JIT_LOG("Failed to patch sys.monitoring.register_callback");
    PyErr_Clear();
    return;
  }

  JIT_DLOG("Successfully patched sys.monitoring.register_callback");

#endif // PY_VERSION_HEX >= 0x030C0000
}

// Patch sys.setprofile and sys.settrace to intercept profiler/debugger
// attachment.
void patchSysSetProfileAndSetTrace(PyObject* cinderjit_module) {
#if PY_VERSION_HEX >= 0x030C0000

  Ref<> sys = Ref<>::steal(PyImport_ImportModule("sys"));
  if (sys == nullptr) {
    PyErr_Clear();
    JIT_DLOG("sys module not found, skipping sys.setprofile/settrace patching");
    return;
  }

  auto mod_state = cinderx::getModuleState();

  auto patchSysFunc =
      [&](const char* attr_name,
          const char* patched_attr_name,
          const std::function<void(BorrowedRef<>)>& storeOriginal) {
        Ref<> original = Ref<>::steal(PyObject_GetAttrString(sys, attr_name));
        if (original == nullptr) {
          JIT_DLOG("sys.{} not found, skipping patching", attr_name);
          PyErr_Clear();
          return;
        }
        storeOriginal(original);

        Ref<> patched = Ref<>::steal(
            PyObject_GetAttrString(cinderjit_module, patched_attr_name));
        if (patched == nullptr) {
          JIT_LOG("Failed to get {} from cinderjit module", patched_attr_name);
          PyErr_Clear();
          return;
        }
        if (PyObject_SetAttrString(sys, attr_name, patched) < 0) {
          JIT_LOG("Failed to patch sys.{}", attr_name);
          PyErr_Clear();
        } else {
          JIT_DLOG("Successfully patched sys.{}", attr_name);
        }
      };

  patchSysFunc("setprofile", "patched_sys_setprofile", [&](BorrowedRef<> func) {
    mod_state->setOriginalSysSetProfile(func);
  });
  patchSysFunc("settrace", "patched_sys_settrace", [&](BorrowedRef<> func) {
    mod_state->setOriginalSysSetTrace(func);
  });

#endif // PY_VERSION_HEX >= 0x030C0000
}

void restoreSysMonitoringRegisterCallback() {
#if PY_VERSION_HEX >= 0x030C0000

  auto mod_state = cinderx::getModuleState();
  BorrowedRef<> original =
      mod_state->getOriginalSysMonitoringRegisterCallback();
  if (original == nullptr) {
    return;
  }

  BorrowedRef<> monitoring = PySys_GetObject("monitoring");
  if (monitoring == nullptr) {
    return;
  }

  if (PyObject_SetAttrString(monitoring, "register_callback", original) < 0) {
    PyErr_Clear();
  }
#endif // PY_VERSION_HEX >= 0x030C0000
}

void restoreSysSetProfileAndSetTrace() {
#if PY_VERSION_HEX >= 0x030C0000

  auto mod_state = cinderx::getModuleState();

  if (BorrowedRef<> original_setprofile =
          mod_state->getOriginalSysSetProfile()) {
    if (PySys_SetObject("setprofile", original_setprofile) < 0) {
      PyErr_Clear();
    }
  }

  if (BorrowedRef<> original_settrace = mod_state->getOriginalSysSetTrace()) {
    if (PySys_SetObject("settrace", original_settrace) < 0) {
      PyErr_Clear();
    }
  }

#endif // PY_VERSION_HEX >= 0x030C0000
}

PyMethodDef jit_methods[] = {
    {"disable",
     reinterpret_cast<PyCFunction>(disable_jit),
     METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR(
         "Compile all functions that are pending compilation and then "
         "disable the JIT.")},
    {"enable",
     enable_jit,
     METH_NOARGS,
     PyDoc_STR(
         "Re-enable the JIT and re-attach compiled onto previously "
         "JIT-compiled functions.")},
#if PY_VERSION_HEX >= 0x030C0000
    {"patched_sys_monitoring_register_callback",
     _PyCFunction_CAST(patched_sys_monitoring_register_callback),
     METH_FASTCALL,
     PyDoc_STR(
         "Patched version of sys.monitoring.register_callback that "
         "disables/enables the JIT when debuggers/profilers attach/detach.")},
    {"patched_sys_setprofile",
     _PyCFunction_CAST(patched_sys_setprofile),
     METH_FASTCALL,
     PyDoc_STR(
         "Patched version of sys.setprofile that "
         "disables/enables the JIT when profilers attach/detach.")},
    {"patched_sys_settrace",
     _PyCFunction_CAST(patched_sys_settrace),
     METH_FASTCALL,
     PyDoc_STR(
         "Patched version of sys.settrace that "
         "disables/enables the JIT when debuggers/profilers attach/detach.")},
#endif
    {"auto",
     auto_jit,
     METH_NOARGS,
     PyDoc_STR(
         "Configure the JIT to automatically compile functions, using "
         "default settings")},
    {"compile_after_n_calls",
     compile_after_n_calls,
     METH_O,
     PyDoc_STR(
         "Configure the JIT to automatically compile functions after "
         "they are called a set number of times.")},
    {"disassemble", disassemble, METH_O, "Disassemble JIT compiled functions."},
    {"dump_elf",
     dump_elf,
     METH_O,
     PyDoc_STR(
         "Write out all generated code into an ELF file, whose filepath "
         "is passed as the first argument. This is currently intended for "
         "debugging purposes.")},
    {"load_aot_bundle",
     load_aot_bundle,
     METH_O,
     PyDoc_STR(
         "Load a bundle of ahead-of-time generated code from an ELF "
         "file, whose filepath is passed as the first argument. Note: "
         "This does not actually work yet, it's being used for debugging "
         "purposes.")},
    {"get_compile_after_n_calls",
     get_compile_after_n_calls,
     METH_NOARGS,
     PyDoc_STR(
         "Get the current number of calls needed before a function is "
         "automatically compiled.")},
    {"is_enabled",
     is_enabled,
     METH_NOARGS,
     PyDoc_STR("Check whether the JIT is enabled and usable")},
    {"count_interpreted_calls",
     count_interpreted_calls,
     METH_O,
     PyDoc_STR(
         "Get the number of times a function has been executed in the "
         "interpreter since cinderx has been initialized")},
    {"is_jit_compiled",
     is_jit_compiled,
     METH_O,
     PyDoc_STR("Check if a function is jit compiled.")},
    {"set_max_code_size",
     set_max_code_size,
     METH_O,
     PyDoc_STR(
         "Set the maximum amount of memory (in bytes) the JIT is allowed to "
         "write")},
    {"precompile_all",
     reinterpret_cast<PyCFunction>(precompile_all),
     METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR(
         "If the JIT is enabled, compile all functions registered for "
         "future compilation and return True, otherwise return False. "
         "This is not meant for general use, it has the potential to "
         "compile many unneeded functions. Use wisely.")},
    {"force_compile",
     force_compile,
     METH_O,
     PyDoc_STR("Force a function to be JIT compiled if it hasn't yet.")},
    {"force_uncompile",
     force_uncompile,
     METH_O,
     PyDoc_STR("Uncompile a function that has been JIT compiled.")},
    {"lazy_compile",
     lazy_compile,
     METH_O,
     PyDoc_STR("Set a function to be JIT compiled the first time it is run.")},
    {"jit_frame_mode",
     jit_frame_mode,
     METH_NOARGS,
     PyDoc_STR(
         "Get JIT frame mode (0 = normal frames, 1 = no frames, 2 = "
         "shadow frames).")},
    {"get_jit_list",
     get_jit_list,
     METH_NOARGS,
     PyDoc_STR("Get the list of functions to JIT compile.")},
    {"append_jit_list",
     append_jit_list,
     METH_O,
     PyDoc_STR("Parse a JIT-list line and append it.")},
    {"read_jit_list",
     read_jit_list,
     METH_O,
     PyDoc_STR("Read a JIT list file and apply it.")},
    {"print_hir",
     print_hir,
     METH_O,
     PyDoc_STR("Print the HIR for a jitted function to stdout.")},
    {"get_compiled_functions",
     get_compiled_functions,
     METH_NOARGS,
     PyDoc_STR("Return a list of functions that are currently JIT-compiled.")},
    {"get_compilation_time",
     get_compilation_time,
     METH_NOARGS,
     PyDoc_STR(
         "Return the total time used for JIT compiling functions in "
         "milliseconds.")},
    {"get_function_compilation_time",
     get_function_compilation_time,
     METH_O,
     PyDoc_STR(
         "Return the time used for JIT compiling a given function in "
         "milliseconds.")},
    {"get_and_clear_runtime_stats",
     get_and_clear_runtime_stats,
     METH_NOARGS,
     PyDoc_STR(
         "Returns information about the runtime behavior of JIT-compiled "
         "code.")},
    {"clear_runtime_stats",
     clear_runtime_stats,
     METH_NOARGS,
     PyDoc_STR(
         "Clears runtime stats about JIT-compiled code without returning "
         "a value.")},
    {"get_and_clear_inline_cache_stats",
     get_and_clear_inline_cache_stats,
     METH_NOARGS,
     PyDoc_STR(
         "Returns and clears information about the runtime inline cache stats "
         "behavior of JIT-compiled code. Stats will only be collected with X "
         "flag jit-enable-inline-cache-stats-collection.")},
    {"is_inline_cache_stats_collection_enabled",
     is_inline_cache_stats_collection_enabled,
     METH_NOARGS,
     PyDoc_STR(
         "Return True if jit-enable-inline-cache-stats-collection is on "
         "and False otherwise.")},
    {"get_compiled_size",
     get_compiled_size,
     METH_O,
     PyDoc_STR("Return code size in bytes for a JIT-compiled function.")},
    {"get_compiled_stack_size",
     get_compiled_stack_size,
     METH_O,
     PyDoc_STR("Return stack size in bytes for a JIT-compiled function.")},
    {"get_compiled_spill_stack_size",
     get_compiled_spill_stack_size,
     METH_O,
     PyDoc_STR(
         "Return stack size in bytes used for register spills for a "
         "JIT-compiled function.")},
    {"jit_suppress",
     jit_suppress,
     METH_O,
     PyDoc_STR("Decorator to prevent the JIT from running on a function.")},
    {"jit_unsuppress",
     jit_unsuppress,
     METH_O,
     PyDoc_STR("Decorator to allow the JIT to run on a function.")},
    {"multithreaded_compile_test",
     multithreaded_compile_test,
     METH_NOARGS,
     PyDoc_STR(
         "Force multi-threaded recompile of still existing JIT functions "
         "for testing.")},
    {"is_multithreaded_compile_test_enabled",
     is_multithreaded_compile_test_enabled,
     METH_NOARGS,
     PyDoc_STR("Return True if multithreaded_compile_test mode is enabled.")},
    {"get_batch_compilation_time_ms",
     get_batch_compilation_time_ms,
     METH_NOARGS,
     PyDoc_STR(
         "Return the number of milliseconds spent in batch compilation "
         "the last time precompile_all() was called.")},
    {"get_allocator_stats",
     get_allocator_stats,
     METH_NOARGS,
     PyDoc_STR("Return stats from the code allocator as a dictionary.")},
    {"is_hir_inliner_enabled",
     is_hir_inliner_enabled,
     METH_NOARGS,
     PyDoc_STR(
         "Return True if the HIR inliner is enabled and False otherwise.")},
    {"enable_hir_inliner",
     enable_hir_inliner,
     METH_NOARGS,
     PyDoc_STR("Enable the HIR inliner.")},
    {"disable_hir_inliner",
     disable_hir_inliner,
     METH_NOARGS,
     PyDoc_STR("Disable the HIR inliner.")},
    {"enable_emit_type_annotation_guards",
     enable_emit_type_annotation_guards,
     METH_NOARGS,
     PyDoc_STR("Enable the emit_type_annotation_guards configuration option.")},
    {"disable_emit_type_annotation_guards",
     disable_emit_type_annotation_guards,
     METH_NOARGS,
     PyDoc_STR(
         "Disable the emit_type_annotation_guards configuration option.")},
    {"enable_specialized_opcodes",
     enable_specialized_opcodes,
     METH_NOARGS,
     PyDoc_STR("Enable compiling specialized opcodes.")},
    {"disable_specialized_opcodes",
     disable_specialized_opcodes,
     METH_NOARGS,
     PyDoc_STR("Disable compiling specialized opcodes.")},
    {"get_inlined_functions_stats",
     get_inlined_functions_stats,
     METH_O,
     PyDoc_STR(
         "Return a dict containing function inlining stats with the the "
         "following structure: {'num_inlined_functions' => int, "
         "'failure_stats' => { "
         "failure_reason => set of function names}} ).")},
    {"get_num_inlined_functions",
     get_num_inlined_functions,
     METH_O,
     PyDoc_STR("Return the number of inline sites in this function.")},
    {"get_function_hir_opcode_counts",
     get_function_hir_opcode_counts,
     METH_O,
     PyDoc_STR(
         "Return a map from HIR opcode name to the count of that opcode in the "
         "JIT-compiled version of this function.")},
    {"mlock_profiler_dependencies",
     mlock_profiler_dependencies,
     METH_NOARGS,
     PyDoc_STR("Keep profiler dependencies paged in.")},
    {"page_in_profiler_dependencies",
     page_in_profiler_dependencies,
     METH_NOARGS,
     PyDoc_STR("Read the memory needed by ebpf-based profilers.")},
    {"after_fork_child",
     after_fork_child,
     METH_NOARGS,
     PyDoc_STR("Callback to be invoked by the runtime after fork().")},
    {"_deopt_gen",
     deopt_gen,
     METH_O,
     PyDoc_STR(
         "Argument must be a suspended generator, coroutine, or async "
         "generator. If it is a JIT generator, deopt it, so it will resume in "
         "the interpreter the next time it executes, and return True. "
         "Otherwise, return False. Intended only for use in tests.")},
    {nullptr, nullptr, 0, nullptr},
};

int jit_exec(PyObject*) {
  return 0;
}

PyModuleDef_Slot jit_slots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(jit_exec)},
    {}};

PyModuleDef jit_module = {
    PyModuleDef_HEAD_INIT,
    "cinderjit", /* m_name */
    PyDoc_STR(
        "Control the Cinder JIT compiler. Only available when the JIT "
        "has been enabled."),
    0, /* m_size */
    jit_methods, /* m_methods */
    jit_slots, /* m_slots */
    nullptr, /* m_traverse */
    nullptr, /* m_clear */
    nullptr, /* m_free */
};

// Preload a function and its dependencies, then compile them all.
//
// Failing to compile a dependent function is a soft failure, and is ignored.
_PyJIT_Result compile_func(BorrowedRef<PyFunctionObject> func) {
  // isolate preloaders state since batch preloading might trigger a call to a
  // jitable function, resulting in a single-function compile
  hir::IsolatedPreloaders ip;

  // Collect a list of functions to compile.  If it's empty then there must have
  // been a Python error during preloading.
  std::vector<BorrowedRef<PyFunctionObject>> targets = preloadFuncAndDeps(func);
  if (targets.empty()) {
    JIT_CHECK(
        PyErr_Occurred(), "Expect a Python exception when preloading fails");
    return PYJIT_RESULT_PYTHON_EXCEPTION;
  }

  if (targets.size() > 1) {
    JIT_DLOG(
        "Compiling {} along with {} functions it calls",
        funcFullname(func),
        targets.size() - 1);
  }

  // Will return unknown error if none of the targets can find a matching
  // preloader.
  _PyJIT_Result result = PYJIT_RESULT_UNKNOWN_ERROR;

  for (BorrowedRef<PyFunctionObject> target : targets) {
    auto preloader = hir::preloaderManager().find(target);
    if (preloader == nullptr) {
      continue;
    }

    // Don't compile functions that were preloaded purely for inlining.
    bool is_static = preloader->code()->co_flags & CI_CO_STATICALLY_COMPILED;
    if (target != func && !is_static) {
      continue;
    }

    result = compilePreloader(target, *preloader).result;
    JIT_CHECK(
        result != PYJIT_RESULT_PYTHON_EXCEPTION,
        "Raised a Python exception while JIT-compiling function {}, which is "
        "not allowed",
        funcFullname(target));
    JIT_CHECK(
        result != PYJIT_RESULT_NO_PRELOADER,
        "Cannot find a preloader for function {}, despite it just being "
        "preloaded",
        funcFullname(target));

    // If we hit the max code size limit, stop compiling further functions
    if (result == PYJIT_OVER_MAX_CODE_SIZE) {
      break;
    }
  }

  // This is the common case, where the original function is compiled last.
  // Return its compilation result.
  BorrowedRef<PyFunctionObject> last_func = targets.back();
  if (last_func == func) {
    return result;
  }

  // Otherwise the original function was destroyed during preloading, which is
  // rare but can happen with nested functions.  In that case, we're just going
  // to pretend everything went okay.  It doesn't make sense to return the
  // results of any of the other preloaded functions, as the caller never asked
  // for them in the first place.
  return PYJIT_RESULT_OK;
}

// Call posix.register_at_fork(None, None, cinderjit.after_fork_child), if it
// exists. Returns 0 on success or if the module/function doesn't exist, and -1
// on any other errors.
int register_fork_callback(BorrowedRef<> cinderjit_module) {
  auto os_module = Ref<>::steal(
      PyImport_ImportModuleLevel("posix", nullptr, nullptr, nullptr, 0));
  if (os_module == nullptr) {
    PyErr_Clear();
    return 0;
  }
  auto register_at_fork =
      Ref<>::steal(PyObject_GetAttrString(os_module, "register_at_fork"));
  if (register_at_fork == nullptr) {
    PyErr_Clear();
    return 0;
  }
  auto callback = Ref<>::steal(
      PyObject_GetAttrString(cinderjit_module, "after_fork_child"));
  if (callback == nullptr) {
    return -1;
  }
  auto args = Ref<>::steal(PyTuple_New(0));
  if (args == nullptr) {
    return -1;
  }
  auto kwargs = Ref<>::steal(PyDict_New());
  if (kwargs == nullptr ||
      PyDict_SetItemString(kwargs, "after_in_child", callback) < 0 ||
      PyObject_Call(register_at_fork, args, kwargs) == nullptr) {
    return -1;
  }
  return 0;
}

// Informs the JIT that an instance has had an assignment to its __class__
// field.
void instanceTypeAssigned(PyTypeObject* old_ty, PyTypeObject* new_ty) {
  if (auto rt = Runtime::getUnchecked()) {
    rt->notifyTypeModified(old_ty, new_ty);
  }
}

// JIT audit event callback. For now, we only pay attention to when an object's
// __class__ is assigned to.
int jit_audit_hook(const char* event, PyObject* args, void* /* data */) {
  if (strcmp(event, "object.__setattr__") != 0 || PyTuple_GET_SIZE(args) != 3) {
    return 0;
  }
  BorrowedRef<> name(PyTuple_GET_ITEM(args, 1));
  if (!PyUnicode_Check(name) ||
      PyUnicode_CompareWithASCIIString(name, "__class__") != 0) {
    return 0;
  }

  BorrowedRef<> object(PyTuple_GET_ITEM(args, 0));
  BorrowedRef<PyTypeObject> new_type(PyTuple_GET_ITEM(args, 2));
  instanceTypeAssigned(Py_TYPE(object), new_type);
  return 0;
}

int install_jit_audit_hook() {
  void* kData = nullptr;
  if (!installAuditHook(jit_audit_hook, kData)) {
    PyErr_SetString(PyExc_RuntimeError, "Could not install JIT audit hook");
    return -1;
  }
  return 0;
}

void dump_jit_stats() {
  auto stats = Ref<>::steal(get_and_clear_runtime_stats(nullptr, nullptr));
  if (stats == nullptr) {
    return;
  }
  auto stats_str = Ref<>::steal(PyObject_Str(stats));
  if (!stats_str) {
    return;
  }

  JIT_LOG("JIT runtime stats:\n{}", PyUnicode_AsUTF8(stats_str.get()));
}

constexpr std::string_view getCpuArchName() {
#if defined(__x86_64__)
  return "x86-64";
#elif defined(__i386__)
  return "x86 (32-bit)";
#elif defined(__aarch64__)
  return "arm64";
#elif defined(__arm__)
  return "arm";
#elif defined(__riscv)
  return "riscv";
#else
  return "unknown";
#endif
}

} // namespace

#if PY_VERSION_HEX < 0x030C0000
PyObject* _PyJIT_GenSend(
    PyGenObject* gen,
    PyObject* arg,
    int exc,
    PyFrameObject* f,
    PyThreadState* tstate,
    int finish_yield_from) {
  GenDataFooter* gen_footer = genDataFooter(gen);

  // state should be valid and the generator should not be completed
  JIT_DCHECK(
      gen_footer->state == Ci_JITGenState_JustStarted ||
          gen_footer->state == Ci_JITGenState_Running,
      "Invalid JIT generator state");

  gen_footer->state = Ci_JITGenState_Running;

  // JIT generators use nullptr arg to indicate an exception
  if (exc) {
    JIT_DCHECK(
        arg == Py_None, "Arg should be None when injecting an exception");
    arg = nullptr;
  } else {
    if (arg == nullptr) {
      arg = Py_None;
    }
  }

  if (f) {
    // Setup tstate/frame as would be done in PyEval_EvalFrameEx() or
    // prologue of a JITed function.
    tstate->frame = f;
    f->f_state = FRAME_EXECUTING;
    // This compensates for the decref which occurs in JITRT_UnlinkFrame().
    Py_INCREF(f);
    // This satisfies code which uses f_lasti == -1 or < 0 to check if a
    // generator is not yet started, but still provides a garbage value in case
    // anything tries to actually use f_lasti.
    f->f_lasti = std::numeric_limits<int>::max();
  }

  // Enter generated code.
  JIT_DCHECK(
      gen_footer->yieldPoint != nullptr,
      "Attempting to resume a generator with no yield point");
  PyObject* result =
      gen_footer->resumeEntry((PyObject*)gen, arg, finish_yield_from, tstate);

  if (!result && (gen->gi_jit_data != nullptr)) {
    // Generator jit data (gen_footer) will be freed if the generator
    // deopts
    gen_footer->state = Ci_JITGenState_Completed;
  }

  return result;
}

PyFrameObject* _PyJIT_GenMaterializeFrame(PyGenObject* gen) {
  PyThreadState* tstate = PyThreadState_Get();
  PyFrameObject* frame = jit::materializePyFrameForGen(tstate, gen);
  return frame;
}

int _PyJIT_GenVisitRefs(PyGenObject* gen, visitproc visit, void* arg) {
  GenDataFooter* gen_footer = genDataFooter(gen);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  const GenYieldPoint* yield_point = gen_footer->yieldPoint;
  if (gen_footer->state != Ci_JITGenState_Completed && yield_point) {
    size_t deopt_idx = yield_point->deoptIdx();
    const DeoptMetadata& deopt_meta =
        gen_footer->code_rt->getDeoptMetadata(deopt_idx);
    return Runtime::get()->forEachOwnedRef(gen, deopt_meta, [&](PyObject* v) {
      Py_VISIT(v);
      return 0;
    });
  }
  return 0;
}

void _PyJIT_GenDealloc(PyGenObject* gen) {
  GenDataFooter* gen_footer = genDataFooter(gen);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  const GenYieldPoint* yield_point = gen_footer->yieldPoint;
  if (gen_footer->state != Ci_JITGenState_Completed && yield_point) {
    size_t deopt_idx = yield_point->deoptIdx();
    const DeoptMetadata& deopt_meta =
        gen_footer->code_rt->getDeoptMetadata(deopt_idx);
    Runtime::get()->forEachOwnedRef(gen, deopt_meta, [](PyObject* v) {
      Py_DECREF(v);
      return 0;
    });
  }
  jitgen_data_free(gen);
}

PyObject* _PyJIT_GenYieldFromValue(PyGenObject* gen) {
  GenDataFooter* gen_footer = genDataFooter(gen);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  const GenYieldPoint* yield_point = gen_footer->yieldPoint;
  PyObject* yield_from = nullptr;
  if (gen_footer->state != Ci_JITGenState_Completed && yield_point) {
    yield_from = yieldFromValue(gen_footer, yield_point);
    Py_XINCREF(yield_from);
  }
  return yield_from;
}

PyObject* _PyJIT_GetGlobals(PyThreadState* tstate) {
  if (tstate->shadow_frame == nullptr) {
    JIT_CHECK(
        tstate->frame == nullptr,
        "Python frame {} without corresponding shadow frame",
        static_cast<void*>(tstate->frame));
    return nullptr;
  }
  return runtimeFrameStateFromThreadState(tstate).globals();
}

PyObject* _PyJIT_GetBuiltins(PyThreadState* tstate) {
  if (tstate->shadow_frame == nullptr) {
    JIT_CHECK(
        tstate->frame == nullptr,
        "Python frame {} without corresponding shadow frame",
        static_cast<void*>(tstate->frame));
    return tstate->interp->builtins;
  }
  return runtimeFrameStateFromThreadState(tstate).builtins();
}

PyFrameObject* _PyJIT_GetFrame(PyThreadState* tstate) {
  if (isJitInitialized()) {
    return jit::materializeShadowCallStack(tstate);
  }
  return tstate->frame;
}
#endif

namespace jit {

int initialize() {
  JIT_CHECK(
      getConfig().state != State::kFinalizing,
      "Trying to re-initialize the JIT as it is finalizing");
  if (isJitInitialized()) {
    return 0;
  }

  // Save the force_init field as it might have be set by test code before
  // jit::initialize() is called.
  auto force_init = getConfig().force_init;
  getMutableConfig() = Config{};
  if (force_init.has_value()) {
    getMutableConfig().force_init = force_init;
  }

  FlagProcessor flag_processor = initFlagProcessor();
  if (jit_help) {
    std::cout << flag_processor.jitXOptionHelpMessage() << '\n';
    // Return rather than exit here for arg printing test doesn't end early.
    return -2;
  }

  // Handle force_init = false case only after parsing all flags.
  if (getConfig().force_init == std::make_optional(false)) {
    return 0;
  }

  // Do this check after config is initialized, so we can use JIT_DLOG().
#ifndef __x86_64__
  JIT_DLOG(
      "JIT only supported x86-64 platforms, detected current architecture as "
      "'{}'. Disabling the JIT.",
      getCpuArchName());
  return 0;
#endif

  std::unique_ptr<JITList> jit_list;
  if (!getConfig().jit_list.filename.empty()) {
    if (getConfig().allow_jit_list_wildcards) {
      jit_list = jit::WildcardJITList::create();
    } else {
      jit_list = jit::JITList::create();
    }
    if (jit_list == nullptr) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to allocate JIT list");
      return -1;
    }

    try {
      jit_list->parseFile(getConfig().jit_list.filename.c_str());
    } catch (const std::exception& exn) {
      PyErr_SetString(PyExc_RuntimeError, exn.what());
      return -1;
    }
  }

#if PY_VERSION_HEX >= 0x030C0000
  jit::init_jit_genobject_type();
#endif

  // Create code allocator after jit::Config has been filled out.
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  mod_state->setCodeAllocator(CodeAllocator::make());

  // Initialize the main compiler object and its context.  This will throw if
  // asmjit cannot initialize.
  try {
    cinderx::getModuleState()->setJitContext(new CompilerContext<Compiler>());
  } catch (const std::exception& exn) {
    PyErr_SetString(PyExc_RuntimeError, exn.what());
    return -1;
  }

  PyObject* mod = _Ci_CreateBuiltinModule(&jit_module, "cinderjit");
  if (mod == nullptr) {
    return -1;
  }

  jitCtx()->setCinderJitModule(Ref<>::steal(mod));

  if (install_jit_audit_hook() < 0 || register_fork_callback(mod) < 0) {
    return -1;
  }

  if (getConfig().support_instrumentation) {
    patchSysMonitoringFunctions(mod);
    patchSysSetProfileAndSetTrace(mod);
  }

  getMutableConfig().state = State::kRunning;

  mod_state->setJitList(std::move(jit_list));

  return 0;
}

void finalize() {
  if (!isJitInitialized()) {
    return;
  }

  // Disable the JIT first so nothing we do in here ends up attempting to
  // invoke the JIT while we're finalizing our data structures.
  getMutableConfig().state = State::kFinalizing;

  // Deopt all JIT generators, since JIT generators reference code and other
  // metadata that we will be freeing later in this function.
  PyUnstable_GC_VisitObjects(deopt_gen_visitor, nullptr);

  if (getConfig().log.dump_stats) {
    dump_jit_stats();
  }

  // Always release references from Runtime objects: C++ clients may have
  // invoked the JIT directly without initializing a full jit::Context.
  jit::Runtime::get()->clearDeoptStats();
  jit::Runtime::get()->releaseReferences();

  deleteJitList();

  // Clear some global maps that reference Python data.
  auto mod_state = cinderx::getModuleState();
  auto& jit_code_outer_funcs = mod_state->codeOuterFunctions();
  auto& jit_reg_units = mod_state->registeredCompilationUnits();
  jit_code_outer_funcs.clear();
  jit_reg_units.clear();
  JIT_CHECK(
      hir::preloaderManager().empty(),
      "JIT cannot be finalized while batch compilation is active");

  for (auto func : jitCtx()->compiledFuncs()) {
    deoptFuncImpl(func);
  }
  mod_state->setJitContext(nullptr);
  mod_state->setCodeAllocator(nullptr);

  g_aot_ctx.destroy();

  restoreSysMonitoringRegisterCallback();
  restoreSysSetProfileAndSetTrace();

  getMutableConfig().state = State::kNotInitialized;
}

bool shouldScheduleCompile(BorrowedRef<PyFunctionObject> func) {
  // Can be called after the module has been finalized, due to function events.
  if (jitCtx() == nullptr) {
    return false;
  }

  if (isCinderModule(func->func_module)) {
    return false;
  }

  // Note: This is not the same as fetching the function's code object and
  // checking its module and qualname, as functions can be renamed after they
  // are created.  Code objects cannot.
  if (auto jit_list = cinderx::getModuleState()->jitList()) {
    return jit_list->lookupFunc(func) == 1;
  }

  BorrowedRef<PyCodeObject> code{func->func_code};
  return shouldAlwaysScheduleCompile(code) ||
      getConfig().compile_after_n_calls.has_value();
}

bool shouldScheduleCompile(
    BorrowedRef<> module_name,
    BorrowedRef<PyCodeObject> code) {
  if (isCinderModule(module_name)) {
    return false;
  }

  if (auto jit_list = cinderx::getModuleState()->jitList()) {
    return jit_list->lookupCode(code) == 1 ||
        jit_list->lookupName(module_name, code->co_qualname) == 1;
  }

  return shouldAlwaysScheduleCompile(code) ||
      getConfig().compile_after_n_calls.has_value();
}

bool scheduleJitCompile(BorrowedRef<PyFunctionObject> func) {
  // Could be creating an inner function with an already-compiled code object.
  if (isJitCompiled(func)) {
    return true;
  }

  // Attempt to attach already-compiled code even if the JIT is disabled, as
  // long as it hasn't been finalized.
  //
  // Without this, nested code objects would almost never run their compiled
  // functions if the user had disabled the JIT without selecting to deopt
  // everything.  This is a weird behavior though, to have "new" functions get
  // JIT-compiled code despite the JIT being disabled.
  if (reoptFunc(func)) {
    return true;
  }

  if (!isJitUsable()) {
    return false;
  }

  func->vectorcall = jitVectorcall;
  if (!registerFunction(func)) {
    func->vectorcall = getInterpretedVectorcall(func);
    return false;
  }

  return true;
}

_PyJIT_Result compileFunction(BorrowedRef<PyFunctionObject> func) {
  if (!isJitInitialized()) {
    return PYJIT_NOT_INITIALIZED;
  }
  if (isJitPaused()) {
    return PYJIT_RESULT_RETRY;
  }
  if (!isJitUsable()) {
    return PYJIT_RESULT_UNKNOWN_ERROR;
  }

  auto& jit_reg_units = cinderx::getModuleState()->registeredCompilationUnits();
  jit_reg_units.erase(func);
  return compile_func(func);
}

std::vector<BorrowedRef<PyFunctionObject>> preloadFuncAndDeps(
    BorrowedRef<PyFunctionObject> func,
    bool forcePreload) {
  // Add one for the original function itself.
  size_t limit = getConfig().preload_dependent_limit + 1;

  std::deque<BorrowedRef<PyFunctionObject>> worklist;
  std::vector<BorrowedRef<PyFunctionObject>> result;

  // Track units that are deleted while preloading.
  std::unordered_set<PyObject*> deleted_units;

  worklist.push_back(func);

  auto shouldPreload = [&](BorrowedRef<PyFunctionObject> f) {
    return !isPreloaded(f) && (shouldScheduleCompile(f) || forcePreload);
  };

  while (worklist.size() > 0 && result.size() < limit) {
    BorrowedRef<PyFunctionObject> f = worklist.front();
    worklist.pop_front();

    // This needs to be set every time before preload() is kicked off.
    // Preloading can run arbitrary Python code, which means it can re-enter
    // the JIT.
    handle_unit_deleted_during_preload = [&](PyObject* deleted_unit) {
      deleted_units.emplace(deleted_unit);
    };
    hir::Preloader* preloader = preload(f);
    handle_unit_deleted_during_preload = nullptr;

    if (preloader == nullptr) {
      return {};
    }
    result.emplace_back(f);

    // Preload all invoked Static Python functions because then the JIT can
    // compile them and emit direct calls to them from the original function.
    for (const auto& [descr, target] : preloader->invokeFunctionTargets()) {
      if (!target->is_function || !target->is_statically_typed) {
        continue;
      }
      BorrowedRef<PyFunctionObject> target_func = target->func();
      if (shouldPreload(target_func)) {
        worklist.push_back(target_func);
      }
    }

    // Preload any used functions in case the JIT might want to inline them.
    for (const auto& [idx, name] : preloader->globalNames()) {
      BorrowedRef<> obj = preloader->global(idx);
      if (!obj || !PyFunction_Check(obj)) {
        continue;
      }
      BorrowedRef<PyFunctionObject> target_func = obj.get();
      if (shouldPreload(target_func)) {
        worklist.push_back(target_func);
      }
    }
  }

  // Prune out all functions that are no longer alive / allocated.
  result.erase(
      std::remove_if(
          result.begin(),
          result.end(),
          [&](BorrowedRef<PyFunctionObject> func) {
            return deleted_units.contains(func.getObj()) ||
                deleted_units.contains(func->func_code);
          }),
      result.end());

  std::reverse(result.begin(), result.end());
  return result;
}

void codeDestroyed(BorrowedRef<PyCodeObject> code) {
  if (isJitUsable()) {
    auto mod_state = cinderx::getModuleState();
    auto& jit_reg_units = mod_state->registeredCompilationUnits();
    auto& jit_code_outer_funcs = mod_state->codeOuterFunctions();
    jit_reg_units.erase(code.getObj());
    jit_code_outer_funcs.erase(code.getObj());
    if (handle_unit_deleted_during_preload != nullptr) {
      handle_unit_deleted_during_preload(code.getObj());
    }
  }
}

void funcDestroyed(BorrowedRef<PyFunctionObject> func) {
  if (isJitUsable()) {
    auto mod_state = cinderx::getModuleState();

    auto& jit_reg_units = mod_state->registeredCompilationUnits();
    jit_reg_units.erase(func.getObj());
    if (handle_unit_deleted_during_preload != nullptr) {
      handle_unit_deleted_during_preload(func.getObj());
    }

    // erase any child code objects we registered too
    if (mod_state->jitList() != nullptr) {
      auto& jit_code_outer_funcs = mod_state->codeOuterFunctions();
      PyObject* module = func->func_module;
      BorrowedRef<PyCodeObject> top_code{func->func_code};
      BorrowedRef<> top_consts{top_code->co_consts};
      for (BorrowedRef<PyCodeObject> code :
           findNestedCodes(module, top_consts)) {
        jit_reg_units.erase(code);
        jit_code_outer_funcs.erase(code);
        if (handle_unit_deleted_during_preload != nullptr) {
          handle_unit_deleted_during_preload(code.getObj());
        }
      }
    }
  }

  // Have to check if context exists as this can fire after jit::finalize().
  if (jitCtx()) {
    jitCtx()->funcDestroyed(func);
  }
}

void funcModified(BorrowedRef<PyFunctionObject> func) {
  deoptFunc(func);
}

void typeDestroyed(BorrowedRef<PyTypeObject> type) {
  if (auto rt = Runtime::getUnchecked()) {
    rt->notifyTypeModified(type, nullptr);
  }
}

void typeModified(BorrowedRef<PyTypeObject> type) {
  if (auto rt = Runtime::getUnchecked()) {
    rt->notifyTypeModified(type, type);
  }
}

void typeNameModified(BorrowedRef<PyTypeObject> type) {
  // We assume that this is a very rare case, and simply give up on tracking
  // the type if it happens.
  if (auto rt = Runtime::getUnchecked()) {
    rt->notifyTypeModified(type, type);
  }
}

Context::CompilationResult compilePreloaderImpl(
    jit::CompilerContext<Compiler>* jit_ctx,
    const hir::Preloader& preloader) {
  BorrowedRef<PyCodeObject> code = preloader.code();
  if (code == nullptr) {
    JIT_DLOG("Can't compile {} as it has no code object", preloader.fullname());
    return {nullptr, PYJIT_RESULT_CANNOT_SPECIALIZE};
  }

  BorrowedRef<PyDictObject> builtins = preloader.builtins();
  BorrowedRef<PyDictObject> globals = preloader.globals();

  // Don't care flags: CO_NOFREE, CO_FUTURE_* (the only still-relevant future is
  // "annotations" which doesn't impact bytecode execution.)
  int required_flags = CO_OPTIMIZED | CO_NEWLOCALS;
  if ((code->co_flags & required_flags) != required_flags) {
    JIT_DLOG(
        "Can't compile {} due to missing required code flags",
        preloader.fullname());
    return {nullptr, PYJIT_RESULT_CANNOT_SPECIALIZE};
  }
  if (code->co_flags & CI_CO_SUPPRESS_JIT) {
    JIT_DLOG(
        "Can't compile {} as it has had the JIT suppressed",
        preloader.fullname());
    return {nullptr, PYJIT_RESULT_CANNOT_SPECIALIZE};
  }
  constexpr int forbidden_flags =
      PY_VERSION_HEX >= 0x030C0000 ? CO_ASYNC_GENERATOR : 0;
  if (code->co_flags & forbidden_flags) {
    JIT_DLOG(
        "Cannot JIT compile {} as it has prohibited code flags: 0x{:x}",
        preloader.fullname(),
        code->co_flags & forbidden_flags);
    return {nullptr, PYJIT_RESULT_CANNOT_SPECIALIZE};
  }

  CompilationKey key{code, builtins, globals};
  {
    // Attempt to atomically transition the code from "not compiled" to "in
    // progress".
    ThreadedCompileSerialize guard;
    if (CompiledFunction* compiled =
            jit_ctx->lookupCode(code, builtins, globals)) {
      return {compiled, PYJIT_RESULT_OK};
    }
    if (!jit_ctx->addActiveCompile(key)) {
      return {nullptr, PYJIT_RESULT_RETRY};
    }
  }

  std::unique_ptr<CompiledFunction> compiled;
  try {
    compiled = jit_ctx->compiler().Compile(preloader);
  } catch (const std::exception& exn) {
    JIT_DLOG("{}", exn.what());
  }

  ThreadedCompileSerialize guard;
  jit_ctx->removeActiveCompile(key);
  if (compiled == nullptr) {
    return {nullptr, PYJIT_RESULT_UNKNOWN_ERROR};
  }

  register_pycode_debug_symbol(
      code, preloader.fullname().c_str(), compiled.get());

  jit_ctx->addCompileTime(compiled->compileTime());

  // Register and return the compiled code
  return {
      jit_ctx->addCompiledFunction(key, std::move(compiled)), PYJIT_RESULT_OK};
}

} // namespace jit

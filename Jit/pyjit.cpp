// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/pyjit.h"

#include <Python.h>

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "cinder/genobject_jit.h"
#include "internal/pycore_ceval.h"
#include "internal/pycore_shadow_frame.h"
#endif

#include "internal/pycore_interp.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/audit.h"
#include "cinderx/Common/code.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/elf/writer.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/Jit/inline_cache.h"
#include "cinderx/Jit/jit_flag_processor.h"
#include "cinderx/Jit/jit_gdb_support.h"
#include "cinderx/Jit/jit_list.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/jit_time_log.h"
#include "cinderx/Jit/lir/inliner.h"
#include "cinderx/Jit/mmap_file.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove
#include "i386-dis/dis-asm.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
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

// Extra information needed to compile a PyCodeObject.
struct CodeData {
  CodeData(PyObject* m, PyObject* b, PyObject* g) {
    JIT_DCHECK(
        !getThreadedCompileContext().compileRunning(),
        "unexpected multithreading");
    module = Ref<>::create(m);
    builtins = Ref<>::create(b);
    globals = Ref<>::create(g);
  }

  Ref<> module;
  Ref<PyDictObject> builtins;
  Ref<PyDictObject> globals;
};

std::chrono::milliseconds total_time;
std::unordered_map<PyFunctionObject*, std::chrono::milliseconds> function_times;

struct CompilationTimer {
  using Clock = std::chrono::steady_clock;

  explicit CompilationTimer(BorrowedRef<PyFunctionObject> func)
      : start{Clock::now()}, func{func} {}

  ~CompilationTimer() {
    auto end = Clock::now();
    auto time_span =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    total_time += time_span;
    jit::ThreadedCompileSerialize guard;
    function_times.emplace(func, time_span);
  }

  std::chrono::steady_clock::time_point start;
  BorrowedRef<PyFunctionObject> func;
};

// Amount of time taken to batch compile everything when disable_jit is called
std::chrono::milliseconds g_batch_compilation_time;

Context* jit_ctx{nullptr};
JITList* g_jit_list{nullptr};

// Function and code objects ("units") registered for compilation.
std::unordered_set<BorrowedRef<>> jit_reg_units;

// Only set during preloading. Used to keep track of functions that were
// deleted as a side effect of preloading.
using UnitDeletedCallback = std::function<void(PyObject*)>;
UnitDeletedCallback handle_unit_deleted_during_preload = nullptr;

// Every unit that is a code object has corresponding entry in jit_code_data.
std::unordered_map<BorrowedRef<PyCodeObject>, CodeData> jit_code_data;

// If non-empty, jit compiled functions' names will be written to this filename
// at shutdown.
std::string g_write_compiled_functions_file;

// Frequently-used strings that we intern at JIT startup and hold references to.
#define INTERNED_STRINGS(X) \
  X(bc_offset)              \
  X(code_hash)              \
  X(count)                  \
  X(description)            \
  X(filename)               \
  X(firstlineno)            \
  X(func_qualname)          \
  X(guilty_type)            \
  X(int)                    \
  X(lineno)                 \
  X(normal)                 \
  X(normvector)             \
  X(opname)                 \
  X(reason)                 \
  X(split_dict_keys)        \
  X(type_name)              \
  X(types)

#define DECLARE_STR(s) static PyObject* s_str_##s{nullptr};
INTERNED_STRINGS(DECLARE_STR)
#undef DECLARE_STR

std::array<PyObject*, 256> s_opnames;
std::array<PyObject*, hir::kNumOpcodes> s_hir_opnames;

std::atomic<int> g_compile_workers_attempted;
std::atomic<int> g_compile_workers_retries;

jit::FlagProcessor xarg_flag_processor;

int use_jit = 0;
int jit_help = 0;
std::string jl_fn;

void setJitLogFile(std::string log_filename) {
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
    g_log_file = file;
  }
}

void setASMSyntax(std::string asm_syntax) {
  if (asm_syntax.compare("intel") == 0) {
    set_intel_syntax();
  } else if (asm_syntax.compare("att") == 0) {
    set_att_syntax();
  } else {
    JIT_ABORT("Unknown asm syntax '{}'", asm_syntax);
  }
}

void warnJITOff(const char* flag) {
  JIT_LOG("Warning: JIT disabled; {} has no effect", flag);
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

void initFlagProcessor() {
  use_jit = 0;
  jl_fn = "";
  jit_help = 0;
  std::string write_compiled_functions_file = "";
  if (!xarg_flag_processor.hasOptions()) {
    // flags are inspected in order of definition below
    xarg_flag_processor.addOption(
        "jit", "PYTHONJIT", use_jit, "Enable the JIT");

    xarg_flag_processor.addOption(
        "jit-auto",
        "PYTHONJITAUTO",
        [](unsigned int threshold) {
          use_jit = 1;
          getMutableConfig().auto_jit_threshold = threshold;
        },
        "Enable auto-JIT mode, which compiles functions after the given "
        "threshold");

    xarg_flag_processor.addOption(
        "jit-debug", "PYTHONJITDEBUG", g_debug, "JIT debug and extra logging");

    xarg_flag_processor
        .addOption(
            "jit-log-file",
            "PYTHONJITLOGFILE",
            [](std::string log_filename) { setJitLogFile(log_filename); },
            "write log entries to <filename> rather than stderr")
        .withFlagParamName("filename");

    xarg_flag_processor
        .addOption(
            "jit-asm-syntax",
            "PYTHONJITASMSYNTAX",
            [](std::string asm_syntax) { setASMSyntax(asm_syntax); },
            "set the assembly syntax used in log files")
        .withFlagParamName("intel|att")
        .withDebugMessageOverride("Sets the assembly syntax used in log files");

    xarg_flag_processor
        .addOption(
            "jit-debug-refcount",
            "PYTHONJITDEBUGREFCOUNT",
            g_debug_refcount,
            "JIT refcount insertion debug mode")
        .withDebugMessageOverride("Enabling");

    xarg_flag_processor.addOption(
        "jit-debug-inliner",
        "PYTHONJITDEBUGINLINER",
        g_debug_inliner,
        "Enable debug logging for the JIT's HIR inliner");

    xarg_flag_processor
        .addOption(
            "jit-dump-hir",
            "PYTHONJITDUMPHIR",
            g_dump_hir,
            "log the HIR representation of all functions after initial "
            "lowering from bytecode")
        .withDebugMessageOverride("Dump initial HIR of JITted functions");

    xarg_flag_processor
        .addOption(
            "jit-dump-hir-passes",
            "PYTHONJITDUMPHIRPASSES",
            g_dump_hir_passes,
            "log the HIR after each optimization pass")
        .withDebugMessageOverride(
            "Dump HIR of JITted functions after each individual  optimization "
            "pass");

    xarg_flag_processor
        .addOption(
            "jit-dump-final-hir",
            "PYTHONJITDUMPFINALHIR",
            g_dump_final_hir,
            "log the HIR after all optimizations")
        .withDebugMessageOverride(
            "Dump final HIR of JITted functions after all optimizations");

    xarg_flag_processor
        .addOption(
            "jit-dump-lir",
            "PYTHONJITDUMPLIR",
            g_dump_lir,
            "log the LIR representation of all functions after lowering from "
            "HIR")
        .withDebugMessageOverride("Dump initial LIR of JITted functions");

    xarg_flag_processor.addOption(
        "jit-dump-lir-no-origin",
        "PYTHONJITDUMPLIRNOORIGIN",
        [](std::string) {
          g_dump_lir = 1;
          g_dump_lir_no_origin = 1;
        },
        "JIT dump-lir mode without origin data");

    xarg_flag_processor.addOption(
        "jit-dump-c-helper",
        "PYTHONJITDUMPCHELPER",
        g_dump_c_helper,
        "dump all c invocations");

    xarg_flag_processor.addOption(
        "jit-disas-funcs",
        "PYTHONJITDISASFUNCS",
        g_dump_asm,
        "jit-disas-funcs/PYTHONJITDISASFUNCS are deprecated and will soon be "
        "removed. Use jit-dump-asm and PYTHONJITDUMPASM instead");

    xarg_flag_processor.addOption(
        "jit-no-symbolize",
        "PYTHONJITNOSYMBOLIZE",
        [](const std::string&) { g_symbolize_funcs = 0; },
        "disable symbolization of functions called by JIT code");

    xarg_flag_processor
        .addOption(
            "jit-dump-asm",
            "PYTHONJITDUMPASM",
            g_dump_asm,
            "log the final compiled code, annotated with HIR instructions")
        .withDebugMessageOverride("Dump asm of JITted functions");

    xarg_flag_processor
        .addOption(
            "jit-dump-compiled-functions",
            "PYTHONJITDUMPCOMPILEDFUNCTIONS",
            g_write_compiled_functions_file,
            "dump JIT compiled functions to <filename>")
        .withFlagParamName("filename");

    xarg_flag_processor.addOption(
        "jit-enable-inline-cache-stats-collection",
        "PYTHONJITCOLLECTINLINECACHESTATS",
        [](int val) {
          if (use_jit) {
            getMutableConfig().collect_attr_cache_stats = !!val;
          } else {
            warnJITOff("jit-enable-inline-cache-stats-collection");
          }
        },
        "Collect inline cache stats (supported stats are cache misses for load "
        "method inline caches");

    xarg_flag_processor.addOption(
        "jit-gdb-support",
        "PYTHONJITGDBSUPPORT",
        [](std::string) {
          g_debug = 1;
          getMutableConfig().gdb.supported = true;
        },
        "GDB support and JIT debug mode");

    xarg_flag_processor.addOption(
        "jit-gdb-write-elf",
        "PYTHONJITGDBWRITEELF",
        [](std::string) {
          g_debug = 1;
          getMutableConfig().gdb.supported = true;
          getMutableConfig().gdb.write_elf_objects = true;
        },
        "Debugging aid, GDB support with ELF output");

    xarg_flag_processor.addOption(
        "jit-dump-stats",
        "PYTHONJITDUMPSTATS",
        g_dump_stats,
        "Dump JIT runtime stats at shutdown");

    xarg_flag_processor.addOption(
        "jit-disable-lir-inliner",
        "PYTHONJITDISABLELIRINLINER",
        g_disable_lir_inliner,
        "disable JIT lir inlining");

    xarg_flag_processor.addOption(
        "jit-disable-huge-pages",
        "PYTHONJITDISABLEHUGEPAGES",
        [](std::string) { getMutableConfig().use_huge_pages = false; },
        "disable huge page support");

    xarg_flag_processor.addOption(
        "jit-enable-jit-list-wildcards",
        "PYTHONJITENABLEJITLISTWILDCARDS",
        getMutableConfig().allow_jit_list_wildcards,
        "allow wildcards in JIT list");

    xarg_flag_processor.addOption(
        "jit-all-static-functions",
        "PYTHONJITALLSTATICFUNCTIONS",
        getMutableConfig().compile_all_static_functions,
        "JIT-compile all static functions");

    xarg_flag_processor
        .addOption(
            "jit-list-file",
            "PYTHONJITLISTFILE",
            [](std::string listFile) {
              jl_fn = listFile;
              use_jit = 1;
            },
            "Load list of functions to compile from <filename>")
        .withFlagParamName("filename");

    xarg_flag_processor.addOption(
        "jit-disable",
        "PYTHONJITDISABLE",
        [](int val) { use_jit = !val; },
        "disable the JIT");

    // these are only set if use_jit == 1
    xarg_flag_processor.addOption(
        "jit-shadow-frame",
        "PYTHONJITSHADOWFRAME",
        [](int val) {
          if (use_jit) {
            if constexpr (PY_VERSION_HEX >= 0x030B0000) {
              JIT_LOG(
                  "Warning: Cinder's shadow frames are not supported in Python "
                  "versions later than 3.10");
              return;
            }
            getMutableConfig().frame_mode =
                val ? FrameMode::kShadow : FrameMode::kNormal;
          } else {
            warnJITOff("jit-shadow-frame");
          }
        },
        "enable shadow frame mode");

    xarg_flag_processor.addOption(
        "jit-stable-frame",
        "PYTHONJITSTABLEFRAME",
        [](int val) {
          if (use_jit) {
            getMutableConfig().stable_frame = !!val;
          } else {
            warnJITOff("jit-stable-frame");
          }
        },
        "Assume that data found in the Python frame is unchanged across "
        "function calls");

    xarg_flag_processor.addOption(
        "jit-preload-dependent-limit",
        "PYTHONJITPRELOADDEPENDENTLIMIT",
        [](int val) {
          if (use_jit) {
            getMutableConfig().preload_dependent_limit = val;
          } else {
            warnJITOff("jit-preload-dependent-limit");
          }
        },
        "When compiling a function, set the number of dependent functions that "
        "can be compiled along with it.");

    // HIR optimizations.

#define HIR_OPTIMIZATION_OPTION(NAME, OPT, CLI, ENV) \
  xarg_flag_processor.addOption(                     \
      (CLI),                                         \
      (ENV),                                         \
      [](int val) {                                  \
        if (use_jit) {                               \
          getMutableConfig().hir_opts.OPT = !!val;   \
        } else {                                     \
          warnJITOff(CLI);                           \
        }                                            \
      },                                             \
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

    xarg_flag_processor.addOption(
        "jit-simplify-iteration-limit",
        "PYTHONJITSIMPLIFYITERATIONLIMIT",
        getMutableConfig().simplifier.iteration_limit,
        "Set the maximum number of times the simplifier can run over a "
        "function");
    xarg_flag_processor.addOption(
        "jit-simplify-new-block-limit",
        "PYTHONJITSIMPLIFYNEWBLOCKLIMIT",
        getMutableConfig().simplifier.new_block_limit,
        "Set the maximum number of blocks that can be added by the simplifier "
        "to a function");
    xarg_flag_processor.addOption(
        "jit-hir-inliner-cost-limit",
        "PYTHONJITHIRINLINERCOSTLIMIT",
        getMutableConfig().inliner_cost_limit,
        "Limit how much the inliner is able to inline. The number's definition "
        "is only relevant to the inliner itself.");

    xarg_flag_processor
        .addOption(
            "jit-batch-compile-workers",
            "PYTHONJITBATCHCOMPILEWORKERS",
            getMutableConfig().batch_compile_workers,
            "set the number of batch compile workers to <COUNT>")
        .withFlagParamName("COUNT");

    xarg_flag_processor
        .addOption(
            "jit-multithreaded-compile-test",
            "PYTHONJITMULTITHREADEDCOMPILETEST",
            [](int val) {
              if (use_jit) {
                getMutableConfig().multithreaded_compile_test = val;
              } else {
                warnJITOff("jit-multithreaded-compile-test ");
              }
            },
            "JIT multithreaded compile test")
        .isHiddenFlag(true);

    xarg_flag_processor.addOption(
        "jit-list-match-line-numbers",
        "PYTHONJITLISTMATCHLINENUMBERS",
        [](int val) {
          if (use_jit) {
            jitlist_match_line_numbers(val);
          } else {
            warnJITOff("jit-list-match-line-numbers");
          }
        },
        "JIT list match line numbers");

    xarg_flag_processor
        .addOption(
            "jit-time",
            "",
            [](std::string flag_value) { parseAndSetFuncList(flag_value); },
            "Measure time taken in compilation phases and output summary to "
            "stderr or approperiate logfile. Only functions in comma separated "
            "<function_list> list will be included. Comma separated list may "
            "include wildcards, * and ?. Wildcards are processed in glob "
            "fashion and not as regex.")
        .withFlagParamName("function_list")
        .withDebugMessageOverride(
            "Will capture time taken in compilation phases and output summary");

    xarg_flag_processor.addOption(
        "jit-dump-hir-passes-json",
        "PYTHONJITDUMPHIRPASSESJSON",
        [](std::string json_output_dir) {
          g_dump_hir_passes_json = json_output_dir;
          int mkdir_result = ::mkdir(g_dump_hir_passes_json.c_str(), 0755);
          JIT_CHECK(
              mkdir_result == 0 || errno == EEXIST,
              "could not make JSON directory");
        },
        "Dump IR passes as JSON to the directory specified by this flag's "
        "value");
    xarg_flag_processor.addOption(
        "jit-multiple-code-sections",
        "PYTHONJITMULTIPLECODESECTIONS",
        [](int val) {
          if (use_jit) {
            getMutableConfig().multiple_code_sections = val;
          } else {
            warnJITOff("jit-multiple-code-sections");
          }
        },
        "Enable emitting code into multiple code sections.");

    xarg_flag_processor.addOption(
        "jit-hot-code-section-size",
        "PYTHONJITHOTCODESECTIONSIZE",
        [](size_t val) {
          if (use_jit) {
            getMutableConfig().hot_code_section_size = val;
          } else {
            warnJITOff("jit-hot-code-section-size");
          }
        },
        "Enable emitting code into multiple code sections.");

    xarg_flag_processor.addOption(
        "jit-cold-code-section-size",
        "PYTHONJITCOLDCODESECTIONSIZE",
        [](size_t val) {
          if (use_jit) {
            getMutableConfig().cold_code_section_size = val;
          } else {
            warnJITOff("jit-cold-code-section-size");
          }
        },
        "Enable emitting code into multiple code sections.");

    xarg_flag_processor.addOption(
        "jit-attr-caches",
        "PYTHONJITATTRCACHES",
        [](int val) {
          if (use_jit) {
            getMutableConfig().attr_caches = !!val;
          } else {
            warnJITOff("jit-attr-caches");
          }
        },
        "Use inline caches for attribute access instructions");

    xarg_flag_processor.addOption(
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

    xarg_flag_processor.addOption(
        "jit-refine-static-python",
        "PYTHONJITREFINESTATICPYTHON",
        [](int val) {
          if (use_jit) {
            getMutableConfig().refine_static_python = !!val;
          } else {
            warnJITOff("jit-refine-static-python");
          }
        },
        "Add RefineType instructions to coerce Static Python types to be "
        "valid");

    xarg_flag_processor.addOption(
        "jit-perfmap",
        "JIT_PERFMAP",
        perf::jit_perfmap,
        "write out /tmp/perf-<pid>.map for JIT symbols");

    xarg_flag_processor
        .addOption(
            "jit-perf-dumpdir",
            "JIT_DUMPDIR",
            perf::perf_jitdump_dir,
            "absolute path to a <DIRECTORY> that exists. A perf jitdump file "
            "will be written to this directory")
        .withFlagParamName("DIRECTORY");

    xarg_flag_processor.addOption(
        "jit-help", "", jit_help, "print all available JIT flags and exits");

    xarg_flag_processor.addOption(
        "perf-trampoline-prefork-compilation",
        "PERFTRAMPOLINEPREFORKCOMPILATION",
        getMutableConfig().compile_perf_trampoline_prefork,
        "Compile perf trampoline pre-fork");

    xarg_flag_processor.addOption(
        "jit-max-code-size",
        "",
        [](const std::string& val) {
          if (use_jit) {
            getMutableConfig().max_code_size = parse_sized_argument(val);
          } else {
            warnJITOff("jit-max-code-size");
          }
        },
        "Set the maximum code size for JIT in bytes (no suffix). For kilobytes "
        "use k or K as a suffix. "
        "Megabytes is m or M and gigabytes is g or G. 0 implies no limit.");
  }

  xarg_flag_processor.setFlags(PySys_GetXOptions());

  if (getConfig().auto_jit_threshold > 0 && jl_fn != "") {
    JIT_LOG(
        "Warning: jit-auto and jit-list-file are both enabled; only functions "
        "on the jit-list will be compiled, and only after {} calls.",
        getConfig().auto_jit_threshold);
  }
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
  auto iter = jit_code_data.find(code);
  if (iter == jit_code_data.end()) {
    return fmt::format(
        "<Unknown code object {}>", static_cast<void*>(code.get()));
  }
  return codeFullname(iter->second.module, code);
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

  std::unique_ptr<hir::Preloader> preloader;
  if (func != nullptr) {
    preloader = hir::Preloader::makePreloader(func);
  } else {
    const CodeData& data = map_get(jit_code_data, code);
    preloader = hir::Preloader::makePreloader(
        code, data.builtins, data.globals, codeFullname(data.module, code));
  }

  if (preloader == nullptr) {
    JIT_CHECK(
        PyErr_Occurred(), "Expect a Python exception when preloading fails");
    return nullptr;
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
  return preloader ? jit_ctx->compilePreloader(func, *preloader)
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

void multithread_compile_units_preloaded(std::vector<BorrowedRef<>>&& units) {
  size_t batch_compile_workers = getConfig().batch_compile_workers;
  JIT_CHECK(batch_compile_workers, "Zero workers for compile");

  JIT_DLOG(
      "Running multithread_compile_units_preloaded for {} units with {} "
      "workers",
      units.size(),
      batch_compile_workers);

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
    for (size_t i = 0; i < batch_compile_workers; i++) {
      worker_threads.emplace_back(compile_worker_thread);
    }
  }
  for (std::thread& worker_thread : worker_threads) {
    worker_thread.join();
  }

  auto retry_list = getThreadedCompileContext().endCompile();
  JIT_DLOG(
      "multithread_compile_units_preloaded retrying {} units serially",
      retry_list.size());
  compile_units_preloaded(std::move(retry_list));
}

bool compile_all() {
  JIT_CHECK(jit_ctx, "JIT not initialized");

  std::vector<BorrowedRef<>> compilation_units;
  // units that were deleted during preloading
  std::unordered_set<PyObject*> deleted_units;

  auto error_cleanup = [&]() {
    hir::preloaderManager().clear();
    handle_unit_deleted_during_preload = nullptr;
  };

  JIT_DLOG(
      "Starting compile_all with {} registered units", jit_reg_units.size());

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

  if (getConfig().batch_compile_workers > 0) {
    multithread_compile_units_preloaded(std::move(compilation_units));
  } else {
    compile_units_preloaded(std::move(compilation_units));
  }

  hir::preloaderManager().clear();
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
  JIT_LOG("(Re)compiling {} units", jit_reg_units.size());
  jit_ctx->clearCache();
  std::chrono::time_point time_start = std::chrono::steady_clock::now();
  if (!compile_all()) {
    return nullptr;
  }
  std::chrono::time_point time_end = std::chrono::steady_clock::now();
  JIT_LOG(
      "Took {} ms, compiles attempted: {}, compiles retried: {}",
      std::chrono::duration_cast<std::chrono::milliseconds>(
          time_end - time_start)
          .count(),
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

PyObject* disable_jit(PyObject* /* self */, PyObject* args, PyObject* kwargs) {
  int do_compile_all = 1;
  int deopt_all = 0;

  const char* keywords[] = {"compile_all", "deopt_all", nullptr};

  if (!PyArg_ParseTupleAndKeywords(
          args,
          kwargs,
          "|pp",
          const_cast<char**>(keywords),
          &do_compile_all,
          &deopt_all)) {
    return nullptr;
  }
  if (jit_ctx == nullptr) {
    Py_RETURN_NONE;
  }

  JIT_DLOG("Disabling the JIT");

  if (do_compile_all) {
    // Compile all of the pending functions/codes before shutting down
    std::chrono::time_point start = std::chrono::steady_clock::now();
    if (!compile_all()) {
      return nullptr;
    }
    std::chrono::time_point end = std::chrono::steady_clock::now();
    g_batch_compilation_time =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    jit_code_data.clear();
  }

  if (deopt_all) {
    JIT_DLOG("Deopting {} compiled functions", jit_ctx->compiledFuncs().size());
    size_t success = 0;
    for (BorrowedRef<PyFunctionObject> func : jit_ctx->compiledFuncs()) {
      if (jit_ctx->deoptFunc(func)) {
        success++;
      } else {
        JIT_DLOG("Failed to deopt compiled function '{}'", funcFullname(func));
      }
    }
    JIT_DLOG("Deopted {} compiled functions", success);
  }

  getMutableConfig().is_enabled = 0;

  Py_RETURN_NONE;
}

PyObject* enable_jit(PyObject* /* self */, PyObject* /* arg */) {
  if (jit_ctx == nullptr) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "Trying to re-enable the JIT but it was never initialized");
    return nullptr;
  }

  size_t count = 0;
  for (BorrowedRef<PyFunctionObject> func : jit_ctx->deoptedFuncs()) {
    jit_ctx->reoptFunc(func);
    count++;
  }

  getMutableConfig().is_enabled = 1;

  JIT_DLOG("Re-enabled the JIT and re-optimized {} functions", count);

  Py_RETURN_NONE;
}

PyObject* get_batch_compilation_time_ms(PyObject*, PyObject*) {
  return PyLong_FromLong(g_batch_compilation_time.count());
}

PyObject* force_compile(PyObject* /* self */, PyObject* func_obj) {
  if (!PyFunction_Check(func_obj)) {
    PyErr_SetString(PyExc_TypeError, "force_compile expected a function");
    return nullptr;
  }

  BorrowedRef<PyFunctionObject> func = func_obj;

  if (!isJitUsable() || isJitCompiled(func)) {
    Py_RETURN_FALSE;
  }

  switch (_PyJIT_CompileFunction(func)) {
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
    case PYJIT_RESULT_PYTHON_EXCEPTION:
      return nullptr;
  }
  PyErr_SetString(PyExc_RuntimeError, "Unhandled compilation result");
  return nullptr;
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
      jit_ctx != nullptr,
      "Loading an AOT bundle currently requires the JIT to be enabled");

  if (!PyUnicode_Check(arg)) {
    PyErr_SetString(
        PyExc_ValueError, "load_aot_bundle expects a filename string");
    return nullptr;
  }

  const char* filename = PyUnicode_AsUTF8(arg);

  // TODO(alexanderm): Verify these options are what we want.
  auto handle = dlopen(filename, RTLD_NOW | RTLD_GLOBAL);
  if (handle == nullptr) {
    std::string msg = fmt::format(
        "Failed to dlopen() the AOT bundle at {}\n{}", filename, dlerror());
    PyErr_SetString(PyExc_RuntimeError, msg.c_str());
    return nullptr;
  }

  g_aot_ctx.init(handle);

  // TODO(T193608222): It would be great to do something other than mmapping the
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
  // TODO(alexanderm): This is terrible and we should be going the other way,
  // mapping read notes over to function objects.
  PyUnstable_GC_VisitObjects(aot_func_visitor, &g_aot_ctx);

  Py_RETURN_NONE;
}

PyObject* auto_jit_threshold(PyObject* /* self */, PyObject*) {
  return PyLong_FromLong(getConfig().auto_jit_threshold);
}

PyObject* is_jit_compiled(PyObject* /* self */, PyObject* func) {
  if (!PyFunction_Check(func)) {
    PyErr_SetString(
        PyExc_RuntimeError, "Must call is_jit_compiled with a function object");
    return nullptr;
  }

  if (isJitCompiled(reinterpret_cast<PyFunctionObject*>(func))) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyObject* print_hir(PyObject* /* self */, PyObject* func) {
  if (jit_ctx == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "JIT is not initialized");
    return nullptr;
  }
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return nullptr;
  }

  CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);
  if (compiled_func == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "function is not jit compiled");
    return nullptr;
  }

  compiled_func->printHIR();
  Py_RETURN_NONE;
}

PyObject* disassemble(PyObject* /* self */, PyObject* func) {
  if (jit_ctx == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "JIT is not initialized");
    return nullptr;
  }
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return nullptr;
  }

  CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);
  if (compiled_func == nullptr) {
    PyErr_SetString(PyExc_RuntimeError, "function is not jit compiled");
    return nullptr;
  }

  compiled_func->disassemble();
  Py_RETURN_NONE;
}

PyObject* dump_elf(PyObject* /* self */, PyObject* arg) {
  JIT_CHECK(
      jit_ctx != nullptr,
      "JIT context not initialized despite cinderjit module having been "
      "loaded");
  if (!PyUnicode_Check(arg)) {
    PyErr_SetString(PyExc_ValueError, "dump_elf expects a filename string");
    return nullptr;
  }

  Py_ssize_t filename_size = 0;
  const char* filename = PyUnicode_AsUTF8AndSize(arg, &filename_size);

  std::vector<elf::CodeEntry> entries;
  for (BorrowedRef<PyFunctionObject> func : jit_ctx->compiledFuncs()) {
    BorrowedRef<PyCodeObject> code{func->func_code};
    CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);

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
  if (g_jit_list == nullptr) {
    Py_RETURN_NONE;
  }
  return g_jit_list->getList().release();
}

PyObject* jit_list_append(PyObject* /* self */, PyObject* line) {
  if (g_jit_list == nullptr) {
    g_jit_list = JITList::create().release();
  }
  Py_ssize_t line_len;
  const char* line_str = PyUnicode_AsUTF8AndSize(line, &line_len);
  if (line_str == nullptr) {
    return nullptr;
  }
  g_jit_list->parseLine(
      {line_str, static_cast<std::string::size_type>(line_len)});
  Py_RETURN_NONE;
}

PyObject* get_compiled_functions(PyObject* /* self */, PyObject*) {
  auto funcs = Ref<>::steal(PyList_New(0));
  if (funcs == nullptr) {
    return nullptr;
  }
  for (BorrowedRef<PyFunctionObject> func : jit_ctx->compiledFuncs()) {
    if (PyList_Append(funcs, func) < 0) {
      return nullptr;
    }
  }
  return funcs.release();
}

PyObject* get_compilation_time(PyObject* /* self */, PyObject*) {
  return PyLong_FromLong(static_cast<long>(total_time.count()));
}

PyObject* get_function_compilation_time(PyObject* /* self */, PyObject* func) {
  auto iter = function_times.find(reinterpret_cast<PyFunctionObject*>(func));
  if (iter == function_times.end()) {
    Py_RETURN_NONE;
  }
  return PyLong_FromLong(iter->second.count());
}

PyObject* get_inlined_functions_stats(PyObject* /* self */, PyObject* arg) {
  if (jit_ctx == nullptr || !PyFunction_Check(arg)) {
    Py_RETURN_NONE;
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);
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
  if (jit_ctx == nullptr || !PyFunction_Check(arg)) {
    return PyLong_FromLong(0);
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);
  int size = compiled_func != nullptr
      ? compiled_func->inlinedFunctionsStats().num_inlined_functions
      : 0;
  return PyLong_FromLong(size);
}

PyObject* get_function_hir_opcode_counts(PyObject* /* self */, PyObject* arg) {
  if (jit_ctx == nullptr || !PyFunction_Check(arg)) {
    Py_RETURN_NONE;
  }
  BorrowedRef<PyFunctionObject> func{arg};
  CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);
  if (compiled_func == nullptr) {
    Py_RETURN_NONE;
  }

  const hir::OpcodeCounts& counts = compiled_func->hirOpcodeCounts();
  Ref<> dict = Ref<>::steal(PyDict_New());
  if (dict == nullptr) {
    return nullptr;
  }
#define HIR_OP(opname)                                               \
  {                                                                  \
    const size_t idx = static_cast<size_t>(hir::Opcode::k##opname);  \
    int count = counts.at(idx);                                      \
    if (count != 0) {                                                \
      Ref<> count_obj = Ref<>::steal(PyLong_FromLong(count));        \
      if (count_obj == nullptr) {                                    \
        return nullptr;                                              \
      }                                                              \
      if (PyDict_SetItem(dict, s_hir_opnames[idx], count_obj) < 0) { \
        return nullptr;                                              \
      }                                                              \
    }                                                                \
  }
  FOREACH_OPCODE(HIR_OP)
#undef HIR_OP
  return dict.release();
}

PyObject* mlock_profiler_dependencies(PyObject* /* self */, PyObject*) {
  if (jit_ctx == nullptr) {
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
  Runtime* runtime = Runtime::get();
  auto stats = Ref<>::steal(check(PyList_New(0)));

  for (auto& pair : runtime->deoptStats()) {
    const DeoptMetadata& meta = runtime->getDeoptMetadata(pair.first);
    const DeoptStat& stat = pair.second;
    const DeoptFrameMetadata& frame_meta = meta.innermostFrame();
    BorrowedRef<PyCodeObject> code = frame_meta.code;

    auto func_qualname = code->co_qualname;
    BCOffset line_offset = frame_meta.cause_instr_idx;
    int lineno_raw = code->co_linetable != nullptr
        ? PyCode_Addr2Line(code, line_offset.value())
        : -1;
    auto lineno = Ref<>::steal(check(PyLong_FromLong(lineno_raw)));
    auto reason =
        Ref<>::steal(check(PyUnicode_FromString(deoptReasonName(meta.reason))));
    auto description = Ref<>::steal(check(PyUnicode_FromString(meta.descr)));

    // Helper to create an event dict with a given count value.
    auto append_event = [&](size_t count_raw, const char* type_name) {
      auto event = Ref<>::steal(check(PyDict_New()));
      auto normals = Ref<>::steal(check(PyDict_New()));
      auto ints = Ref<>::steal(check(PyDict_New()));

      check(PyDict_SetItem(event, s_str_normal, normals));
      check(PyDict_SetItem(event, s_str_int, ints));
      check(PyDict_SetItem(normals, s_str_func_qualname, func_qualname));
      check(PyDict_SetItem(normals, s_str_filename, code->co_filename));
      check(PyDict_SetItem(ints, s_str_lineno, lineno));
      check(PyDict_SetItem(normals, s_str_reason, reason));
      check(PyDict_SetItem(normals, s_str_description, description));

      auto count = Ref<>::steal(check(PyLong_FromSize_t(count_raw)));
      check(PyDict_SetItem(ints, s_str_count, count));
      auto type_str =
          Ref<>::steal(check(PyUnicode_InternFromString(type_name)));
      check(PyDict_SetItem(normals, s_str_guilty_type, type_str) < 0);
      check(PyList_Append(stats, event));
    };

    // For deopts with type profiles, add a copy of the dict with counts for
    // each type, including "other".
    if (!stat.types.empty()) {
      for (size_t i = 0; i < stat.types.size && stat.types.types[i] != nullptr;
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
  if (jit_ctx == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->codeSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* get_compiled_stack_size(PyObject* /* self */, PyObject* func) {
  if (jit_ctx == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->stackSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* get_compiled_spill_stack_size(PyObject* /* self */, PyObject* func) {
  if (jit_ctx == nullptr) {
    return PyLong_FromLong(0);
  }
  CompiledFunction* compiled_func = jit_ctx->lookupFunc(func);
  int size = compiled_func != nullptr ? compiled_func->spillStackSize() : -1;
  return PyLong_FromLong(size);
}

PyObject* jit_frame_mode(PyObject* /* self */, PyObject*) {
  return PyLong_FromLong(static_cast<int>(getConfig().frame_mode));
}

PyObject* get_supported_opcodes(PyObject* /* self */, PyObject*) {
  auto set = Ref<>::steal(PySet_New(nullptr));
  if (set == nullptr) {
    return nullptr;
  }

  for (auto op : hir::kSupportedOpcodes) {
    auto op_obj = Ref<>::steal(PyLong_FromLong(op));
    if (op_obj == nullptr) {
      return nullptr;
    }
    if (PySet_Add(set, op_obj) < 0) {
      return nullptr;
    }
  }

  return set.release();
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

PyObject* jit_suppress(PyObject*, PyObject* func_obj) {
  if (!PyFunction_Check(func_obj)) {
    PyErr_SetString(PyExc_TypeError, "Input must be a function");
    return nullptr;
  }
  PyFunctionObject* func = reinterpret_cast<PyFunctionObject*>(func_obj);

  reinterpret_cast<PyCodeObject*>(func->func_code)->co_flags |=
      CI_CO_SUPPRESS_JIT;

  Py_INCREF(func_obj);
  return func_obj;
}

PyObject* get_allocator_stats(PyObject*, PyObject*) {
  auto base_allocator = CodeAllocator::get();
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

// If the given generator-like object is a suspended JIT generator, deopt it
// and return 1. Otherwise, return 0.
int deopt_gen_impl(PyGenObject* gen) {
#if PY_VERSION_HEX < 0x030C0000
  GenDataFooter* footer = genDataFooter(gen);
  if (Ci_GenIsCompleted(gen) || footer == nullptr) {
    return 0;
  }
  JIT_CHECK(
      footer->yieldPoint != nullptr,
      "Suspended JIT generator has nullptr yieldPoint");
  const DeoptMetadata& deopt_meta =
      Runtime::get()->getDeoptMetadata(footer->yieldPoint->deoptIdx());
  JIT_CHECK(
      deopt_meta.frame_meta.size() == 1,
      "Generators with inlined calls are not supported (T109706798)");

  _PyJIT_GenMaterializeFrame(gen);
  _PyShadowFrame_SetOwner(&gen->gi_shadow_frame, PYSF_INTERP);
  reifyGeneratorFrame(
      gen->gi_frame, deopt_meta, deopt_meta.frame_meta[0], footer);
  gen->gi_frame->f_state = FRAME_SUSPENDED;
  releaseRefs(deopt_meta, footer);
  jitgen_data_free(gen);
#else
  UPGRADE_NOTE(GENERATOR_JIT_SUPPORT, T194022335)
#endif
  return 1;
}

PyObject* deopt_gen(PyObject*, PyObject* gen) {
  if (!PyGen_Check(gen) && !PyCoro_CheckExact(gen) &&
      !PyAsyncGen_CheckExact(gen)) {
    PyErr_Format(
        PyExc_TypeError,
        "Exected generator-like object, got %.200s",
        Py_TYPE(gen)->tp_name);
    return nullptr;
  }
  if (Ci_GenIsExecuting(reinterpret_cast<PyGenObject*>(gen))) {
    PyErr_SetString(PyExc_RuntimeError, "generator is executing");
    return nullptr;
  }
  if (deopt_gen_impl(reinterpret_cast<PyGenObject*>(gen))) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

int deopt_gen_visitor(PyObject* obj, void*) {
  if (PyGen_Check(obj) || PyCoro_CheckExact(obj) ||
      PyAsyncGen_CheckExact(obj)) {
    deopt_gen_impl(reinterpret_cast<PyGenObject*>(obj));
  }
  return 1;
}

PyObject* after_fork_child(PyObject*, PyObject*) {
  perf::afterForkChild();
  Py_RETURN_NONE;
}

PyMethodDef jit_methods[] = {
    {"disable",
     reinterpret_cast<PyCFunction>(disable_jit),
     METH_VARARGS | METH_KEYWORDS,
     PyDoc_STR("Compile all functions that are pending compilation and then "
               "disable the JIT.")},
    {"enable",
     enable_jit,
     METH_NOARGS,
     PyDoc_STR("Re-enable the JIT and re-attach compiled onto previously "
               "JIT-compiled functions.")},
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
     PyDoc_STR("Load a bundle of ahead-of-time generated code from an ELF "
               "file, whose filepath is passed as the first argument. Note: "
               "This does not actually work yet, it's being used for debugging "
               "purposes.")},
    {"auto_jit_threshold",
     auto_jit_threshold,
     METH_NOARGS,
     PyDoc_STR("Return the current AutoJIT threshold, only makes sense when "
               "the JIT is enabled.")},
    {"is_jit_compiled",
     is_jit_compiled,
     METH_O,
     PyDoc_STR("Check if a function is jit compiled.")},
    {"force_compile",
     force_compile,
     METH_O,
     PyDoc_STR("Force a function to be JIT compiled if it hasn't yet.")},
    {"jit_frame_mode",
     jit_frame_mode,
     METH_NOARGS,
     PyDoc_STR("Get JIT frame mode (0 = normal frames, 1 = no frames, 2 = "
               "shadow frames).")},
    {"get_jit_list",
     get_jit_list,
     METH_NOARGS,
     PyDoc_STR("Get the list of functions to JIT compile.")},
    {"jit_list_append",
     jit_list_append,
     METH_O,
     PyDoc_STR("Parse a JIT-list line and append it.")},
    {"print_hir",
     print_hir,
     METH_O,
     PyDoc_STR("Print the HIR for a jitted function to stdout.")},
    {"get_supported_opcodes",
     get_supported_opcodes,
     METH_NOARGS,
     PyDoc_STR("Return a set of all supported opcodes, as ints.")},
    {"get_compiled_functions",
     get_compiled_functions,
     METH_NOARGS,
     PyDoc_STR("Return a list of functions that are currently JIT-compiled.")},
    {"get_compilation_time",
     get_compilation_time,
     METH_NOARGS,
     PyDoc_STR("Return the total time used for JIT compiling functions in "
               "milliseconds.")},
    {"get_function_compilation_time",
     get_function_compilation_time,
     METH_O,
     PyDoc_STR("Return the time used for JIT compiling a given function in "
               "milliseconds.")},
    {"get_and_clear_runtime_stats",
     get_and_clear_runtime_stats,
     METH_NOARGS,
     PyDoc_STR("Returns information about the runtime behavior of JIT-compiled "
               "code.")},
    {"clear_runtime_stats",
     clear_runtime_stats,
     METH_NOARGS,
     PyDoc_STR("Clears runtime stats about JIT-compiled code without returning "
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
     PyDoc_STR("Return True if jit-enable-inline-cache-stats-collection is on "
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
     PyDoc_STR("Return stack size in bytes used for register spills for a "
               "JIT-compiled function.")},
    {"jit_suppress",
     jit_suppress,
     METH_O,
     PyDoc_STR("Decorator to prevent the JIT from running on a function.")},
    {"multithreaded_compile_test",
     multithreaded_compile_test,
     METH_NOARGS,
     PyDoc_STR("Force multi-threaded recompile of still existing JIT functions "
               "for testing.")},
    {"is_multithreaded_compile_test_enabled",
     is_multithreaded_compile_test_enabled,
     METH_NOARGS,
     PyDoc_STR("Return True if multithreaded_compile_test mode is enabled.")},
    {"get_batch_compilation_time_ms",
     get_batch_compilation_time_ms,
     METH_NOARGS,
     PyDoc_STR("Return the number of milliseconds spent in batch compilation "
               "when disabling the JIT.")},
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
    {"get_inlined_functions_stats",
     get_inlined_functions_stats,
     METH_O,
     PyDoc_STR("Return a dict containing function inlining stats with the the "
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

PyModuleDef jit_module = {
    PyModuleDef_HEAD_INIT,
    "cinderjit", /* m_name */
    PyDoc_STR("Control the Cinder JIT compiler. Only available when the JIT "
              "has been enabled."),
    -1, /* m_size */
    jit_methods, /* m_methods */
    nullptr, /* m_slots */
    nullptr, /* m_traverse */
    nullptr, /* m_clear */
    nullptr, /* m_free */
};

bool shouldAlwaysCompile(BorrowedRef<PyCodeObject> code) {
  // No explicit list implies everything can and should be compiled.
  if (g_jit_list == nullptr) {
    return true;
  }

  // There's a config option for forcing all Static Python functions to be
  // compiled.
  bool is_static = code->co_flags & CI_CO_STATICALLY_COMPILED;
  return is_static && getConfig().compile_all_static_functions;
}

// Check whether a function should be compiled.
bool shouldCompile(BorrowedRef<PyFunctionObject> func) {
  return shouldAlwaysCompile(func->func_code) ||
      g_jit_list->lookupFunc(func) == 1;
}

// Check whether a code object should be compiled. Intended for nested code
// objects.
bool shouldCompile(BorrowedRef<> module_name, BorrowedRef<PyCodeObject> code) {
  return (
      shouldAlwaysCompile(code) || (g_jit_list->lookupCode(code) == 1) ||
      (g_jit_list->lookupName(module_name, code->co_qualname) == 1));
}

// Check if a function has been preloaded.
bool isPreloaded(BorrowedRef<PyFunctionObject> func) {
  return hir::preloaderManager().find(func) != nullptr;
}

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

  _PyJIT_Result result;
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

    result = jit_ctx->compilePreloader(target, *preloader);
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
  }

  BorrowedRef<PyFunctionObject> last_func = targets.back();
  JIT_CHECK(
      last_func == func,
      "Last compiled function expected to be {}, but got {}",
      funcFullname(func),
      funcFullname(last_func));
  return result;
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

// Initialize some interned strings that can be used even when the JIT is off.
int initializeInternedStrings() {
#define INTERN_STR(s)                                            \
  if ((s_str_##s = PyUnicode_InternFromString(#s)) == nullptr) { \
    return -1;                                                   \
  }
  INTERNED_STRINGS(INTERN_STR)
#undef INTERN_STR

#define MAKE_OPNAME(opname, opnum)                                             \
  /*                                                                           \
   * HAVE_ARGUMENT is not a real opcode, it shares its value with              \
   * STORE_NAME. It's the demarcation line between opcodes that take arguments \
   * and those that don't.  If we tried to intern the "HAVE_ARGUMENT" string   \
   * here, it would be leaked because the "STORE_NAME" string would silently   \
   * replace it.                                                               \
   */                                                                          \
  if (opname != HAVE_ARGUMENT) {                                               \
    if ((s_opnames.at(opnum) = PyUnicode_InternFromString(#opname)) ==         \
        nullptr) {                                                             \
      return -1;                                                               \
    }                                                                          \
  }
  PY_OPCODES(MAKE_OPNAME)
#undef MAKE_OPNAME

#define HIR_OP(opname)                                                 \
  if ((s_hir_opnames.at(static_cast<size_t>(hir::Opcode::k##opname)) = \
           PyUnicode_InternFromString(#opname)) == nullptr) {          \
    return -1;                                                         \
  }
  FOREACH_OPCODE(HIR_OP)
#undef HIR_OP

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
          code->co_qualname == nullptr || !shouldCompile(module, code)) {
        continue;
      }

      result.emplace_back(code);
      consts_tuples.emplace(code->co_consts);
    }
  }

  return result;
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

void dump_jit_compiled_functions(const std::string& filename) {
  std::ofstream file(filename);
  if (!file) {
    JIT_LOG("Failed to open {} when dumping jit compiled functions", filename);
    return;
  }
  for (BorrowedRef<PyFunctionObject> func : jit_ctx->compiledFuncs()) {
    file << funcFullname(func) << std::endl;
  }
}

void finalizeInternedStrings() {
#define CLEAR_STR(s) Py_CLEAR(s_str_##s);
  INTERNED_STRINGS(CLEAR_STR)
#undef CLEAR_STR

  for (PyObject*& opname : s_opnames) {
    Py_CLEAR(opname);
  }

  for (PyObject*& opname : s_hir_opnames) {
    Py_CLEAR(opname);
  }
}

} // namespace

int _PyJIT_Initialize() {
  if (getConfig().init_state == InitState::kInitialized) {
    return 0;
  }

  if (initializeInternedStrings() == -1) {
    return -1;
  }

  bool force_init = getConfig().force_init;
  getMutableConfig() = Config{};
  getMutableConfig().force_init = force_init;

  initFlagProcessor();

  if (jit_help) {
    std::cout << xarg_flag_processor.jitXOptionHelpMessage() << std::endl;
    // Return rather than exit here for arg printing test doesn't end early.
    return -2;
  }

  std::unique_ptr<JITList> jit_list;
  if (!jl_fn.empty()) {
    if (getConfig().allow_jit_list_wildcards) {
      jit_list = jit::WildcardJITList::create();
    } else {
      jit_list = jit::JITList::create();
    }
    if (jit_list == nullptr) {
      JIT_LOG("Failed to allocate JIT list");
      return -1;
    }
    if (!jit_list->parseFile(jl_fn.c_str())) {
      JIT_LOG("Could not parse jit-list, disabling JIT.");
      return 0;
    }
  }

  if (use_jit || getConfig().force_init) {
    JIT_DLOG("Initializing JIT");
  } else {
    return 0;
  }

  CodeAllocator::makeGlobalCodeAllocator();

  jit_ctx = new Context();

  PyObject* mod = PyModule_Create(&jit_module);
  if (mod == nullptr) {
    return -1;
  }

  jit_ctx->setCinderJitModule(Ref<PyObject>::steal(mod));

  PyObject* modname = PyUnicode_InternFromString("cinderjit");
  if (modname == nullptr) {
    return -1;
  }

  PyObject* modules = PyImport_GetModuleDict();
  int st = _PyImport_FixupExtensionObject(mod, modname, modname, modules);
  Py_DECREF(modname);
  if (st == -1) {
    return -1;
  }

  if (install_jit_audit_hook() < 0 || register_fork_callback(mod) < 0) {
    return -1;
  }

  getMutableConfig().init_state = InitState::kInitialized;
  getMutableConfig().is_enabled = use_jit;
  g_jit_list = jit_list.release();

  JIT_DLOG("JIT is {}", getConfig().is_enabled ? "enabled" : "disabled");

  total_time = std::chrono::milliseconds::zero();

  return 0;
}

_PyJIT_Result _PyJIT_CompileFunction(PyFunctionObject* raw_func) {
  if (jit_ctx == nullptr) {
    return PYJIT_NOT_INITIALIZED;
  }

  BorrowedRef<PyFunctionObject> func{raw_func};

  if (!shouldCompile(func)) {
    return PYJIT_RESULT_NOT_ON_JITLIST;
  }

  CompilationTimer timer(func);
  jit_reg_units.erase(func);
  return compile_func(func);
}

int _PyJIT_RegisterFunction(PyFunctionObject* func) {
  // Attempt to attach already-compiled code even if the JIT is disabled, as
  // long as it hasn't been finalized.
  if (jit_ctx != nullptr && jit_ctx->reoptFunc(func)) {
    return 1;
  }

  if (!isJitUsable()) {
    return 0;
  }
  auto max_code_size = getConfig().max_code_size;
  if (max_code_size && CodeAllocator::get()->usedBytes() >= max_code_size) {
    return 0;
  }

  JIT_CHECK(
      !getThreadedCompileContext().compileRunning(),
      "Not intended for using during threaded compilation");
  int result = 0;
  if (shouldCompile(func)) {
    jit_reg_units.emplace(reinterpret_cast<PyObject*>(func));
    result = 1;
  }

  // If we have an active jit-list, scan this function's code object for any
  // nested functions that might be on the jit-list, and register them as
  // well.
  if (g_jit_list != nullptr) {
    PyObject* module = func->func_module;
    PyObject* builtins = func->func_builtins;
    PyObject* globals = func->func_globals;
    for (auto code : findNestedCodes(
             module,
             reinterpret_cast<PyCodeObject*>(func->func_code)->co_consts)) {
      jit_reg_units.emplace(reinterpret_cast<PyObject*>(code.get()));
      jit_code_data.emplace(
          std::piecewise_construct,
          std::forward_as_tuple(code),
          std::forward_as_tuple(module, builtins, globals));
    }
  }
  return result;
}

void _PyJIT_TypeModified(PyTypeObject* type) {
  if (auto rt = Runtime::getUnchecked()) {
    rt->notifyTypeModified(type, type);
  }
}

void _PyJIT_TypeNameModified(PyTypeObject* type) {
  // We assume that this is a very rare case, and simply give up on tracking
  // the type if it happens.
  if (auto rt = Runtime::getUnchecked()) {
    rt->notifyTypeModified(type, type);
  }
}

void _PyJIT_TypeDestroyed(PyTypeObject* type) {
  if (auto rt = Runtime::getUnchecked()) {
    rt->notifyTypeModified(type, nullptr);
  }
}

void _PyJIT_FuncModified(PyFunctionObject* func) {
  if (jit_ctx) {
    jit_ctx->funcModified(func);
  }
}

void _PyJIT_FuncDestroyed(PyFunctionObject* func) {
  if (isJitUsable()) {
    auto func_obj = reinterpret_cast<PyObject*>(func);
    jit_reg_units.erase(func_obj);
    if (handle_unit_deleted_during_preload != nullptr) {
      handle_unit_deleted_during_preload(func_obj);
    }
  }
  if (jit_ctx) {
    jit_ctx->funcDestroyed(func);
  }
}

void _PyJIT_CodeDestroyed(PyCodeObject* code) {
  if (isJitUsable()) {
    auto code_obj = reinterpret_cast<PyObject*>(code);
    jit_reg_units.erase(code_obj);
    jit_code_data.erase(code);
    if (handle_unit_deleted_during_preload != nullptr) {
      handle_unit_deleted_during_preload(code_obj);
    }
  }
}

int _PyJIT_Finalize() {
  // Disable the JIT first so nothing we do in here ends up attempting to
  // invoke the JIT while we're finalizing our data structures.
  getMutableConfig().is_enabled = 0;

  // Deopt all JIT generators, since JIT generators reference code and other
  // metadata that we will be freeing later in this function.
  PyUnstable_GC_VisitObjects(deopt_gen_visitor, nullptr);

  if (g_dump_stats) {
    dump_jit_stats();
  }

  if (!g_write_compiled_functions_file.empty()) {
    dump_jit_compiled_functions(g_write_compiled_functions_file);
    g_write_compiled_functions_file.clear();
  }

  // Always release references from Runtime objects: C++ clients may have
  // invoked the JIT directly without initializing a full jit::Context.
  jit::Runtime::get()->clearDeoptStats();
  jit::Runtime::get()->releaseReferences();

  if (getMutableConfig().init_state == InitState::kInitialized) {
    delete g_jit_list;
    g_jit_list = nullptr;

    // Clear some global maps that reference Python data.
    jit_code_data.clear();
    jit_reg_units.clear();
    JIT_CHECK(
        hir::preloaderManager().empty(),
        "JIT cannot be finalized while batch compilation is active");

    getMutableConfig().init_state = InitState::kFinalized;

    JIT_CHECK(jit_ctx != nullptr, "jit_ctx not initialized");
    delete jit_ctx;
    jit_ctx = nullptr;

    CodeAllocator::freeGlobalCodeAllocator();
  }

  finalizeInternedStrings();

  Runtime::shutdown();
  Symbolizer::shutdown();

  g_aot_ctx.destroy();

  return 0;
}

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
    return Runtime::get()->forEachOwnedRef(gen, deopt_idx, [&](PyObject* v) {
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
    Runtime::get()->forEachOwnedRef(gen, deopt_idx, [](PyObject* v) {
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
  if (getConfig().init_state == InitState::kInitialized) {
    return jit::materializeShadowCallStack(tstate);
  }
  return tstate->frame;
}
#endif

namespace jit {

std::vector<BorrowedRef<PyFunctionObject>> preloadFuncAndDeps(
    BorrowedRef<PyFunctionObject> func) {
  // Add one for the original function itself.
  size_t limit = getConfig().preload_dependent_limit + 1;

  std::deque<BorrowedRef<PyFunctionObject>> worklist;
  std::vector<BorrowedRef<PyFunctionObject>> result;

  // Track units that are deleted while preloading.
  std::unordered_set<PyObject*> deleted_units;

  worklist.push_back(func);

  while (worklist.size() > 0 && result.size() < limit) {
    BorrowedRef<PyFunctionObject> f = worklist.front();
    worklist.pop_front();

    // This needs to be set every time before preload() is kicked off.
    // Preloading can run arbitrary Python code, which means it can re-enter the
    // JIT.
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
      if (!isPreloaded(target_func) && shouldCompile(target_func)) {
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
      if (!isPreloaded(target_func) && shouldCompile(target_func)) {
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

} // namespace jit

// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>

namespace jit {

// Lifetime diagram of the JIT compiler:
//
//   NotInitialized <---------+
//        |                   |
//        v                   |
//     Running <---> Paused   |
//        |            |      |
//        v            |      |
//    Finalizing <-----+      |
//        |                   |
//        |                   |
//        +-------------------+
enum class State : uint8_t {
  kNotInitialized,
  kRunning,
  kPaused,
  kFinalizing,
};

enum class FrameMode : uint8_t {
  kNormal,
  kShadow,
};

// List of HIR optimization passes to run.
struct HIROptimizations {
  bool begin_inlined_function_elim{true};
  bool builtin_load_method_elim{true};
  bool clean_cfg{true};
  bool dead_code_elim{true};
  bool dynamic_comparison_elim{true};
  bool guard_type_removal{true};
  // TODO(T156009029): Inliner should be on by default.
  bool inliner{false};
  bool insert_update_prev_instr{true};
  bool phi_elim{true};
  bool simplify{true};
};

struct SimplifierConfig {
  // The maximum number of times the simplifier can process a function's CFG.
  size_t iteration_limit{100};
  // The maximum number of new blocks that can be added by the simplifier to a
  // function.
  size_t new_block_limit{1000};
};

struct GdbOptions {
  // Whether GDB support is enabled.
  bool supported{false};
  // Whether to write generated ELF objects to disk.
  bool write_elf_objects{false};
};

struct Config {
  // Current lifetime state of the JIT.
  State state{State::kNotInitialized};
  // Ignore CLI arguments and environment variables, always initialize the JIT
  // without enabling it.  Intended for testing.
  bool force_init{false};
  FrameMode frame_mode{FrameMode::kNormal};
  bool allow_jit_list_wildcards{false};
  bool compile_all_static_functions{false};
  bool multiple_code_sections{false};
  bool multithreaded_compile_test{false};
  bool use_huge_pages{true};
  // Assume that data found in the Python frame is unchanged across function
  // calls.  This includes the code object, and the globals and builtins
  // dictionaries (but not their contents).
  bool stable_frame{true};
  // Use inline caches for attribute accesses.
  bool attr_caches{true};
  // Collect stats information about attribute caches.
  bool collect_attr_cache_stats{false};

  // Add RefineType instructions for Static Python values before they get
  // typechecked.  Enabled by default as HIR doesn't pass through Static Python
  // types very well right now.  Disable to expose new typing opportunities in
  // HIR.
  //
  // TODO(T195042385): Replace this with actual typing.
  bool refine_static_python{true};
  HIROptimizations hir_opts;
  SimplifierConfig simplifier;
  // Limit on how much the inliner can inline.  The number here is internal to
  // the inliner, doesn't have any specific meaning, and can change as the
  // inliner's algorithm changes.
  size_t inliner_cost_limit{2000};
  // Number of workers to use for batch compilation, like in precompile_all().
  // If this number isn't configured then batch compilation will happen inline
  // on the calling thread.
  size_t batch_compile_workers{0};
  // When a function is being compiled, this is the maximum number of dependent
  // functions called by it that can be compiled along with it.
  size_t preload_dependent_limit{99};
  // Sizes (in bytes) of the hot and cold code sections. Only applicable if
  // multiple code sections are enabled.
  size_t cold_code_section_size{0};
  size_t hot_code_section_size{0};
  // Memory threshold after which we stop jitting.
  size_t max_code_size{0};
  // Size (in number of entries) of the LoadAttrCached and StoreAttrCached
  // inline caches used by the JIT.
  uint32_t attr_cache_size{1};
  uint32_t auto_jit_threshold{0};
  GdbOptions gdb;
  bool compile_perf_trampoline_prefork{false};
};

// Get the JIT's current config object.
const Config& getConfig();

// Get the JIT's current config object with the intent of modifying it.
Config& getMutableConfig();

// Check that the JIT is initialized.  Though it might be paused and or
// finalizing, it's not necessarily usable.
bool isJitInitialized();

// Check that the JIT is initialized and is currently usable.
bool isJitUsable();

// Check that the JIT is initialized but currently paused and unusable.
bool isJitPaused();

} // namespace jit

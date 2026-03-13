// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/watchers.h"
#include "cinderx/Jit/code_allocator_iface.h"
#include "cinderx/Jit/containers.h"
#include "cinderx/Jit/context_iface.h"
#include "cinderx/Jit/generators_mm_iface.h"
#include "cinderx/Jit/global_cache_iface.h"
#include "cinderx/Jit/jit_list_iface.h"
#include "cinderx/Jit/symbolizer_iface.h"
#include "cinderx/async_lazy_value_iface.h"

#include <functional>
#include <memory>
#include <unordered_map>

namespace cinderx {

// State for the entire CinderX module.
//
// Prefer stashing data here instead of in global variables whenever possible.
// Especially for Python objects as this state is traversed and cleared by the
// Python GC.
struct ModuleState {
  // Implement CPython's traverse functionality for tracing through to GC
  // references.
  int traverse(visitproc visit, void* arg);

  // Implement CPython's clear functionality for dropping GC references.
  int clear();

  bool initBuiltinMembers();

  // State for dict/type/code/func watchers registered with CPython.
  WatcherState watcher_state;

  // Assorted JIT state.
  std::unique_ptr<jit::IGlobalCacheManager> cache_manager;
  std::unique_ptr<jit::ICodeAllocator> code_allocator;
  std::unique_ptr<jit::ISymbolizer> symbolizer;
  std::unique_ptr<jit::IJitContext> jit_context;
  std::unique_ptr<jit::IJITList> jit_list;
  std::unique_ptr<jit::IJitGenFreeList> jit_gen_free_list;

  std::unique_ptr<IAsyncLazyValueState> async_lazy_value;

  // CinderX's own coroutine and generator types used by the JIT.
  Ref<PyTypeObject> coro_type;
  Ref<PyTypeObject> gen_type;

  // Type for the custom awaitable returned by CinderX's anext() replacement.
  Ref<PyTypeObject> anext_awaitable_type;

  // Snapshotted member dicts for standard builtin types (int, str, list, etc.)
  // so the JIT optimizer can look up methods during multithreaded compilation
  // without calling PyType_Lookup (which isn't safe off the main thread).
  std::unordered_map<PyTypeObject*, Ref<>> builtin_members;

#if PY_VERSION_HEX < 0x030E0000
  // Sentinel function object placed in JIT frames to identify them.
  Ref<> frame_reifier;
#endif

  // Original sys._clear_type_cache, forwarded to by our replacement.
  Ref<> sys_clear_caches;

  // Reference to builtins.next so the JIT can recognize and inline calls to it.
  Ref<> builtin_next;

  // Original references to instrumentation functions that CinderX patches over.
  Ref<> orig_sys_monitoring_register_callback;
  Ref<> orig_sys_monitoring_free_tool_id;
  Ref<> orig_sys_setprofile;
  Ref<> orig_sys_settrace;

  // Function and code objects ("units") registered for compilation.
  jit::UnorderedSet<BorrowedRef<>> registered_compilation_units;

  // Function objects registered for pre-fork perf-trampoline compilation.
  jit::UnorderedSet<BorrowedRef<PyFunctionObject>> perf_trampoline_worklist;

  // The CinderX PyModule instance itself.  Stored so that code with data backed
  // by the module (e.g. the generator free-list) can prevent premature cleanup
  // by holding a reference to it.
  BorrowedRef<> cinderx_module;

  // Counters for multithreaded compilation diagnostics.
  std::atomic<int> compile_workers_attempted{0};
  std::atomic<int> compile_workers_retries{0};

  // Callback invoked when a compilation unit is deleted during preloading.
  std::function<void(BorrowedRef<>)> unit_deleted_during_preload;

  // Whether runtime modification of Strict Module types is allowed.
  bool enable_patching{false};
};

// Get the global ModuleState singleton.
ModuleState* getModuleState();

// Get the ModuleState from the CinderX module object.
//
// Prefer this to using the global singleton when possible.
ModuleState* getModuleState(BorrowedRef<> mod);

// Set the global ModuleState singleton, using the CinderX module object.
void setModuleState(BorrowedRef<> mod);

// Unset the global ModuleState singleton, but don't destroy it.
//
// Destroying the state object is done manually.
void removeModuleState();

} // namespace cinderx

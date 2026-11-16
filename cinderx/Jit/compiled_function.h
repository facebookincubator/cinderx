// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors CINDER_X86_64 from Common/define.h, which this part of the header
// cannot include: everything down to the closing `extern "C"` has to stay
// compilable as C, and that header declares C++.  The offsets below have to
// agree with the encodings gen_asm.cpp emits under CINDER_X86_64, so both
// blocks key off one condition rather than spelling it out twice.
#if defined(__x86_64__) || defined(_M_AMD64)
#define JITRT_ENTRY_OFFSETS_X86_64
#endif

// Specifies the offset from a JITed function entry point where the re-entry
// point for calling with the correct bound args lives.
#if defined(JITRT_ENTRY_OFFSETS_X86_64)
// push rbp                 1 byte
// mov rbp, rsp             3 bytes
// jmp correct-bound-args   2 bytes short, 5 bytes near
//
// Windows needs the near form. The sret bridge that the ABI forces into the
// argcount check puts `correct-bound-args` beyond the 127 bytes a short jump
// can reach, and this offset is what pins the jump to one encoding or the
// other. Everywhere else keeps the short form and the smaller offset.
#if defined(_WIN32)
#define JITRT_CALL_REENTRY_OFFSET (-9)
#else
#define JITRT_CALL_REENTRY_OFFSET (-6)
#endif
#elif defined(__aarch64__)
// stp fp, lr, [sp, #-16]!
// mov fp, sp
// b correct-bound-args
#define JITRT_CALL_REENTRY_OFFSET (-12)
#else
#define JITRT_CALL_REENTRY_OFFSET (0)
#endif

// Fixes the JITed function entry point up to be the re-entry point after
// binding the args.
#define JITRT_GET_REENTRY(entry) \
  ((vectorcallfunc)(((char*)entry) + JITRT_CALL_REENTRY_OFFSET))

// Specifies the offset from a JITed function entry point where the static
// entry point lives.
#if defined(JITRT_ENTRY_OFFSETS_X86_64)
// The static entry point is a fixed 13 bytes and sits immediately before the
// re-entry point, so this offset tracks JITRT_CALL_REENTRY_OFFSET.
#if defined(_WIN32)
#define JITRT_STATIC_ENTRY_OFFSET (-22)
#else
#define JITRT_STATIC_ENTRY_OFFSET (-19)
#endif
#elif defined(__aarch64__)
#define JITRT_STATIC_ENTRY_OFFSET (-28)
#else
// Without JIT support there's no entry offset.
#define JITRT_STATIC_ENTRY_OFFSET (0)
#endif

#undef JITRT_ENTRY_OFFSETS_X86_64

// Fixes the JITed function entry point up to be the static entry point after
// binding the args.
#define JITRT_GET_STATIC_ENTRY(entry) \
  ((vectorcallfunc)(((char*)entry) + JITRT_STATIC_ENTRY_OFFSET))

// Offset from the normal entry to the dedicated JIT-to-JIT register entry, used
// for direct register-ABI calls between ordinary (non-Static-Python) compiled
// functions.  On both architectures the entry begins with a patchable NOP so
// the relink patch is a pure NOP->branch store that never overwrites the real
// prologue.  On x86-64 that slot is a 5-byte NOP (the width of the JMP rel32
// the patch writes), making the entry 5 bytes larger than the static entry.  On
// aarch64 it is a single 4-byte NOP word (a CMODX-safe NOP->B store), making
// the entry 4 bytes larger.  Keeping a separate offset leaves the Static Python
// static entry (which is never patched) untouched.
#if defined(__x86_64__) || defined(_M_AMD64)
#define JITRT_JIT_TO_JIT_ENTRY_OFFSET (JITRT_STATIC_ENTRY_OFFSET - 5)
#elif defined(__aarch64__)
#define JITRT_JIT_TO_JIT_ENTRY_OFFSET (JITRT_STATIC_ENTRY_OFFSET - 4)
#else
#define JITRT_JIT_TO_JIT_ENTRY_OFFSET (0)
#endif

#define JITRT_GET_JIT_TO_JIT_ENTRY(entry) \
  ((vectorcallfunc)(((char*)entry) + JITRT_JIT_TO_JIT_ENTRY_OFFSET))

// Fixes the JITed function entry point up to be the static entry point after
// binding the args.
#define JITRT_GET_NORMAL_ENTRY_FROM_STATIC(entry) \
  ((vectorcallfunc)(((char*)entry) - JITRT_STATIC_ENTRY_OFFSET))

/*
 * Check if a function has been compiled by Cinder's JIT and has a new
 * vectorcall entry point.
 *
 * Note: This returns false for the initial JIT entry points set by
 * scheduleJitCompile().
 */
bool isJitCompiled(const PyFunctionObject* func);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/frozen_list.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_patcher.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/profile.h"
#include "cinderx/Jit/stable_storage.h"

#include <chrono>
#include <cstddef>
#include <span>
#include <unordered_set>
#include <vector>

namespace cinderx::jit {

class CompiledFunctionOwner;

// Whether a code object is eligible to be called via the JIT-to-JIT native
// calling convention, and therefore should be given a JIT-to-JIT register entry
// point when compiled.  Requires the feature to be active and the function to
// have a simple positional signature.  This is the
// callee-side mirror of the call-shape checks the caller applies at a specific
// call site.  Static Python functions are excluded: they already have a static
// register entry and are dispatched via InvokeStaticFunction, so the two
// register-entry kinds never coexist in one compiled function.
inline bool isJitToJitCallTarget(PyCodeObject* code) {
  return useJitToJitCallingConvention() && code != nullptr &&
      (code->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) == 0 &&
      (code->co_flags & CI_CO_STATICALLY_COMPILED) == 0 &&
      code->co_kwonlyargcount == 0;
}

struct JitToJitCallSite {
  uintptr_t address;
  size_t nargs;
};

// Data members extracted from CompiledFunction to enable separate storage.
// CompiledFunction is a GC tracked object and cannot be constructed during
// multi-threaded compile because we can't hold the GIL which is required for
// allocating Python memory. This struct is used to pass around the data before
// we get the full object constructed.
struct CompiledFunctionData {
  // If the resulting CompiledFunction object is immortal, then this data object
  // will be reallocated inline to the CompiledFunction and immortalized too.
  //
  // If the CompiledFunction is not immortal, then this data object will be
  // refcounted and have its lifetime managed by the Python runtime.  This is
  // set up at the same time the CompiledFunction is created.
  PyObject_HEAD
  // The generated machine code for the code object.  Memory is owned by the
  // CodeAllocator.
  std::span<const std::byte> code;
  // Entry point for calling this code object with the vectorcall API.
  vectorcallfunc vectorcall_entry{nullptr};
  // The total stack size used by the code object.
  int stack_size{0};
  // The amount of stack used for spilled values.
  int spill_stack_size{0};
  // Time taken to compile this code object.  Doesn't include preloading time.
  std::chrono::nanoseconds compile_time{};
  // Stats about the functions inlined into this code object.
  hir::Function::InlineFunctionStats inline_function_stats;
  // Counts of the HIR opcodes emitted while compiling this code object.
  hir::OpcodeCounts hir_opcode_counts{};
  // All the code patchers pointing to patch points in this function.
  std::vector<std::unique_ptr<CodePatcher>> code_patchers;
#ifndef ENABLE_PREFORK_MODEL
  // Stable-address objects referenced by this generated code. Keeping them
  // with the compiled data preserves them during deferred code destruction.
  std::unique_ptr<PerCompilationStableStorage> stable_storage;
#endif
  std::unique_ptr<ProfileCollector> profile_collector;
  // The HIR representation of this code object.  Optional, only used when debug
  // logging.
  std::unique_ptr<hir::Function> irfunc;
  // Runtime state referenced by the generated code. In prefork builds it is
  // owned by the JIT Context's stable storage; otherwise it is owned by
  // stable_storage above.
  CodeRuntime* runtime{nullptr};
  // References collected during codegen that `runtime` does not own yet.
  // A compile detached from the GIL defers owning them until publication,
  // where the GIL is already held, instead of acquiring it at the end of
  // compilation. Borrowed entries stay alive until transfer through the
  // preloader, whose manager outlives publication for a detached worker
  // compile. A compile that never reaches publication drops them unowned.
  std::vector<BorrowedRef<>> pending_references;
  BorrowedRef<> pending_reifier;
  std::unordered_set<Ref<>> pending_strong_references;

  // Own every pending reference in `runtime`. The GIL must be held. A
  // no-op once the pending references have been transferred.
  void transferPendingReferences();
  // Attached only after the data reaches its final allocation. Published data
  // is not moved; this owner outlives the CompiledFunction wrapper.
  CompiledFunctionOwner* owner{nullptr};
  bool retired{false};

  // Whether this function was compiled with a register (native ABI) entry point
  // at JITRT_STATIC_ENTRY_OFFSET from `vectorcall_entry`.  True for Static
  // Python functions and for JIT-to-JIT call targets.
  bool has_register_entry{false};

  // True if this function is a JIT-to-JIT call target whose register entry may
  // be the destination of patched direct calls from other compiled functions.
  // Such code cannot be freed eagerly; it is reclaimed by the stale-code GC.
  bool jit_to_jit_target{false};

  // Absolute addresses of the JIT-to-JIT call sites emitted in this function's
  // code.  Populated once at finalize and never mutated afterwards; the stale
  // code GC scans these (under stop-the-world) to find direct calls into code
  // it is about to free.
  FrozenList<JitToJitCallSite> jit_to_jit_call_sites;
  bool jit_to_jit_entry_diverted{false};
  bool jit_to_jit_registered{false};
  CompiledFunctionOwner* jit_to_jit_owner{nullptr};

  bool participatesInJitToJitCalls() const {
    return jit_to_jit_target || jit_to_jit_call_sites.size() != 0;
  }

  const JitToJitCallSite* findJitToJitCallSite(uintptr_t address) const;

  // The far-relink stub emitted in this function's code (or nullptr if it has
  // no JIT-to-JIT register entry).  When the register entry goes stale it is
  // patched to branch here, and the stub jumps to the relink trampoline -- so
  // the patch's relative branch always reaches a valid target.
  void* jit_to_jit_far_relink_stub{nullptr};

  // Reverse index for O(callers) stale-code relinking.  These form a bipartite
  // graph over compiled blobs and are the exact mirror of each other:
  //   A ∈ callee->callers  <=>  callee ∈ A->callees
  // Populated by the link shim when it patches a call site to a direct call,
  // and torn down when either endpoint's code is freed.  Bare pointers, so they
  // never keep a CompiledFunctionData alive.  All access is guarded by the JIT
  // Context's reverse-index mutex (or stop-the-world during reclamation).
  //
  // Distinct caller blobs that currently have >=1 direct call into this
  // function's register entry.
  std::unordered_set<CompiledFunctionData*> callers;
  // Distinct callee blobs this function currently has >=1 direct call into.
  std::unordered_set<CompiledFunctionData*> callees;

  CompiledFunctionData() = default;
  CompiledFunctionData(CompiledFunctionData&&) = default;
  CompiledFunctionData& operator=(CompiledFunctionData&&) = default;
  CompiledFunctionData(const CompiledFunctionData&) = delete;
  CompiledFunctionData& operator=(const CompiledFunctionData&) = delete;

  // Unwind info for the code range is owned by the code allocator
  // that allocated it, which deregisters it when the code is released.
  ~CompiledFunctionData();

  [[nodiscard]] bool defer();
  void releaseRuntimeResources();
  void releaseRuntimeReferences();
};

class CompiledFunction;
class NestedCompileData;

// Interface for the owner of a CompiledFunction.  The owner holds the
// bookkeeping that maps Python functions and compilation keys to their
// CompiledFunction, plus a type-watcher registry, and is notified as a
// CompiledFunction is torn down.
//
// Currently implemented by jit::Context.
class CompiledFunctionOwner {
 public:
  virtual ~CompiledFunctionOwner() = default;

  virtual void retireJitToJitCode(CompiledFunctionData*) {}

  // Notify the owner that a CompiledFunction is about to be destroyed.
  virtual void forgetCompiledFunction(CompiledFunction& function) = 0;

  // Unwatch a single TypeDeoptPatcher from a CompiledFunction.  The watched
  // Python type itself is left watched for any other patchers.
  virtual void unwatch(TypeDeoptPatcher* patcher) = 0;

  // Unwatch a single FuncCodeDeoptPatcher from a CompiledFunction. The
  // watched function itself is left watched for any other patchers.
  virtual void unwatchFunc(FuncCodeDeoptPatcher* patcher) = 0;

  // Hand off a CompiledFunctionData for deferred destruction instead of freeing
  // it inline, since the machine code may still be executing on some thread's
  // stack.  The (code, builtins, globals) triple keys the deferred entry and
  // keeps the compilation data alive until cleanup determines no thread is
  // running the code.
  virtual void deferCompiledData(
      Ref<> code,
      Ref<> builtins,
      Ref<> globals,
      CompiledFunctionData* data) = 0;
};

// CompiledFunction is a Python GC object that contains a pointer to the native
// code compiled for a Python function. The memory behind the generated native
// code is in the CodeAllocator.
//
// CompiledFunction is reference counted via Python's GC. The Context holds
// strong references via Ref<CompiledFunction>. Functions also hold references
// via their __dict__.
class CompiledFunction {
 public:
  PyObject_HEAD
  // factory function to create a new CompiledFunction.
  [[nodiscard]] static Ref<CompiledFunction> create(
      CompiledFunctionData&& data,
      CompiledFunctionOwner& owner,
      bool immortal);

  ~CompiledFunction();

  // Get the buffer containing the compiled machine code.  The start of this
  // buffer is not guaranteed to be a valid entry point.
  std::span<const std::byte> codeBuffer() const;

  // Entry point for calling the function via the vectorcall API.
  vectorcallfunc vectorcallEntry() const {
    return data_->vectorcall_entry;
  }

  // Entry point for Static Python calls that skips argument boxing.  Returns
  // nullptr if the function was not statically compiled.
  void* staticEntry() const;

  // Runtime state (code object, globals, builtins) backing this function.
  CodeRuntime* runtime() const {
    return data_->runtime;
  }

  // The JIT-to-JIT register entry point, or nullptr for other entry kinds.
  void* registerEntry() const;

  // Patch the register entry point with a branch to the relink trampoline so
  // existing patched direct calls into this (now stale) function are diverted.
  // No-op unless this is a JIT-to-JIT target with a register entry.
  void patchRegisterEntryForDeopt();

  // Call the compiled code directly through its vectorcall entry point.
  PyObject* invoke(PyObject* func, PyObject** args, Py_ssize_t nargs) const;

  void printHIR() const;
  void disassemble() const;

  // Size of the compiled machine code in bytes.
  size_t codeSize() const;

  // Total stack size used by the compiled code.
  int stackSize() const;

  // Stack size used for spilled values.
  int spillStackSize() const;

  std::chrono::nanoseconds compileTime() const;
  void setCompileTime(std::chrono::nanoseconds time);

  void setCodePatchers(
      std::vector<std::unique_ptr<CodePatcher>>&& code_patchers);

  void setHirFunc(std::unique_ptr<hir::Function>&& irfunc);

  const hir::Function::InlineFunctionStats& inlinedFunctionsStats() const;

  const hir::OpcodeCounts& hirOpcodeCounts() const;

#ifndef ENABLE_PREFORK_MODEL
  const std::vector<InlineCacheSite>& inlineCacheSites() const;
#endif
  Profile snapshotProfile() const;
  // Drop the profile state for a location whose profile guard deopted.
  // Silently ignores missing collectors: deopt must never fail.
  void removeProfileEntry(const ProfileLocation& location);

  // Associate a function with this CompiledFunction and set its vectorcall
  // entry point, the function will keep the compiled code alive.
  void addFunction(BorrowedRef<PyFunctionObject> func);

  // Undo addFunction().  Put the function back on the interpreter entry point
  // and hand back the reference it owned, or null if it wasn't registered.
  [[nodiscard]] Ref<CompiledFunction> stealFunction(
      BorrowedRef<PyFunctionObject> func);

  // Same as stealFunction(), but drops the reference.  This can free the
  // CompiledFunction, so callers must not use it afterwards.
  void removeFunction(BorrowedRef<PyFunctionObject> func);

  // Marks a dunction as being deopted
  void deoptFunction(BorrowedRef<PyFunctionObject> func);

  // Undo deoptFunction(): put an already-registered function back on the
  // compiled entry point.
  void reoptFunction(BorrowedRef<PyFunctionObject> func);

  // Steal the registration of a function that is currently parked by
  // deoptFunction().  Same as stealFunction(), which cannot be used because the
  // function is not on the compiled entry point any more.
  [[nodiscard]] Ref<CompiledFunction> stealDeoptedFunction(
      BorrowedRef<PyFunctionObject> func);

  // Same as stealDeoptedFunction(), but drops the reference.  This can free the
  // CompiledFunction, so callers must not use it afterwards.
  void releaseDeoptedFunction(BorrowedRef<PyFunctionObject> func);

  // How many functions are currently registered, i.e. how many of this object's
  // references are owned by a function.
  size_t numFunctions() const {
    return num_functions_;
  }

  // Hand back one reference per registered function, for tp_clear only.  See
  // the definition for why clear() must not do this.
  void releaseFunctionRefs();

  // Traverse all GC-reachable objects for the GC.
  int traverse(visitproc visit, void* arg);

  // Clear all references held by this CompiledFunction and deopt all
  // associated functions.
  void clear(bool context_finalizing = false);

  // Whether the data object is allocated contiguously with this object, which
  // is the case for immortal CompiledFunction objects.
  bool isContiguous() const;

  CompiledFunctionData* data() const;

  // Transfer ownership of the CompiledFunctionData out of this object.
  // After this call, the CF no longer owns or references the data.
  [[nodiscard]] CompiledFunctionData* stealData();

  CompiledFunction(const CompiledFunction&) = delete;
  CompiledFunction& operator=(const CompiledFunction&) = delete;

 private:
  CompiledFunction(
      CompiledFunctionData* data,
      CompiledFunctionOwner& owner,
      bool contiguous);

  friend Ref<CompiledFunction>;

  CompiledFunctionOwner* owner_{};
  CompiledFunctionData* data_{nullptr};
  bool contiguous_data_{false};
  // How many functions are registered with this CompiledFunction.
  size_t num_functions_{0};
#if Py_DEBUG
  // Debugging helper to verify our function book keeping is correct
  std::unordered_set<BorrowedRef<PyFunctionObject>> functions_;
#endif
};

// Initialize the CompiledFunction key and types. Should be called during
// module initialization.
int initCompiledFunctionType();

BorrowedRef<PyTypeObject> getCompiledFunctionType();

// Whether a tp_traverse may report `compiled` to the collector.
//
// Immortal compiles - the prefork model, and functions that are themselves
// immortal - are allocated in one block with their data by CompiledFunction::
// create() and so have no GC header in front of them.  They can never be freed,
// so the collector does not need to know about them, and visiting one has it
// read a header that isn't there.
inline bool isCollectableCompile(BorrowedRef<CompiledFunction> compiled) {
  return compiled != nullptr && !compiled->isContiguous();
}

} // namespace cinderx::jit

#endif

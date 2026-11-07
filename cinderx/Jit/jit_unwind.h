// Copyright (c) Python Software Foundation under PSF license v2
// // Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

namespace cinderx::jit {

// DWARF .eh_frame generation and registration for JIT-compiled code.
//
// The .eh_frame builder is vendored and adapted from CPython's
// `Python/jit_unwind.c` (CPython main, commit
// 2934fd1085679c2137166a9368ab8fba856a3a89).  It is vendored
// into CinderX rather than borrowed because it must also be used on Python
// versions whose trees do not contain it (3.12, 3.14), and because CinderX
// builds do not define the `_Py_JIT` / pyconfig macros the upstream header
// gates its feature tests on.  No CPython source is modified.
//
// Adaptations from the upstream file:
// - Only the absolute-address flavor is vendored (the one upstream uses for
//   the GDB JIT interface and for `__register_frame`).  The PC-relative
//   "perf" flavor is not: it encodes CPython's own trampoline epilogue
//   (`call *%rcx; pop %rbp`), which CinderX code does not have, and CinderX's
//   perf jitdump writer does not emit unwinding records.
// - The steady-state frame rule is CinderX's, not the constants from
//   CPython's generated `jit_unwind_info.h`: every CinderX entry sequence
//   establishes a frame pointer (`push rbp; mov rbp, rsp` on x86-64,
//   `stp fp, lr, [sp, #-16]!; mov fp, sp` on AArch64; see
//   `generateFunctionEntry` / `translatePrologue`), and the FDE's prologue
//   rows describe exactly that sequence.  The resulting rule
//   (CFA = frame pointer + 16, saved frame pointer at CFA-16, return address
//   at CFA-8) also holds for generator bodies: after the frame-pointer swap
//   the heap-allocated `GenDataFooter` carries `linkAddress` /
//   `returnAddress` ahead of the frame pointer, so it reads as a frame
//   record.  Deopt exit stubs preserve the frame pointer.
// - The emitted size is independent of the output buffer's address (padding
//   is relative to the start of the frame), and a size mismatch fails closed
//   instead of asserting: unwind registration must never fail a compile.
// - No CPython internal headers, `PyMutex`, or `PyMem` allocator are used,
//   so this compiles unchanged against every supported Python version.
// - On architectures other than x86-64/AArch64 the builder generates nothing
//   and registration is a no-op, so all targets keep compiling.
//
// Registration reaches two unwinder families, which consult disjoint
// registries.  Both are fed the same generated frame:
// - libgcc / LLVM unwinders, via `__register_frame` / `__deregister_frame`
//   (the `PY_HAVE_JIT_GNU_BACKTRACE_UNWIND` mechanism in upstream
//   `jit_unwind.c`).  This is what glibc `backtrace()` and `_Unwind_*` use.
// - HP libunwind (tp2 `libunwind` 1.9, used by jemalloc's `unw_backtrace`),
//   via `_U_dyn_register` / `_U_dyn_cancel` with a
//   `UNW_INFO_FORMAT_TABLE` entry.  libunwind's `dwarf_step` checks its
//   dynamic-info list before the ELF program-header scan, and it has no
//   `__register_frame` implementation, so the libgcc registration alone does
//   not keep it off the backward prologue scan that crashed in T296442166.
//   The table/frame-list structures below mirror `libunwind-dynamic.h` and
//   `dwarf.h` field-for-field; they are part of the registration contract
//   libunwind reads.
//
// Registration is Linux/ELF-only, matching upstream's
// `PY_HAVE_JIT_GDB_UNWIND` gating.  The frame symbols are declared weak:
// when neither unwinder is linked into the process, registration returns
// null and JIT code simply runs without unwind info.
//
// The code allocator owns registration for JIT code (see
// `CodeAllocator`): one frame per chunk, or one frame per executable
// code section for individually freed allocations.  A registration must
// be unregistered before the code range or the frame buffer can be
// reused.  Frames registered through `registerPermanentJitUnwindInfo`
// are owned by the registry in `ModuleState`, which unregisters them
// when the module state is torn down.  No narrower frame may overlap a
// registered range: libgcc's FDE lookup picks the frame with the
// greatest start address at or below the PC, so an enclosed frame
// would shadow the enclosing one for PCs past it.

// Size in bytes of the .eh_frame data for one code range, or 0 when no
// unwind info is generated for this target.
size_t jitUnwindEhFrameSize();

// Build the .eh_frame data for `[code_addr, code_addr + code_size)` into
// `buffer`.  Returns the number of bytes written, or 0 on any error
// (null arguments, zero size, or a buffer smaller than
// `jitUnwindEhFrameSize()`).
size_t buildJitUnwindEhFrame(
    uint8_t* buffer,
    size_t buffer_size,
    const void* code_addr,
    size_t code_size);

// One entry of the binary-search index libunwind uses to find an FDE.
// Mirrors `struct table_entry` in libunwind's `Gfind_proc_info_i.h`.
struct JitUnwindTableEntry {
  int32_t start_ip_offset{0};
  int32_t fde_offset{0};
};

// Descriptor for a loaded .eh_frame section.  Mirrors
// `struct unw_debug_frame_list` in libunwind's `dwarf.h`.
struct JitUnwindDebugFrameList {
  uintptr_t start{0};
  uintptr_t end{0};
  char* debug_frame{nullptr};
  size_t debug_frame_size{0};
  JitUnwindTableEntry* index{nullptr};
  size_t index_size{0};
  JitUnwindDebugFrameList* next{nullptr};
};

// Unwind-info payloads for `JitUnwindDynInfo`, mirroring
// `libunwind-dynamic.h`.  Only the table payload is populated by CinderX;
// the others exist so the union has the ABI layout libunwind expects.
struct JitUnwindDynOp {
  int8_t tag{0};
  int8_t qp{0};
  int16_t reg{0};
  int32_t when{0};
  uintptr_t val{0};
};

struct JitUnwindDynRegionInfo {
  JitUnwindDynRegionInfo* next{nullptr};
  int32_t insn_count{0};
  uint32_t op_count{0};
  JitUnwindDynOp op[1];
};

struct JitUnwindDynProcInfo {
  uintptr_t name_ptr{0};
  uintptr_t handler{0};
  uint32_t flags{0};
  int32_t pad0{0};
  JitUnwindDynRegionInfo* regions{nullptr};
};

struct JitUnwindDynTableInfo {
  uintptr_t name_ptr{0};
  uintptr_t segbase{0};
  uintptr_t table_len{0};
  uintptr_t* table_data{nullptr};
};

struct JitUnwindDynRemoteTableInfo {
  uintptr_t name_ptr{0};
  uintptr_t segbase{0};
  uintptr_t table_len{0};
  uintptr_t table_data{0};
};

// Dynamic unwind-info entry, mirroring `unw_dyn_info_t` in
// `libunwind-dynamic.h`.  libunwind reads the list these form, so the
// field order and sizes are load-bearing.
struct JitUnwindDynInfo {
  static constexpr int32_t kFormatTable = 1; // UNW_INFO_FORMAT_TABLE

  JitUnwindDynInfo* next{nullptr};
  JitUnwindDynInfo* prev{nullptr};
  uintptr_t start_ip{0};
  uintptr_t end_ip{0};
  uintptr_t gp{0};
  int32_t format{0};
  int32_t pad{0};
  union {
    JitUnwindDynProcInfo proc;
    JitUnwindDynTableInfo table;
    JitUnwindDynRemoteTableInfo remote_table;
  } u;
};

// Frame buffers are allocated with `calloc` (zeroed past the frame so
// libgcc's frame walker stops at the end of the data) and freed with
// `free`, so they are owned by a `unique_ptr` with a `free` deleter
// rather than by a raw pointer freed by hand in every failure path.
struct EhFrameFreeDeleter {
  void operator()(uint8_t* ptr) const noexcept {
    std::free(ptr);
  }
};
using EhFrameBuffer = std::unique_ptr<uint8_t[], EhFrameFreeDeleter>;

// One registration: the generated frame plus the bookkeeping both
// unwinders need to find and later release it.
// `unregisterJitUnwindInfo` deregisters it; destroying the registration
// (or the owning `unique_ptr`) frees the frame buffer.
struct JitUnwindRegistration {
  EhFrameBuffer eh_frame{};
  size_t eh_frame_size{0};
  bool libgcc_registered{false};
  bool dyn_registered{false};
  JitUnwindDynInfo dyn_info{};
  JitUnwindDebugFrameList frame_list{};
  JitUnwindTableEntry index[1]{};
};

// Registry of permanent unwind registrations, owned by `ModuleState`
// (see `permanent_unwind_registrations`).  Destroying the registry
// unregisters every registration it holds before freeing any frame
// buffer, so no unwinder keeps pointing at a freed frame when the
// module state is torn down at interpreter shutdown.
struct JitUnwindPermanentRegistry {
  ~JitUnwindPermanentRegistry();
  std::mutex mutex;
  std::vector<std::unique_ptr<JitUnwindRegistration>> registrations;
};

// The permanent-registration registry owned by the current module
// state, or null when the module state is not initialized (e.g. direct
// calls before the cinderx module is imported).  Defined in
// module_state.cpp so this header does not depend on `ModuleState`.
JitUnwindPermanentRegistry* jitUnwindPermanentRegistry() noexcept;

// Register .eh_frame info for `[code_addr, code_addr + code_size)` with
// every unwinder linked into the process.  Returns null when unwind
// registration is unsupported, when no unwinder is linked, or on
// allocation failure; JIT compilation must treat null as "no unwind info"
// and keep going.  The returned registration must be passed to
// `unregisterJitUnwindInfo` before the code range is freed or reused.
//
// The libgcc half of the registration (`__register_frame`) is gated by
// the `unwind.libgcc_frame_registration` JIT config flag, which is on
// by default; turning it off disables `__register_frame` while the HP
// libunwind registration is always on.  `libgcc_registered` on the returned
// registration records which half ran, so unregistering stays correct
// when the flag changes between registration and unregistration.
std::unique_ptr<JitUnwindRegistration> registerJitUnwindInfo(
    const void* code_addr,
    size_t code_size);

// Undo `registerJitUnwindInfo` and free the registration.  Passing null is
// a no-op.  Callers must guarantee no thread is executing (or unwinding
// through) the code range; freeing the frame while an unwinder is reading
// it is the same hazard as deregistering any JIT frame.
void unregisterJitUnwindInfo(
    std::unique_ptr<JitUnwindRegistration>&& registration);

// Like `registerJitUnwindInfo`, but the registration is never
// unregistered: for code that is never freed, such as the standalone
// deopt trampolines.  The registration is kept in the registry owned by
// `ModuleState`, so it stays reachable (and valid) until the module
// state is torn down, rather than leaking in the sanitizer sense.
// Returns a non-owning pointer to the registry-owned registration, or
// null on failure; callers must not free it or pass it to
// `unregisterJitUnwindInfo`.
JitUnwindRegistration* registerPermanentJitUnwindInfo(
    const void* code_addr,
    size_t code_size);

} // namespace cinderx::jit

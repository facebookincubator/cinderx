// Copyright (c) Python Software Foundation under PSF license v2
// Copyright (c) Meta Platforms, Inc. and affiliates.

// DWARF .eh_frame builder and unwinder registration for JIT code.
//
// Vendored and adapted from CPython's `Python/jit_unwind.c`; see
// `jit_unwind.h` for the full list of adaptations.  The DWARF generation
// helpers and the CIE/FDE layout below follow the upstream file closely;
// the registration half is CinderX-specific (upstream has no libunwind
// dynamic-info registration).

#include "cinderx/Jit/jit_unwind.h"

#include "cinderx/Common/define.h"
#include "cinderx/Jit/config.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace cinderx::jit {
namespace {

// The CIE/FDE program below only describes targets CinderX emits code for.
// Other targets generate nothing instead of failing the build, as upstream
// does with `#error` only because the JIT itself is target-gated there.
#if defined(__x86_64__) || defined(_M_AMD64)
constexpr int kCodeAlignment = 1;
constexpr int kFpReg = 6; // RBP
constexpr int kSpReg = 7; // RSP
constexpr int kRaReg = 16; // Return address (RIP)
constexpr int kInitialCfaOffset = 8;
#elif defined(__aarch64__) || defined(_M_ARM64)
constexpr int kCodeAlignment = 4;
constexpr int kFpReg = 29; // Frame pointer (x29)
constexpr int kRaReg = 30; // Link register (x30)
constexpr int kSpReg = 31; // Stack pointer
constexpr int kInitialCfaOffset = 0;
#endif

// DWARF Call Frame Information version.
#define DWRF_CIE_VERSION 1

// DWARF CFA opcodes used below.
enum {
  DWRF_CFA_nop = 0x0,
  DWRF_CFA_def_cfa = 0xc,
  DWRF_CFA_def_cfa_register = 0xd,
  DWRF_CFA_def_cfa_offset = 0xe,
  DWRF_CFA_advance_loc = 0x40,
  DWRF_CFA_offset = 0x80,
};

// DWARF exception-handling pointer encodings.  Only the absolute encoding
// is used: the PC-relative encoding belongs to the perf flavor, which is
// intentionally not vendored (see `jit_unwind.h`).
enum {
  DWRF_EH_PE_absptr = 0x00,
};

// Context for building DWARF structures, mirroring upstream's
// `ELFObjectContext` without the fields only the perf flavor needed.
struct EHFrameContext {
  uint8_t* p{nullptr};
  uint8_t* startp{nullptr};
  uintptr_t code_addr{0};
  size_t code_size{0};
};

void appendSleb128(EHFrameContext* ctx, int32_t v) {
  uint8_t* p = ctx->p;
  for (; (uint32_t)(v + 0x40) >= 0x80; v >>= 7) {
    *p++ = (uint8_t)((v & 0x7f) | 0x80);
  }
  *p++ = (uint8_t)(v & 0x7f);
  ctx->p = p;
}

void appendUleb128(EHFrameContext* ctx, uint32_t v) {
  uint8_t* p = ctx->p;
  for (; v >= 0x80; v >>= 7) {
    *p++ = (char)((v & 0x7f) | 0x80);
  }
  *p++ = (char)v;
  ctx->p = p;
}

void appendString(EHFrameContext* ctx, const char* str) {
  uint8_t* p = ctx->p;
  do {
    *p++ = (uint8_t)*str;
  } while (*str++);
  ctx->p = p;
}

// Writes use memcpy rather than upstream's direct pointer casts, so the
// builder does not rely on the output buffer being naturally aligned.
#define DWRF_U8(x) (*p++ = (uint8_t)(x))
#define DWRF_U32(x)                  \
  do {                               \
    uint32_t v_ = (uint32_t)(x);     \
    std::memcpy(p, &v_, sizeof(v_)); \
    p += sizeof(v_);                 \
  } while (0)
#define DWRF_ADDR(x)                 \
  do {                               \
    uintptr_t v_ = (uintptr_t)(x);   \
    std::memcpy(p, &v_, sizeof(v_)); \
    p += sizeof(v_);                 \
  } while (0)
#define DWRF_UV(x) (ctx->p = p, appendUleb128(ctx, (x)), p = ctx->p)
#define DWRF_SV(x) (ctx->p = p, appendSleb128(ctx, (x)), p = ctx->p)
#define DWRF_STR(str) (ctx->p = p, appendString(ctx, (str)), p = ctx->p)

// Pad with DW_CFA_nop to a boundary relative to the start of the frame
// data, not to the buffer's absolute address, so the generated size does
// not depend on where the caller put the buffer.
#define DWRF_ALIGNNOP(s)                         \
  while (((p - ctx->startp) & ((s) - 1)) != 0) { \
    *p++ = DWRF_CFA_nop;                         \
  }

#define DWRF_SECTION(name, stmt)                                               \
  {                                                                            \
    uint32_t* szp_##name = (uint32_t*)p;                                       \
    p += 4;                                                                    \
    stmt;                                                                      \
    uint32_t section_size_##name = (uint32_t)((p - (uint8_t*)szp_##name) - 4); \
    std::memcpy(szp_##name, &section_size_##name, sizeof(uint32_t));           \
  }

// Generate one CIE and one FDE covering the whole code range.
//
// The FDE's program first walks through the entry sequence shared by every
// CinderX function (see `generateFunctionEntry` / `translatePrologue`) and
// then leaves the steady-state rule in force for the rest of the range:
// the frame pointer is pinned for the life of the frame -- including
// generator bodies, whose heap-resident footer reads as a frame record --
// and the stack pointer is only restored by the epilogue.  The epilogue's
// own teardown instructions are therefore covered by the steady-state rule
// rather than by per-PC rows; that window is a few instructions per exit,
// the same approximation upstream makes for its GDB frame.
[[maybe_unused]]
void emitEhFrame(EHFrameContext* ctx) {
  uint8_t* p = ctx->p;
  uint8_t* framep = p;

  DWRF_SECTION(
      CIE, DWRF_U32(0); DWRF_U8(DWRF_CIE_VERSION); DWRF_STR("zR");
      DWRF_UV(kCodeAlignment);
      DWRF_SV(-(int64_t)sizeof(uintptr_t));
      DWRF_U8(kRaReg);
      DWRF_UV(1);
      DWRF_U8(DWRF_EH_PE_absptr);
      DWRF_U8(DWRF_CFA_def_cfa);
      DWRF_UV(kSpReg);
      DWRF_UV(kInitialCfaOffset);
      if constexpr (kBuildArch == Arch::kX86_64) {
        DWRF_U8(DWRF_CFA_offset | kRaReg);
        DWRF_UV(1);
      } DWRF_ALIGNNOP(sizeof(uintptr_t));)

  DWRF_SECTION(
      FDE, DWRF_U32((uint32_t)(p - framep)); DWRF_ADDR(ctx->code_addr);
      DWRF_ADDR((uintptr_t)ctx->code_size);
      DWRF_U8(0);
      if constexpr (kBuildArch == Arch::kX86_64) {
        // push %rbp (1 byte); CFA = SP + 16, saved RBP at CFA-16.
        DWRF_U8(DWRF_CFA_advance_loc | 1);
        DWRF_U8(DWRF_CFA_def_cfa_offset);
        DWRF_UV(16);
        DWRF_U8(DWRF_CFA_offset | kFpReg);
        DWRF_UV(2);
        // mov %rsp, %rbp (3 bytes); CFA = RBP + 16.
        DWRF_U8(DWRF_CFA_advance_loc | 3);
        DWRF_U8(DWRF_CFA_def_cfa_register);
        DWRF_UV(kFpReg);
      } else {
        // stp fp, lr, [sp, #-16]! (1 instruction); CFA = SP + 16, saved
        // FP at CFA-16, saved LR at CFA-8.
        DWRF_U8(DWRF_CFA_advance_loc | 1);
        DWRF_U8(DWRF_CFA_def_cfa_offset);
        DWRF_UV(16);
        DWRF_U8(DWRF_CFA_offset | kFpReg);
        DWRF_UV(2);
        DWRF_U8(DWRF_CFA_offset | kRaReg);
        DWRF_UV(1);
        // mov fp, sp (1 instruction); CFA = FP + 16.
        DWRF_U8(DWRF_CFA_advance_loc | 1);
        DWRF_U8(DWRF_CFA_def_cfa_register);
        DWRF_UV(kFpReg);
      } DWRF_ALIGNNOP(sizeof(uintptr_t));)

  ctx->p = p;
}

// Offset of the FDE within a generated frame, read back from the CIE
// length field.  Returns -1 when the buffer does not hold a frame this
// builder produced.
[[maybe_unused]] int32_t ehFrameFdeOffset(
    const uint8_t* eh_frame,
    size_t size) {
  if (eh_frame == nullptr || size < 8) {
    return -1;
  }
  uint32_t cie_length;
  std::memcpy(&cie_length, eh_frame, sizeof(cie_length));
  uint64_t fde_offset = 4ULL + cie_length;
  if (fde_offset > INT32_MAX || fde_offset + 4 > size) {
    return -1;
  }
  return (int32_t)fde_offset;
}

} // namespace

size_t jitUnwindEhFrameSize() {
  // The generated frame has fixed-width fields, so its size is independent
  // of the code address and size.  Generate once into scratch to measure
  // it, as upstream does.
  alignas(8) uint8_t scratch[256];
  EHFrameContext ctx;
  ctx.code_size = 1;
  ctx.code_addr = 0;
  ctx.startp = ctx.p = scratch;
  emitEhFrame(&ctx);
  ptrdiff_t size = ctx.p - ctx.startp;
  assert(size > 0 && size <= (ptrdiff_t)sizeof(scratch));
  return (size_t)size;
}

size_t buildJitUnwindEhFrame(
    uint8_t* buffer,
    size_t buffer_size,
    const void* code_addr,
    size_t code_size) {
  if (buffer == nullptr || code_addr == nullptr || code_size == 0) {
    return 0;
  }
  size_t required = jitUnwindEhFrameSize();
  if (required == 0 || required > buffer_size) {
    return 0;
  }
  EHFrameContext ctx;
  ctx.code_size = code_size;
  ctx.code_addr = (uintptr_t)code_addr;
  ctx.startp = ctx.p = buffer;
  emitEhFrame(&ctx);
  size_t written = (size_t)(ctx.p - ctx.startp);
  if (written != required) {
    return 0;
  }
  return written;
}

// The structures libunwind reads have the layout the header declares;
// verify the ABI contract here rather than trusting it at unwind time.
static_assert(
    sizeof(JitUnwindTableEntry) == 8,
    "JitUnwindTableEntry must match libunwind's unw_dyn_info_t layout");
static_assert(
    sizeof(JitUnwindDebugFrameList) == 7 * sizeof(uintptr_t),
    "JitUnwindDebugFrameList must match libunwind's unw_dyn_info_t layout");
static_assert(
    sizeof(JitUnwindDynTableInfo) == 4 * sizeof(uintptr_t),
    "JitUnwindDynTableInfo must match libunwind's unw_dyn_info_t layout");
static_assert(
    sizeof(JitUnwindDynInfo) == 9 * sizeof(uintptr_t) + 8,
    "JitUnwindDynInfo must match libunwind's unw_dyn_info_t layout");

#if defined(__linux__) && defined(__ELF__)
#define JIT_UNWIND_REGISTRATION_SUPPORTED 1
#else
#define JIT_UNWIND_REGISTRATION_SUPPORTED 0
#endif

#if JIT_UNWIND_REGISTRATION_SUPPORTED
// Frame registration entry points.  libgcc exposes its pair without a
// public header on some distributions, as upstream notes; the libunwind
// pair is declared in `libunwind-dynamic.h`, which CinderX does not
// depend on.  All four are weak so a process that links neither unwinder
// still runs: registration then reports "no unwind info" instead of
// failing to link.
extern "C" {
#if defined(__GNUC__) || defined(__clang__)
void __register_frame(const void*) __attribute__((weak));
void __deregister_frame(const void*) __attribute__((weak));
void _U_dyn_register(JitUnwindDynInfo*) __attribute__((weak));
void _U_dyn_cancel(JitUnwindDynInfo*) __attribute__((weak));
#else
void __register_frame(const void*);
void __deregister_frame(const void*);
void _U_dyn_register(JitUnwindDynInfo*);
void _U_dyn_cancel(JitUnwindDynInfo*);
#endif
} // extern "C"

namespace {

// Name libunwind reports for the table.  It lives in static storage:
// libunwind only reads it for diagnostics, but it must outlive every
// registration that points at it.
constexpr const char* kDynTableName = "cinderx-jit";

} // namespace
#endif

std::unique_ptr<JitUnwindRegistration> registerJitUnwindInfo(
    const void* code_addr,
    size_t code_size) {
  if (code_addr == nullptr || code_size == 0) {
    return nullptr;
  }
#if !JIT_UNWIND_REGISTRATION_SUPPORTED
  return nullptr;
#else
  uintptr_t start = (uintptr_t)code_addr;
  if (code_size > UINTPTR_MAX - start) {
    return nullptr;
  }
  size_t eh_frame_size = jitUnwindEhFrameSize();
  if (eh_frame_size == 0) {
    return nullptr;
  }

  std::unique_ptr<JitUnwindRegistration> registration(
      new (std::nothrow) JitUnwindRegistration());
  if (registration == nullptr) {
    return nullptr;
  }
  // Zeroed past the frame so libgcc's frame walker, which scans for a
  // zero-length terminator entry, stops at the end of the generated data.
  // Owned by the registration from here on; every failure return below
  // frees it through the buffer's deleter.
  EhFrameBuffer eh_frame{
      static_cast<uint8_t*>(std::calloc(1, eh_frame_size + sizeof(uint32_t)))};
  if (eh_frame == nullptr) {
    return nullptr;
  }
  if (buildJitUnwindEhFrame(
          eh_frame.get(), eh_frame_size, code_addr, code_size) !=
      eh_frame_size) {
    return nullptr;
  }
  int32_t fde_offset = ehFrameFdeOffset(eh_frame.get(), eh_frame_size);
  if (fde_offset < 0) {
    return nullptr;
  }

  registration->eh_frame = std::move(eh_frame);
  registration->eh_frame_size = eh_frame_size;

  // Dynamic-info entry for libunwind.  The single table entry maps the
  // start of the range to the FDE; `segbase` is the range start, so the
  // entry's start offset is 0.
  JitUnwindDynInfo& dyn = registration->dyn_info;
  dyn.start_ip = start;
  dyn.end_ip = start + code_size;
  dyn.gp = 0;
  dyn.format = JitUnwindDynInfo::kFormatTable;
  dyn.u.table.name_ptr = (uintptr_t)kDynTableName;
  dyn.u.table.segbase = start;
  dyn.u.table.table_len = sizeof(JitUnwindDebugFrameList) / sizeof(uintptr_t);
  dyn.u.table.table_data = (uintptr_t*)&registration->frame_list;

  JitUnwindDebugFrameList& frame_list = registration->frame_list;
  frame_list.start = start;
  frame_list.end = start + code_size;
  frame_list.debug_frame =
      reinterpret_cast<char*>(registration->eh_frame.get());
  frame_list.debug_frame_size = eh_frame_size;
  frame_list.index = registration->index;
  frame_list.index_size = sizeof(JitUnwindTableEntry);
  frame_list.next = nullptr;
  registration->index[0].start_ip_offset = 0;
  registration->index[0].fde_offset = fde_offset;

  // libgcc's frame registry (for C++ exceptions and glibc `backtrace()`)
  // is gated by `unwind.libgcc_frame_registration` (off by default): on
  // libgcc older than GCC 13 every C++ exception or `backtrace()` frame
  // lookup takes a global mutex and walks every registered frame.  HP
  // libunwind registration below is unaffected.  Read the flag once per
  // registration; already-registered frames are unaffected when it
  // changes, because `unregisterJitUnwindInfo` keys off
  // `libgcc_registered`.
  if (getConfig().unwind.libgcc_frame_registration &&
      &__register_frame != nullptr) {
    __register_frame(registration->eh_frame.get());
    registration->libgcc_registered = true;
  }
  if (&_U_dyn_register != nullptr) {
    _U_dyn_register(&dyn);
    registration->dyn_registered = true;
  }
  if (!registration->libgcc_registered && !registration->dyn_registered) {
    // No unwinder took the frame; returning null destroys the
    // registration and frees the frame buffer through its deleter.
    return nullptr;
  }
  return registration;
#endif
}

JitUnwindRegistration* registerPermanentJitUnwindInfo(
    const void* code_addr,
    size_t code_size) {
  std::unique_ptr<JitUnwindRegistration> registration =
      registerJitUnwindInfo(code_addr, code_size);
  if (registration == nullptr) {
    return nullptr;
  }
  JitUnwindPermanentRegistry* registry = jitUnwindPermanentRegistry();
  if (registry == nullptr) {
    // No module state: there is nowhere to root the registration, so
    // unregister it instead of leaking a frame that can never be
    // released.  Trampoline creation already requires module state.
    unregisterJitUnwindInfo(std::move(registration));
    return nullptr;
  }
  // The registry owns the registration; return a non-owning pointer to
  // it.  It stays valid until the module state is torn down, so
  // callers must not free it or pass it to `unregisterJitUnwindInfo`.
  JitUnwindRegistration* result = registration.get();
  std::lock_guard<std::mutex> lock{registry->mutex};
  registry->registrations.push_back(std::move(registration));
  return result;
}

void unregisterJitUnwindInfo(
    std::unique_ptr<JitUnwindRegistration>&& registration) {
  if (registration == nullptr) {
    return;
  }
#if JIT_UNWIND_REGISTRATION_SUPPORTED
  if (registration->dyn_registered && &_U_dyn_cancel != nullptr) {
    _U_dyn_cancel(&registration->dyn_info);
  }
  if (registration->libgcc_registered && &__deregister_frame != nullptr) {
    __deregister_frame(registration->eh_frame.get());
  }
#endif
  // The frame buffer is freed when `registration` goes out of scope.
}

JitUnwindPermanentRegistry::~JitUnwindPermanentRegistry() {
  // Deregister every frame before any frame buffer is freed: after
  // this, no unwinder may observe a registration whose buffer is gone.
  // Trampoline code is never freed, so this runs only when the module
  // state is torn down, after JIT finalization.  Hold the mutex so a
  // concurrent registration cannot add to the list mid-drain.
  std::lock_guard<std::mutex> lock{mutex};
  for (std::unique_ptr<JitUnwindRegistration>& registration : registrations) {
    unregisterJitUnwindInfo(std::move(registration));
  }
  registrations.clear();
}

} // namespace cinderx::jit

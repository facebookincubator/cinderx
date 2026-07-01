// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/tls.h"

#include "cinderx/python.h"

#include "internal/pycore_pystate.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/codegen/arch/detection.h"
#include "cinderx/module_state.h"

namespace cinderx::jit::codegen {

namespace {

#if defined(CINDER_X86_64) && defined(__linux__)
// __tls_get_addr is provided by the dynamic loader and has no public header.
extern "C" void* __tls_get_addr(void* tls_index);

// When libpython is a shared library, `_PyThreadState_GetCurrent` is compiled
// with the general-dynamic TLS model and looks like:
//   push %rbp; mov %rsp,%rbp
//   lea  disp32(%rip), %rdi        # &tls_index
//   call __tls_get_addr
//   mov  disp32(%rax), %rax        # tstate = *(tls_block + disp)
// __tls_get_addr returns the per-thread address of libpython's TLS block. For a
// module loaded at startup that block sits at a fixed offset from the thread
// pointer, so we resolve a thread-pointer-relative offset once here and let the
// JIT emit direct %fs loads instead of calling __tls_get_addr on every tstate
// access. Returns the offset, or -1 if the function doesn't match the expected
// shape or the resolved offset fails verification.
int32_t decodeGeneralDynamicTstateOffset(const uint8_t* ts_func) {
  const bool matches = ts_func[0] == 0x55 && // push %rbp
      ts_func[1] == 0x48 && ts_func[2] == 0x89 &&
      ts_func[3] == 0xe5 && // mov %rsp,%rbp
      ts_func[4] == 0x48 && ts_func[5] == 0x8d &&
      ts_func[6] == 0x3d && // lea disp32(%rip),%rdi (bytes 4-10)
      ts_func[11] == 0xe8 && // call rel32 (bytes 11-15)
      ts_func[16] == 0x48 && ts_func[17] == 0x8b &&
      ts_func[18] == 0x80; // mov disp32(%rax),%rax (bytes 16-22)
  if (!matches) {
    return -1;
  }

  // The lea is RIP-relative to the following instruction, the call at
  // ts_func + 11.
  const int32_t lea_disp = *reinterpret_cast<const int32_t*>(ts_func + 7);
  void* tls_index = const_cast<uint8_t*>(ts_func) + 11 + lea_disp;
  const int32_t member_disp = *reinterpret_cast<const int32_t*>(ts_func + 19);

  const uintptr_t tls_block =
      reinterpret_cast<uintptr_t>(__tls_get_addr(tls_index));

  // On x86-64 the %fs base is the thread pointer, and %fs:0 holds its value.
  uintptr_t thread_ptr;
  asm volatile("mov %%fs:0, %0" : "=r"(thread_ptr));

  const int32_t offset =
      static_cast<int32_t>(tls_block - thread_ptr) + member_disp;

  // Verify the computed offset reads back the real tstate before trusting it.
  PyThreadState* from_offset =
      *reinterpret_cast<PyThreadState**>(thread_ptr + offset);
  return from_offset == _PyThreadState_GetCurrent() ? offset : -1;
}
#endif

} // namespace

void initThreadStateOffset() {
  auto module_state = cinderx::getModuleState();
  if (module_state->tstate_offset_inited) {
    return;
  }

  // The repetitive single byte checks here are ugly but they guarantee
  // that we're not reading unsafe memory. If we just tried to do a big
  // comparison we might encounter assembly that ends, but as long as
  // we keep seeing our pattern we know that the function is correct.
#if defined(CINDER_X86_64)
  uint8_t* ts_func = reinterpret_cast<uint8_t*>(&_PyThreadState_GetCurrent);

  if (ts_func[0] == 0x55 && // push rbp
      ts_func[1] == 0x48 && ts_func[2] == 0x89 &&
      ts_func[3] == 0xe5 && // mov rsp, rbp
      ts_func[4] == 0x64 && ts_func[5] == 0x48 && ts_func[6] == 0x8b &&
      ts_func[7] == 0x04 && ts_func[8] == 0x25) { // movq   %fs:OFFSET, %rax
    module_state->tstate_offset = *reinterpret_cast<int32_t*>(ts_func + 9);
  }
#ifdef __linux__
  // General-dynamic TLS fallback (the form used when libpython is a shared
  // library), so JIT code can load tstate with a direct %fs access rather than
  // calling _PyThreadState_GetCurrent / __tls_get_addr on every use.
  if (module_state->tstate_offset == -1) {
    module_state->tstate_offset = decodeGeneralDynamicTstateOffset(ts_func);
  }
#endif
#elif defined(CINDER_AARCH64)
  uint32_t* ts_func = reinterpret_cast<uint32_t*>(&_PyThreadState_GetCurrent);

  if (ts_func[0] == 0xa9bf7bfd && // stp x29, x30, [sp, #-16]!
      ts_func[1] == 0x910003fd && // mov x29, sp
      ((ts_func[2] & ~0x1f) == 0xd53bd040) // mrs x?, tpidr_el0
  ) {
    uint32_t reg = ts_func[2] & 0x1f;
    int32_t current_offset = 0;

    for (size_t index = 3;; index++) {
      if (ts_func[index] == (0xf9400000 | (reg << 5))) {
        // ldr x0, [x?] - done
        break;
      } else if (
          (ts_func[index] & ~0x7ffc00) == (0x91000000 | (reg << 5) | reg)) {
        // add x?, x?, #<imm>{, <shift>}
        uint32_t imm = (ts_func[index] >> 10) & 0xfff;
        if (ts_func[index] & (1 << 22)) {
          imm <<= 12;
        }
        current_offset += imm;
      } else {
        current_offset = -1;
        break;
      }
    }

    module_state->tstate_offset = current_offset;
  }

#ifdef __linux__
  // TLSDESC fallback: if the initial-exec pattern didn't match, check for
  // a TLSDESC sequence. When the descriptor is already resolved its
  // resolver is _dl_tlsdesc_return which just returns desc[1] — the TLS
  // offset we need.
  if (module_state->tstate_offset == -1 &&
      ts_func[0] == 0xa9bf7bfd && // stp x29, x30, [sp, #-16]!
      ts_func[1] == 0x910003fd && // mov x29, sp
      (ts_func[2] & 0x9f00001f) == 0x90000000 && // adrp x0, #imm
      (ts_func[3] & 0xffc003ff) == 0xf9400001 && // ldr x1, [x0, #imm]
      (ts_func[4] & 0xffc003ff) == 0x91000000 && // add x0, x0, #imm
      ts_func[5] == 0xd63f0020) // blr x1
  {
    // Decode adrp x0, #imm → page address
    uint32_t adrp = ts_func[2];
    int64_t immlo = (adrp >> 29) & 0x3;
    int64_t immhi = (adrp >> 5) & 0x7ffff;
    int64_t page_off = ((immhi << 2) | immlo) << 12;
    // Sign-extend from 33 bits (21-bit imm << 12)
    if (page_off & (1LL << 32)) {
      page_off |= ~((1LL << 33) - 1);
    }
    uintptr_t adrp_pc = reinterpret_cast<uintptr_t>(&ts_func[2]);
    uintptr_t page_base = (adrp_pc & ~0xfffULL) + page_off;

    // Decode add x0, x0, #imm → lo12 byte offset
    uint32_t add_insn = ts_func[4];
    uint32_t lo12 = (add_insn >> 10) & 0xfff;
    if (add_insn & (1 << 22)) {
      lo12 <<= 12;
    }

    // descriptor[0] = resolver, descriptor[1] = arg (the TLS offset)
    auto* desc = reinterpret_cast<uintptr_t*>(page_base + lo12);
    int32_t current_offset = static_cast<int32_t>(desc[1]);

    // Verify the offset is correct by comparing against the actual function.
    uintptr_t tp;
    asm volatile("mrs %0, tpidr_el0" : "=r"(tp));
    auto* from_offset = *reinterpret_cast<PyThreadState**>(tp + current_offset);
    if (from_offset == _PyThreadState_GetCurrent()) {
      module_state->tstate_offset = current_offset;
    }
  }
#endif

#else
  CINDER_UNSUPPORTED
#endif

  module_state->tstate_offset_inited = true;
}
} // namespace cinderx::jit::codegen

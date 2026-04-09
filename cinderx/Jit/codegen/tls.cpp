// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/tls.h"

#include "cinderx/python.h"

#include "internal/pycore_pystate.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/codegen/arch/detection.h"
#include "cinderx/module_state.h"

namespace jit::codegen {

void initThreadStateOffset() {
#if PY_VERSION_HEX >= 0x030C0000
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
#elif defined(CINDER_AARCH64)
  uint32_t* ts_func = reinterpret_cast<uint32_t*>(&_PyThreadState_GetCurrent);

  if (ts_func[0] == 0xa9bf7bfd && // stp x29, x30, [sp, #-16]!
      ts_func[1] == 0x910003fd && // mov x29, sp
      ((ts_func[2] & ~0x1f) == 0xd53bd048) // mrs x?, tpidr_el0
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
#else
  CINDER_UNSUPPORTED
#endif

  module_state->tstate_offset_inited = true;
#endif // PY_VERSION_HEX >= 0x030C0000
}
} // namespace jit::codegen

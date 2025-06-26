// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/frame_asm.h"

#include <Python.h>

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/register_preserver.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/Jit/jit_rt.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "internal/pycore_pystate.h"
#include "internal/pycore_shadow_frame.h"
#endif

using namespace asmjit;
using namespace jit::hir;

// Use a special define to keep it clear why much code changes in 3.12+
#if PY_VERSION_HEX < 0x030C0000
#define SHADOW_FRAMES 1
#endif

namespace jit::codegen {

#ifdef SHADOW_FRAMES

namespace shadow_frame {
// Shadow stack frames appear at the beginning of native frames for jitted
// functions
constexpr x86::Mem kFramePtr = x86::ptr(x86::rbp, -kJITShadowFrameSize);
constexpr x86::Mem kInFramePrevPtr =
    x86::ptr(x86::rbp, -kJITShadowFrameSize + SHADOW_FRAME_FIELD_OFF(prev));
constexpr x86::Mem kInFrameDataPtr =
    x86::ptr(x86::rbp, -kJITShadowFrameSize + SHADOW_FRAME_FIELD_OFF(data));
constexpr x86::Mem kInFrameOrigDataPtr = x86::ptr(
    x86::rbp,
    -kJITShadowFrameSize + JIT_SHADOW_FRAME_FIELD_OFF(orig_data));

constexpr x86::Mem getStackTopPtr(x86::Gp tstate_reg) {
  return x86::ptr(tstate_reg, offsetof(PyThreadState, shadow_frame));
}

} // namespace shadow_frame

#endif // SHADOW_FRAMES

#if PY_VERSION_HEX < 0x030C0000
void FrameAsm::loadTState(x86::Gp dst_reg) {
  uint64_t tstate =
      reinterpret_cast<uint64_t>(&_PyRuntime.gilstate.tstate_current);
  if (fitsInt32(tstate)) {
    as_->mov(dst_reg, x86::ptr(tstate));
  } else {
    as_->mov(dst_reg, tstate);
    as_->mov(dst_reg, x86::ptr(dst_reg));
  }
}

void FrameAsm::generateLinkFrame(
    const asmjit::x86::Gp& func_reg,
    const asmjit::x86::Gp& tstate_reg,
    const std::vector<
        std::pair<const asmjit::x86::Reg&, const asmjit::x86::Reg&>>&
        save_regs) {
  auto load_tstate_and_move = [&]() {
    loadTState(tstate_reg);
    for (const auto& pair : save_regs) {
      if (pair.first != pair.second) {
        if (pair.first.isGpq()) {
          JIT_DCHECK(pair.second.isGpq(), "can't mix and match register types");
          as_->mov(
              static_cast<const asmjit::x86::Gpq&>(pair.second),
              static_cast<const asmjit::x86::Gpq&>(pair.first));
        } else if (pair.first.isXmm()) {
          JIT_DCHECK(pair.second.isXmm(), "can't mix and match register types");
          as_->movsd(
              static_cast<const asmjit::x86::Xmm&>(pair.second),
              static_cast<const asmjit::x86::Xmm&>(pair.first));
        }
      }
    }
  };

  // Prior to 3.12 we did not link a frame on initial generator entry.
  if (isGen()) {
    load_tstate_and_move();
    return;
  }

  switch (GetFunction()->frameMode) {
    case FrameMode::kShadow:
      load_tstate_and_move();
      break;
    case FrameMode::kNormal: {
      RegisterPreserver preserver(as_, save_regs);
      preserver.preserve();

      as_->mov(
          x86::rdi,
          reinterpret_cast<intptr_t>(
              codeRuntime()->frameState()->code().get()));
      as_->mov(
          x86::rsi,
          reinterpret_cast<intptr_t>(
              codeRuntime()->frameState()->builtins().get()));
      as_->mov(
          x86::rdx,
          reinterpret_cast<intptr_t>(
              codeRuntime()->frameState()->globals().get()));

      as_->call(reinterpret_cast<uint64_t>(JITRT_AllocateAndLinkFrame));

      as_->mov(tstate_reg, x86::rax);

      preserver.restore();
      break;
    }
  }
}
#else
void FrameAsm::generateLinkFrame(
    const x86::Gp& func_reg,
    const x86::Gp& tstate_reg,
    const std::vector<std::pair<const x86::Reg&, const x86::Reg&>>& save_regs) {
  JIT_CHECK(
      GetFunction()->frameMode == FrameMode::kNormal,
      "3.12 only has normal frames");

  RegisterPreserver preserver(as_, save_regs);
  preserver.preserve();

  JIT_DCHECK(func_reg == x86::rdi, "func_reg must be rdi");
  if (isGen()) {
    uint64_t full_words = env_.shadow_frames_and_spill_size / kPointerSize;
    as_->mov(x86::rsi, full_words);
    as_->mov(x86::rdx, reinterpret_cast<intptr_t>(codeRuntime()));
    as_->lea(x86::rcx, x86::ptr(env_.gen_resume_entry_label));
    as_->mov(x86::r8, x86::rbp);
    as_->call(reinterpret_cast<uint64_t>(
        JITRT_AllocateAndLinkGenAndInterpreterFrame));
    // tstate is now in RAX and GenDataFooter* in RDX. Swap RBP over to the
    // generator data so spilled data starts getting stored there. There
    // shouldn't have been any other data stored in the spilled area so far
    // so no need to copy things over.
    as_->mov(x86::rbp, x86::rdx);
  } else {
    if (kPyDebug) {
      as_->mov(x86::rsi, reinterpret_cast<intptr_t>(GetFunction()->code.get()));
      as_->call(reinterpret_cast<uint64_t>(
          JITRT_AllocateAndLinkInterpreterFrame_Debug));
    } else {
      as_->call(reinterpret_cast<uint64_t>(
          JITRT_AllocateAndLinkInterpreterFrame_Release));
    }
  }
  as_->mov(tstate_reg, x86::rax);

  preserver.restore();
}
#endif

void FrameAsm::generateUnlinkFrame(
    const x86::Gp& tstate_r,
    [[maybe_unused]] bool is_generator) {
#ifdef SHADOW_FRAMES
  // It's safe to use caller saved registers in this function
  auto scratch_reg = tstate_r == x86::rsi ? x86::rdx : x86::rsi;
  x86::Mem shadow_stack_top_ptr = shadow_frame::getStackTopPtr(tstate_r);

  // Check bit 0 of _PyShadowFrame::data to see if a frame needs
  // unlinking. This bit will be set (pointer kind == PYSF_PYFRAME) if so.
  // scratch_reg = tstate->shadow_frame
  as_->mov(scratch_reg, shadow_stack_top_ptr);
  static_assert(
      PYSF_PYFRAME == 1 && _PyShadowFrame_NumPtrKindBits == 2,
      "Unexpected constants");
  bool might_have_heap_frame =
      func_->canDeopt() || func_->frameMode == FrameMode::kNormal;
  if (might_have_heap_frame) {
    as_->bt(
        x86::qword_ptr(scratch_reg, offsetof(_PyShadowFrame, data)),
        _PyShadowFrame_PtrKindOff);
  }

  // Unlink shadow frame. The send implementation handles unlinking these for
  // generators.
  if (!is_generator) {
    // tstate->shadow_frame = ((_PyShadowFrame*)scratch_reg)->prev
    as_->mov(
        scratch_reg,
        x86::qword_ptr(scratch_reg, offsetof(_PyShadowFrame, prev)));
    as_->mov(shadow_stack_top_ptr, scratch_reg);
  }

  // Unlink PyFrame if needed
  asmjit::Label done = as_->newLabel();
  if (might_have_heap_frame) {
    as_->jnc(done);
#endif

    auto saved_rax_ptr = x86::ptr(x86::rbp, -8);

    hir::Type ret_type = func_->return_type;
    if (ret_type <= TCDouble) {
      as_->movsd(saved_rax_ptr, x86::xmm0);
    } else {
      as_->mov(saved_rax_ptr, x86::rax);
    }
    if (tstate_r != x86::rdi) {
      as_->mov(x86::rdi, tstate_r);
    }
    as_->call(reinterpret_cast<uint64_t>(JITRT_UnlinkFrame));
    if (ret_type <= TCDouble) {
      as_->movsd(x86::xmm0, saved_rax_ptr);
    } else {
      as_->mov(x86::rax, saved_rax_ptr);
    }
#ifdef SHADOW_FRAMES
    as_->bind(done);
  }
#endif
}

#ifdef SHADOW_FRAMES
void FrameAsm::linkOnStackShadowFrame(x86::Gp tstate_reg, x86::Gp scratch_reg) {
  const hir::Function* func = GetFunction();
  FrameMode frame_mode = func->frameMode;
  using namespace shadow_frame;
  x86::Mem shadow_stack_top_ptr = getStackTopPtr(tstate_reg);
  uintptr_t data =
      _PyShadowFrame_MakeData(env_.code_rt, PYSF_CODE_RT, PYSF_JIT);
  // Save old top of shadow stack
  as_->mov(scratch_reg, shadow_stack_top_ptr);
  as_->mov(kInFramePrevPtr, scratch_reg);
  // Set data
  if (frame_mode == FrameMode::kNormal) {
    as_->mov(scratch_reg, x86::ptr(tstate_reg, offsetof(PyThreadState, frame)));
    static_assert(
        PYSF_PYFRAME == 1 && _PyShadowFrame_NumPtrKindBits == 2,
        "Unexpected constant");
    as_->bts(scratch_reg, 0);
  } else {
    as_->mov(scratch_reg, data);
  }
  as_->mov(kInFrameDataPtr, scratch_reg);
  // Set orig_data
  // This is only necessary when in normal-frame mode because the frame is
  // already materialized on function entry. It is lazily filled when the frame
  // is materialized in shadow-frame mode.
  if (frame_mode == FrameMode::kNormal) {
    as_->mov(scratch_reg, data);
    as_->mov(shadow_frame::kInFrameOrigDataPtr, scratch_reg);
  }
  // Set our shadow frame as top of shadow stack
  as_->lea(scratch_reg, kFramePtr);
  as_->mov(shadow_stack_top_ptr, scratch_reg);
}

void FrameAsm::initializeFrameHeader(
    asmjit::x86::Gp tstate_reg,
    asmjit::x86::Gp scratch_reg) {
  if (!isGen()) {
    as_->push(scratch_reg);
    linkOnStackShadowFrame(tstate_reg, scratch_reg);
    as_->pop(scratch_reg);
  }
}
#endif
} // namespace jit::codegen

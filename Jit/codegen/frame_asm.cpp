// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/frame_asm.h"

#include <Python.h>

#include "internal/pycore_pystate.h"

#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/register_preserver.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/threaded_compile.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "internal/pycore_shadow_frame.h"
#endif

using namespace asmjit;
using namespace jit::hir;

namespace jit::codegen {

#ifdef ENABLE_SHADOW_FRAMES

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

#endif // ENABLE_SHADOW_FRAMES

#if PY_VERSION_HEX >= 0x030C0000

bool tstate_offset_inited;
int32_t tstate_offset = -1;

void initThreadStateOffset() {
  if (tstate_offset_inited) {
    return;
  }
  // PyThreadState_GetCurrent just accesses the thread local value, and
  // we want to figure out what the offset from the fs register it's
  // stored at. So verify that we recognize what it's doing and pull
  // out that offset.
  uint8_t* ts_func = reinterpret_cast<uint8_t*>(&_PyThreadState_GetCurrent);
  // 0x4 8b 48 64 e5 89 48 55
  if (ts_func[0] == 0x55 && // push rbp
      ts_func[1] == 0x48 && ts_func[2] == 0x89 &&
      ts_func[3] == 0xe5 && // mov rsp, rbp
      ts_func[4] == 0x64 && ts_func[5] == 0x48 && ts_func[6] == 0x8b &&
      ts_func[7] == 0x04 && ts_func[8] == 0x25) { // movq   %fs:OFFSET, %rax
    // movq   %fs:-0x18, %rax
    tstate_offset = *reinterpret_cast<int32_t*>(ts_func + 9);
  } else {
#ifndef Py_DEBUG
    assert(false);
#endif
  }
  tstate_offset_inited = true;
}

void FrameAsm::loadTState(
    const x86::Gp& dst_reg,
    [[maybe_unused]] RegisterPreserver& preserver) {
  if (tstate_offset != -1) {
    asmjit::x86::Mem tls(tstate_offset);
    tls.setSegment(x86::fs);
    as_->mov(dst_reg, tls);
  } else {
    as_->call(_PyThreadState_GetCurrent);
    as_->mov(dst_reg, x86::rax);
  }
}

void FrameAsm::linkNormalGeneratorFrame(
    RegisterPreserver& preserver,
    const asmjit::x86::Gp&,
    const asmjit::x86::Gp& tstate_reg) {
  preserver.preserve();

  uint64_t full_words = env_.shadow_frames_and_spill_size / kPointerSize;
  as_->mov(x86::rsi, full_words);
  as_->mov(x86::rdx, reinterpret_cast<intptr_t>(codeRuntime()));
  as_->lea(x86::rcx, x86::ptr(env_.gen_resume_entry_label));
  as_->mov(x86::r8, x86::rbp);
  as_->call(
      reinterpret_cast<uint64_t>(JITRT_AllocateAndLinkGenAndInterpreterFrame));
  as_->mov(tstate_reg, x86::rax);
  // tstate is now in RAX and GenDataFooter* in RDX. Swap RBP over to the
  // generator data so spilled data starts getting stored there. There
  // shouldn't have been any other data stored in the spilled area so far
  // so no need to copy things over.
  as_->mov(x86::rbp, x86::rdx);

  preserver.restore();
}

void FrameAsm::emitIncTotalRefCount(const asmjit::x86::Gp& scratch_reg) {
#ifdef Py_REF_DEBUG
  PyInterpreterState* interp;
  if (jit::getThreadedCompileContext().compileRunning()) {
    interp = jit::getThreadedCompileContext().interpreter();
  } else {
    interp = PyInterpreterState_Get();
  }
  Py_ssize_t* ref_total = &interp->object_state.reftotal;
  as_->mov(scratch_reg, ref_total);
  as_->inc(x86::ptr(scratch_reg.r64(), 0, sizeof(void*)));
#endif
}

void FrameAsm::incRef(
    const asmjit::x86::Gp& reg,
    const asmjit::x86::Gp& scratch_reg) {
  as_->mov(scratch_reg, x86::ptr(reg, offsetof(PyObject, ob_refcnt)));
  as_->inc(scratch_reg);
  Label immortal = as_->newLabel();
  as_->je(immortal);
  // mortal
  as_->mov(x86::ptr(reg, offsetof(PyObject, ob_refcnt)), scratch_reg);
  emitIncTotalRefCount(scratch_reg.r64());
  as_->bind(immortal);
}

bool FrameAsm::storeConst(
    const asmjit::x86::Gp& reg,
    int32_t offset,
    void* val,
    const asmjit::x86::Gp& scratch) {
  auto dest = x86::ptr(reg, offset, sizeof(void*));
  int64_t value = reinterpret_cast<int64_t>(val);
  if (fitsInt32(value)) {
    // the value fits in the register, let the caller know we didn't
    // populate scratch.
    as_->mov(dest, static_cast<uint32_t>(value));
    return true;
  }
  as_->mov(scratch, value);
  as_->mov(dest, scratch);
  return false;
}

#ifdef ENABLE_LIGHTWEIGHT_FRAMES

void FrameAsm::linkLightWeightFunctionFrame(
    RegisterPreserver& preserver,
    const asmjit::x86::Gp& func_reg,
    const asmjit::x86::Gp& tstate_reg) {
  // Light weight function headers are allocated on the stack as:
  //  PyFunctionObject* func_obj
  //  _PyInterpererFrame
  //
  // We need to initialize the f_code, f_funcobj fields of
  // the frame along w/ the previous pointer.
  initThreadStateOffset();

  // We have precious caller saved registers that we can trash - rax
  // and r10 are the only non-argument registers, and our arguments
  // are still in their initial registers. r10 we use for the extra
  // args, and if we aren't preserving the stack it's not initialized
  // yet, so we can use it. If we are preserving the stack (typically
  // only in ASAN builds) then we'll need to preserve that as well
  // after spilling and restoring the arguments around the call to
  // get the thread state.
  auto scratch = x86::gpq(INITIAL_EXTRA_ARGS_REG.loc);
  if (tstate_offset == -1) {
    preserver.preserve();
  }
  loadTState(tstate_reg, preserver);

  if (tstate_offset == -1) {
    preserver.restore();
    // and here's where we need to preserve the initial extra args reg
    // too.
    as_->push(scratch);
  }

  int frame_header_size = frameHeaderSize();
  PyObject* frame_helper = cinderx::getModuleState()->frameReifier();
  const auto ref_cnt = x86::eax;

#define FRAME_OFFSET(NAME) \
  -frame_header_size + offsetof(_PyInterpreterFrame, NAME) + sizeof(PyObject*)

  // Initialize the fields minus previous.
  // Store func before the header
  as_->mov(x86::ptr(x86::rbp, -frame_header_size), func_reg);
  incRef(func_reg, ref_cnt);

  // Store f_code
  bool needs_load =
      storeConst(x86::rbp, FRAME_OFFSET(f_code), func_->code.get(), scratch);
  if (!_Py_IsImmortal(func_->code.get())) {
    if (needs_load) {
      // if this fit into a 32-bit value we didn't spill it into scratch
      as_->mov(scratch, reinterpret_cast<uint64_t>(func_->code.get()));
    }
    incRef(scratch, ref_cnt);
  }

  // Store f_funcobj as our helper frame object
  storeConst(x86::rbp, FRAME_OFFSET(f_funcobj), frame_helper, scratch);
  JIT_DCHECK(_Py_IsImmortal(frame_helper), "frame helper must be immortal");

  // Store prev_instr
  _Py_CODEUNIT* code = _PyCode_CODE(GetFunction()->code.get()) - 1;
  storeConst(x86::rbp, FRAME_OFFSET(prev_instr), code, scratch);

  as_->mov(
      x86::ptr(x86::rbp, FRAME_OFFSET(owner), sizeof(char)),
      FRAME_OWNED_BY_THREAD);

  // Get the frame that is currently linked into thread state and update
  // our frames pointer back to it.
#if PY_VERSION_HEX >= 0x030D0000
  // 3.14+ - current_frame is stored in PyThreadState.current_frame
  const asmjit::x86::Gp& frame_holder = tstate_reg;
  // cur_frame->previous = PyThreadState.current_frame
  as_->mov(
      scratch, x86::ptr(tstate_reg, offsetof(PyThreadState, current_frame)));
#else
  // 3.12 - current_frame is stored in PyThreadState.cframe
  const asmjit::x86::Gp& frame_holder =
      x86::rax; // return value, we can freely use this as scratch
  as_->mov(frame_holder, x86::ptr(tstate_reg, offsetof(PyThreadState, cframe)));
  as_->mov(scratch, x86::ptr(frame_holder, offsetof(_PyCFrame, current_frame)));
#endif

  // cur_frame->previous = PyThreadState.cframe.current_frame
  as_->mov(x86::ptr(x86::rbp, FRAME_OFFSET(previous)), scratch);
  // Then finally link in our frame to thread state
  as_->lea(scratch, x86::ptr(x86::rbp, -frame_header_size + sizeof(PyObject*)));
  // (PyThreadState.cframe|PyThreadState).current_frame = &cur_frame
  as_->mov(x86::ptr(frame_holder, offsetof(_PyCFrame, current_frame)), scratch);

  if (tstate_offset == -1) {
    as_->pop(scratch);
  } else {
    preserver.remap();
  }
}
#endif

void FrameAsm::linkNormalFunctionFrame(
    RegisterPreserver& preserver,
    const asmjit::x86::Gp&,
    const asmjit::x86::Gp& tstate_reg) {
  preserver.preserve();
  if (kPyDebug) {
    as_->mov(x86::rsi, reinterpret_cast<intptr_t>(GetFunction()->code.get()));
    as_->call(reinterpret_cast<uint64_t>(
        JITRT_AllocateAndLinkInterpreterFrame_Debug));
  } else {
    as_->call(reinterpret_cast<uint64_t>(
        JITRT_AllocateAndLinkInterpreterFrame_Release));
  }
  as_->mov(tstate_reg, x86::rax);
  preserver.restore();
}

void FrameAsm::linkNormalFrame(
    RegisterPreserver& preserver,
    const asmjit::x86::Gp& func_reg,
    const asmjit::x86::Gp& tstate_reg) {
  JIT_DCHECK(func_reg == x86::rdi, "func_reg must be rdi");
  if (isGen()) {
    linkNormalGeneratorFrame(preserver, func_reg, tstate_reg);
  } else {
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
    linkLightWeightFunctionFrame(preserver, func_reg, tstate_reg);
#else
    linkNormalFunctionFrame(preserver, func_reg, tstate_reg);
#endif
  }
}

#else

// Links a normal frame and initializes tstate variable.
void FrameAsm::linkNormalFrame(
    RegisterPreserver& preserver,
    const asmjit::x86::Gp& func_reg,
    const asmjit::x86::Gp& tstate_reg) {
  preserver.preserve();
  as_->mov(
      x86::rdi,
      reinterpret_cast<intptr_t>(codeRuntime()->frameState()->code().get()));
  as_->mov(
      x86::rsi,
      reinterpret_cast<intptr_t>(
          codeRuntime()->frameState()->builtins().get()));
  as_->mov(
      x86::rdx,
      reinterpret_cast<intptr_t>(codeRuntime()->frameState()->globals().get()));

  as_->call(reinterpret_cast<uint64_t>(JITRT_AllocateAndLinkFrame));
  as_->mov(tstate_reg, x86::rax);
  preserver.restore();
}

#endif

#if PY_VERSION_HEX < 0x030C0000
void FrameAsm::loadTState(
    const x86::Gp& dst_reg,
    RegisterPreserver& preserver) {
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
  RegisterPreserver preserver(as_, save_regs);

  auto load_tstate_and_move = [&]() {
    loadTState(tstate_reg, preserver);
    preserver.remap();
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
      linkNormalFrame(preserver, func_reg, tstate_reg);
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

  linkNormalFrame(preserver, func_reg, tstate_reg);
}
#endif

void FrameAsm::generateUnlinkFrame(
    const x86::Gp& tstate_r,
    [[maybe_unused]] bool is_generator) {
#ifdef ENABLE_SHADOW_FRAMES
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
#ifdef ENABLE_SHADOW_FRAMES
    as_->bind(done);
  }
#endif
}

#ifdef ENABLE_SHADOW_FRAMES
void FrameAsm::linkOnStackShadowFrame(
    const x86::Gp& tstate_reg,
    const x86::Gp& scratch_reg) {
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

int FrameAsm::frameHeaderSize() {
  if (func_->code->co_flags & kCoFlagsAnyGenerator) {
    return 0;
  }

#if defined(ENABLE_SHADOW_FRAMES)
  return sizeof(FrameHeader);
#elif defined(ENABLE_LIGHTWEIGHT_FRAMES)
  return sizeof(FrameHeader) + sizeof(PyObject*) * func_->code->co_framesize;
#else
  return 0;
#endif
}

} // namespace jit::codegen

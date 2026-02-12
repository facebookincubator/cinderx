// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/frame_asm.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_interp_structs.h"
#endif
#include "internal/pycore_pystate.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/register_preserver.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/frame_header.h"
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

#if defined(CINDER_X86_64)
constexpr arch::Mem kFramePtr = x86::ptr(x86::rbp, -kJITShadowFrameSize);
constexpr arch::Mem kInFramePrevPtr =
    x86::ptr(x86::rbp, -kJITShadowFrameSize + SHADOW_FRAME_FIELD_OFF(prev));
constexpr arch::Mem kInFrameDataPtr =
    x86::ptr(x86::rbp, -kJITShadowFrameSize + SHADOW_FRAME_FIELD_OFF(data));
constexpr arch::Mem kInFrameOrigDataPtr = x86::ptr(
    x86::rbp,
    -kJITShadowFrameSize + JIT_SHADOW_FRAME_FIELD_OFF(orig_data));

constexpr arch::Mem getStackTopPtr(arch::Gp tstate_reg) {
  return x86::ptr(tstate_reg, offsetof(PyThreadState, shadow_frame));
}
#else
CINDER_UNSUPPORTED
#endif

} // namespace shadow_frame

#endif // ENABLE_SHADOW_FRAMES

#if PY_VERSION_HEX >= 0x030C0000

bool tstate_offset_inited;
int32_t tstate_offset = -1;

void initThreadStateOffset() {
  if (tstate_offset_inited) {
    return;
  }

#if defined(CINDER_X86_64)
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
#elif defined(CINDER_AARCH64)
  // PyThreadState_GetCurrent just accesses the thread local value, and
  // we want to figure out what the offset from the thread-local storage it's
  // stored at. So verify that we recognize what it's doing and pull
  // out that offset.
  uint32_t* ts_func = reinterpret_cast<uint32_t*>(&_PyThreadState_GetCurrent);

  if (ts_func[0] == 0xa9bf7bfd && // stp x29, x30, [sp, #-16]!
      ts_func[1] == 0x910003fd && // mov x29, sp
      ((ts_func[2] & ~0x1f) == 0xd53bd048) // mrs x?, tpidr_el0
  ) {
    // Here we know we are loading the thread local base offset into some
    // register, based on the mrs instruction.
    uint32_t reg = ts_func[2] & 0x1f;
    int32_t current_offset = 0;

    // Now, we will interpret any subsequent add instructions in order to
    // determine the offset. We will know we are done when we hit an ldr x0, or
    // we hit something unknown and need to break.
    for (size_t index = 3;; index++) {
      if (ts_func[index] == (0xf9400000 | (reg << 5))) {
        // ldr x0, [x?]
        //
        // Here we are loading the temporarily calculated offset into x0, which
        // is the return register. At this point we are done.
        break;
      } else if (
          (ts_func[index] & ~0x7ffc00) == (0x91000000 | (reg << 5) | reg)) {
        // add x?, x?, #<imm>{, <shift>}
        //
        // Here we are adding to the temporary offset register. It is encoded
        // as: 100100010<shift><imm><rn><rd>, where shift is 1 bit, imm is 12
        // bits, rn and rd are both 5 bits, which should be equivalent to reg.
        uint32_t imm = (ts_func[index] >> 10) & 0xfff;
        if (ts_func[index] & (1 << 22)) {
          imm <<= 12;
        }

        current_offset += imm;
      } else {
        // Otherwise, we found something we did not anticipate, so we need to
        // bail out.
        current_offset = -1;
        break;
      }
    }

    tstate_offset = current_offset;
  }

#ifndef Py_DEBUG
  if (tstate_offset == -1) {
    assert(false);
  }
#endif
#else
  CINDER_UNSUPPORTED
#endif

  tstate_offset_inited = true;
}

void FrameAsm::loadTState(const arch::Gp& dst_reg) {
#if defined(CINDER_X86_64)
  if (tstate_offset != -1) {
    asmjit::x86::Mem tls(tstate_offset);
    tls.setSegment(x86::fs);
    as_->mov(dst_reg, tls);
  } else {
    as_->call(_PyThreadState_GetCurrent);
    as_->mov(dst_reg, x86::rax);
  }
#elif defined(CINDER_AARCH64)
  if (tstate_offset != -1) {
    as_->mrs(dst_reg, a64::Predicate::SysReg::kTPIDR_EL0);
    as_->ldr(
        dst_reg,
        arch::ptr_resolve(as_, dst_reg, tstate_offset, arch::reg_scratch_0));
  } else {
    as_->mov(arch::reg_scratch_br, _PyThreadState_GetCurrent);
    as_->blr(arch::reg_scratch_br);
    as_->mov(dst_reg, a64::x0);
  }
#else
  CINDER_UNSUPPORTED
#endif
}

void FrameAsm::linkNormalGeneratorFrame(
    RegisterPreserver& preserver,
    const arch::Gp&,
    const arch::Gp& tstate_reg) {
  preserver.preserve();

#if defined(CINDER_X86_64)
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
#else
  CINDER_UNSUPPORTED
#endif

  preserver.restore();
}

#ifdef Py_GIL_DISABLED

void emit_inc_total_ref_count_x86_nogil(
    [[maybe_unused]] asmjit::x86::Builder* as,
    [[maybe_unused]] const arch::Gp& tstate_reg) {
#ifdef Py_REF_DEBUG
#if defined(CINDER_X86_64)
  as->inc(
      x86::ptr(
          tstate_reg,
          offsetof(_PyThreadStateImpl, reftotal),
          sizeof(Py_ssize_t)));
#else
  CINDER_UNSUPPORTED
#endif
#endif
}

void inc_ref_x86_nogil(
    [[maybe_unused]] asmjit::x86::Builder* as,
    [[maybe_unused]] const arch::Gp& reg,
    [[maybe_unused]] const arch::Gp& scratch_reg,
    [[maybe_unused]] const arch::Gp& tstate_reg) {
#if defined(CINDER_X86_64)
  // For free-threaded Python, check immortality via ob_ref_local.
  // Load ob_ref_local (32-bit). Note this load should be atomic with relaxed
  // memory semantics, which is default on x86.
  as->mov(
      scratch_reg.r32(), x86::dword_ptr(reg, offsetof(PyObject, ob_ref_local)));
  // Add 1 - if result is zero, object was immortal (UINT32_MAX + 1 overflows
  // to 0)
  as->inc(scratch_reg.r32());
  Label immortal = as->newLabel();
  as->jz(immortal);

  // Check if object is owned by current thread by comparing ob_tid with
  // the current thread ID. This is equivalent to _Py_IsOwnedByCurrentThread.
  // TODO: I don't have the stomach to find another scratch register for the
  // ob_tid to current TID comparison, so I'm just reusing the one scratch
  // register we have for now plus the stack. Should still be pretty fast on
  // x86.
  Label not_owned = as->newLabel();
  as->push(scratch_reg);
  as->mov(scratch_reg, x86::ptr(reg, offsetof(PyObject, ob_tid)));
  x86::Mem tid_mem;
  tid_mem.setOffset(0);
  tid_mem.setSize(sizeof(uintptr_t));
  tid_mem.setSegment(x86::fs);
  as->cmp(scratch_reg, tid_mem);
  as->pop(scratch_reg);
  as->jne(not_owned);

  // Owned by current thread - store directly to ob_ref_local (fast path).
  // Note this store should be atomic with relaxed memory semantics, which is
  // default on x86.
  as->mov(
      x86::dword_ptr(reg, offsetof(PyObject, ob_ref_local)), scratch_reg.r32());
  Label done_incref = as->newLabel();
  as->jmp(done_incref);

  // Not owned - use atomic add to ob_ref_shared (slow path)
  as->bind(not_owned);
  as->lock().add(
      x86::qword_ptr(reg, offsetof(PyObject, ob_ref_shared)),
      1 << _Py_REF_SHARED_SHIFT);
  as->bind(done_incref);
  emit_inc_total_ref_count_x86_nogil(as, tstate_reg);
  as->bind(immortal);
#else
  CINDER_UNSUPPORTED
#endif
}

#else
// GILful inc-ref implementation

void emit_inc_total_ref_count_x86_gil(
    [[maybe_unused]] asmjit::x86::Builder* as,
    [[maybe_unused]] const arch::Gp& scratch_reg) {
#ifdef Py_REF_DEBUG
#if defined(CINDER_X86_64)
  PyInterpreterState* interp;
  if (jit::getThreadedCompileContext().compileRunning()) {
    interp = jit::getThreadedCompileContext().interpreter();
  } else {
    interp = PyInterpreterState_Get();
  }

  Py_ssize_t* ref_total = &interp->object_state.reftotal;
  as->mov(scratch_reg, ref_total);
  as->inc(x86::ptr(scratch_reg, 0, sizeof(void*)));
#else
  CINDER_UNSUPPORTED
#endif
#endif
}

void inc_ref_x86_gil(
    asmjit::x86::Builder* as,
    const arch::Gp& reg,
    const arch::Gp& scratch_reg) {
#if defined(CINDER_X86_64)
  Label immortal = as->newLabel();
  as->mov(scratch_reg.r32(), x86::ptr(reg, offsetof(PyObject, ob_refcnt)));
  as->inc(scratch_reg.r32());
#if PY_VERSION_HEX >= 0x030E0000
  as->js(immortal);
#else
  as->je(immortal);
#endif
  // mortal
  as->mov(x86::ptr(reg, offsetof(PyObject, ob_refcnt)), scratch_reg.r32());
  emit_inc_total_ref_count_x86_gil(as, scratch_reg);
  as->bind(immortal);
#else
  CINDER_UNSUPPORTED
#endif
}
#endif // Py_GIL_DISABLED

void FrameAsm::incRef(
    const arch::Gp& reg,
    const arch::Gp& scratch_reg,
    [[maybe_unused]] const arch::Gp& tstate_reg) {
#if defined(CINDER_X86_64)

#if defined(Py_GIL_DISABLED)
  inc_ref_x86_nogil(as_, reg, scratch_reg, tstate_reg);
#else
  inc_ref_x86_gil(as_, reg, scratch_reg);
#endif

#else
  CINDER_UNSUPPORTED
#endif
}

bool FrameAsm::storeConst(
    const arch::Gp& reg,
    int32_t offset,
    void* val,
    const arch::Gp& scratch) {
#if defined(CINDER_X86_64)
  auto dest = x86::ptr(reg, offset, sizeof(void*));
  int64_t value = reinterpret_cast<int64_t>(val);
  if (fitsSignedInt<32>(value)) {
    // the value fits in the register, let the caller know we didn't
    // populate scratch.
    as_->mov(dest, static_cast<uint32_t>(value));
    return true;
  }
  as_->mov(scratch, value);
  as_->mov(dest, scratch);
#else
  CINDER_UNSUPPORTED
#endif
  return false;
}

void FrameAsm::linkLightWeightFunctionFrame(
    RegisterPreserver& preserver,
    const arch::Gp& func_reg,
    const arch::Gp& tstate_reg) {
#if defined(CINDER_X86_64) && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  // Light weight function headers are allocated on the stack as:
  //  PyFunctionObject* func_obj
  //  _PyInterpreterFrame
  //
  // We need to initialize the f_code, f_funcobj fields of
  // the frame along w/ the previous pointer.
  asmjit::BaseNode* init_tstate_off_cursor = as_->cursor();
  initThreadStateOffset();
  env_.addAnnotation("Init tstate offset", init_tstate_off_cursor);

  // We have precious caller saved registers that we can trash - rax
  // and r10 are the only non-argument registers, and our arguments
  // are still in their initial registers. r10 we use for the extra
  // args, and if we aren't preserving the stack it's not initialized
  // yet, so we can use it. If we are preserving the stack (typically
  // only in ASAN builds) then we'll need to preserve that as well
  // after spilling and restoring the arguments around the call to
  // get the thread state.
  asmjit::BaseNode* load_tstate_cursor = as_->cursor();
  auto scratch = x86::gpq(INITIAL_EXTRA_ARGS_REG.loc);
  if (tstate_offset == -1) {
    preserver.preserve();
  }
  loadTState(tstate_reg);

  if (tstate_offset == -1) {
    preserver.restore();
    // and here's where we need to preserve the initial extra args reg
    // too.
    as_->push(scratch);
  }
  env_.addAnnotation("Load tstate", load_tstate_cursor);

  int frame_header_size = frameHeaderSizeExcludingSpillSpace();
#if PY_VERSION_HEX < 0x030E0000
  PyObject* frame_reifier = cinderx::getModuleState()->frameReifier();
#else
  PyObject* frame_reifier = env_.code_rt->reifier();
#endif
  const auto ref_cnt = x86::rax;

#define FRAME_OFFSET(NAME) \
  -frame_header_size + offsetof(_PyInterpreterFrame, NAME) + sizeof(FrameHeader)

  asmjit::BaseNode* store_func_cursor = as_->cursor();
#if PY_VERSION_HEX >= 0x030E0000
  as_->mov(x86::ptr(x86::rbp, -frame_header_size, sizeof(void*)), 0);
  env_.addAnnotation("Store rtfs state to 0", store_func_cursor);
#else
  // Initialize the fields minus previous.
  // Store func before the header
  as_->mov(x86::ptr(x86::rbp, -frame_header_size), func_reg);
  incRef(func_reg, ref_cnt, tstate_reg);
  env_.addAnnotation("Store func before frame header", store_func_cursor);
#endif

  asmjit::BaseNode* store_f_code_cursor = as_->cursor();
#if PY_VERSION_HEX >= 0x030E0000
  PyObject* executable = frame_reifier;
#else
  PyObject* executable = (PyObject*)func_->code.get();
#endif
  bool needs_load =
      storeConst(x86::rbp, FRAME_OFFSET(FRAME_EXECUTABLE), executable, scratch);
  if (!_Py_IsImmortal(executable)) {
    if (needs_load) {
      // if this fit into a 32-bit value we didn't spill it into scratch
      as_->mov(scratch, reinterpret_cast<uint64_t>(executable));
    }
    incRef(scratch, ref_cnt, tstate_reg);
  }
  env_.addAnnotation(
      "Set _PyInterpreterFrame::f_executable/f_code", store_f_code_cursor);

  // Store f_funcobj as our helper frame reifier object
  asmjit::BaseNode* store_f_funcobj_cursor = as_->cursor();
#if PY_VERSION_HEX >= 0x030E0000
  as_->mov(x86::ptr(x86::rbp, FRAME_OFFSET(f_funcobj)), func_reg);
  incRef(func_reg, ref_cnt, tstate_reg);
#else
  storeConst(x86::rbp, FRAME_OFFSET(f_funcobj), frame_reifier, scratch);
  JIT_DCHECK(_Py_IsImmortal(frame_reifier), "frame helper must be immortal");
#endif
  env_.addAnnotation(
      "Set _PyInterpreterFrame::f_funcobj", store_f_funcobj_cursor);

  // Store prev_instr + tlbc_index
  asmjit::BaseNode* store_prev_instr_cursor = as_->cursor();
#if PY_VERSION_HEX >= 0x030E0000
  _Py_CODEUNIT* code = _PyCode_CODE(GetFunction()->code.get());
#else
  _Py_CODEUNIT* code = _PyCode_CODE(GetFunction()->code.get()) - 1;
#endif
  storeConst(x86::rbp, FRAME_OFFSET(FRAME_INSTR), code, scratch);
  env_.addAnnotation(
      "Set _PyInterpreterFrame::prev_instr", store_prev_instr_cursor);
#ifdef Py_GIL_DISABLED
  asmjit::BaseNode* tlbc_index_cursor = as_->cursor();
  as_->mov(x86::dword_ptr(x86::rbp, FRAME_OFFSET(tlbc_index)), 0);
  env_.addAnnotation("Set TLBC index to 0", tlbc_index_cursor);
#endif

  // Store owner
  asmjit::BaseNode* store_owner_cursor = as_->cursor();
  as_->mov(
      x86::ptr(x86::rbp, FRAME_OFFSET(owner), sizeof(char)),
      FRAME_OWNED_BY_THREAD);
  env_.addAnnotation("Set _PyInterpreterFrame::owner", store_owner_cursor);

  // Get the frame that is currently linked into thread state and update
  // our frames pointer back to it.
  asmjit::BaseNode* get_tos_cursor = as_->cursor();
#if PY_VERSION_HEX >= 0x030D0000
  // 3.14+ - current_frame is stored in PyThreadState.current_frame
  const arch::Gp& frame_holder = tstate_reg;
  // cur_frame->previous = PyThreadState.current_frame
  as_->mov(
      scratch, x86::ptr(tstate_reg, offsetof(PyThreadState, current_frame)));
#else
  // 3.12 - current_frame is stored in PyThreadState.cframe
  const arch::Gp& frame_holder =
      x86::rax; // return value, we can freely use this as scratch
  as_->mov(frame_holder, x86::ptr(tstate_reg, offsetof(PyThreadState, cframe)));
  as_->mov(scratch, x86::ptr(frame_holder, offsetof(_PyCFrame, current_frame)));
#endif
  env_.addAnnotation("Get topmost frame", get_tos_cursor);

  asmjit::BaseNode* store_prev_cursor = as_->cursor();
  // cur_frame->previous = PyThreadState.cframe.current_frame
  as_->mov(x86::ptr(x86::rbp, FRAME_OFFSET(previous)), scratch);
  env_.addAnnotation("Set _PyInterpreterFrame::previous", store_prev_cursor);

#if PY_VERSION_HEX >= 0x030E0000
  asmjit::BaseNode* stack_pointer_cursor = as_->cursor();
  as_->lea(scratch, x86::ptr(x86::rbp, FRAME_OFFSET(localsplus)));
  as_->mov(x86::ptr(x86::rbp, FRAME_OFFSET(stackpointer)), scratch);
  env_.addAnnotation(
      "Set _PyInterpreterFrame::stackpointer", stack_pointer_cursor);

  asmjit::BaseNode* locals_cursor = as_->cursor();
  as_->mov(x86::qword_ptr(x86::rbp, FRAME_OFFSET(f_locals)), 0);
  env_.addAnnotation("Set _PyInterpreterFrame::f_locals", locals_cursor);
#endif

  // Then finally link in our frame to thread state
  asmjit::BaseNode* update_linkage_cursor = as_->cursor();
  as_->lea(scratch, x86::ptr(x86::rbp, -frame_header_size + sizeof(PyObject*)));
#if PY_VERSION_HEX >= 0x030D0000
  // (PyThreadState.cframe|PyThreadState).current_frame = &cur_frame
  as_->mov(
      x86::ptr(frame_holder, offsetof(PyThreadState, current_frame)), scratch);
#else
  // (PyThreadState.cframe|PyThreadState).current_frame = &cur_frame
  as_->mov(x86::ptr(frame_holder, offsetof(_PyCFrame, current_frame)), scratch);
#endif
  env_.addAnnotation(
      "Set _PyInterpreterFrame as topmost frame", update_linkage_cursor);

  if (tstate_offset == -1) {
    as_->pop(scratch);
  } else {
    preserver.remap();
  }
#else
  throw std::runtime_error{
      "linkLightWeightFunctionFrame: Lightweight frames are not supported"};
#endif
}

void FrameAsm::linkNormalFunctionFrame(
    RegisterPreserver& preserver,
    const arch::Gp&,
    const arch::Gp& tstate_reg) {
  preserver.preserve();

#if defined(CINDER_X86_64)
  if (kPyDebug) {
    as_->mov(x86::rsi, reinterpret_cast<intptr_t>(GetFunction()->code.get()));
    as_->call(
        reinterpret_cast<uint64_t>(
            JITRT_AllocateAndLinkInterpreterFrame_Debug));
  } else {
    as_->call(
        reinterpret_cast<uint64_t>(
            JITRT_AllocateAndLinkInterpreterFrame_Release));
  }

  as_->mov(tstate_reg, x86::rax);
#else
  CINDER_UNSUPPORTED
#endif

  preserver.restore();
}

void FrameAsm::linkNormalFrame(
    RegisterPreserver& preserver,
    const arch::Gp& func_reg,
    const arch::Gp& tstate_reg) {
#if defined(CINDER_X86_64)
  JIT_DCHECK(func_reg == x86::rdi, "func_reg must be rdi");
#else
  CINDER_UNSUPPORTED
#endif

  if (isGen()) {
    linkNormalGeneratorFrame(preserver, func_reg, tstate_reg);
  } else if (getConfig().frame_mode == FrameMode::kLightweight) {
    linkLightWeightFunctionFrame(preserver, func_reg, tstate_reg);
  } else {
    linkNormalFunctionFrame(preserver, func_reg, tstate_reg);
  }
}

#else

// Links a normal frame and initializes tstate variable.
void FrameAsm::linkNormalFrame(
    RegisterPreserver& preserver,
    const arch::Gp& func_reg,
    const arch::Gp& tstate_reg) {
  preserver.preserve();

#if defined(CINDER_X86_64)
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
#else
  CINDER_UNSUPPORTED
#endif

  preserver.restore();
}

#endif

#if PY_VERSION_HEX < 0x030C0000
void FrameAsm::loadTState(const arch::Gp& dst_reg) {
#if defined(CINDER_X86_64)
  uint64_t tstate =
      reinterpret_cast<uint64_t>(&_PyRuntime.gilstate.tstate_current);

  if (fitsSignedInt<32>(tstate)) {
    as_->mov(dst_reg, x86::ptr(tstate));
  } else {
    as_->mov(dst_reg, tstate);
    as_->mov(dst_reg, x86::ptr(dst_reg));
  }
#else
  CINDER_UNSUPPORTED
#endif
}

void FrameAsm::generateLinkFrame(
    const arch::Gp& func_reg,
    const arch::Gp& tstate_reg,
    const std::vector<std::pair<const arch::Reg&, const arch::Reg&>>&
        save_regs) {
  RegisterPreserver preserver(as_, save_regs);

  auto load_tstate_and_move = [&]() {
    loadTState(tstate_reg);
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
    case FrameMode::kNormal:
      linkNormalFrame(preserver, func_reg, tstate_reg);
      break;
    case FrameMode::kLightweight:
      JIT_ABORT("Lightweight frames are not supported in 3.10");
      break;
  }
}

#else

void FrameAsm::generateLinkFrame(
    const arch::Gp& func_reg,
    const arch::Gp& tstate_reg,
    const std::vector<std::pair<const arch::Reg&, const arch::Reg&>>&
        save_regs) {
  JIT_CHECK(
      GetFunction()->frameMode != FrameMode::kShadow,
      "3.12 doesn't have shadow frames");

  RegisterPreserver preserver(as_, save_regs);

  linkNormalFrame(preserver, func_reg, tstate_reg);
}
#endif

void FrameAsm::generateUnlinkFrame([[maybe_unused]] bool is_generator) {
#if defined(CINDER_X86_64)
#ifdef ENABLE_SHADOW_FRAMES
  // Unlink shadow frame? The send implementation handles unlinking these for
  // generators.
  as_->mov(x86::rdi, is_generator ? 0 : 1);
  auto saved_rax_ptr = x86::ptr(x86::rbp, -8);
#else
  auto saved_rax_ptr = x86::ptr(x86::rbp, -frameHeaderSize());
#endif

  hir::Type ret_type = func_->return_type;
  if (ret_type <= TCDouble) {
    as_->movsd(saved_rax_ptr, x86::xmm0);
  } else {
    as_->mov(saved_rax_ptr, x86::rax);
  }
  as_->call(reinterpret_cast<uint64_t>(JITRT_UnlinkFrame));
  if (ret_type <= TCDouble) {
    as_->movsd(x86::xmm0, saved_rax_ptr);
  } else {
    as_->mov(x86::rax, saved_rax_ptr);
  }
#else
  CINDER_UNSUPPORTED
#endif
}

#ifdef ENABLE_SHADOW_FRAMES
void FrameAsm::linkOnStackShadowFrame(
    const arch::Gp& tstate_reg,
    const arch::Gp& scratch_reg) {
#if defined(CINDER_X86_64)
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
#else
  CINDER_UNSUPPORTED
#endif
}

void FrameAsm::initializeFrameHeader(
    arch::Gp tstate_reg,
    arch::Gp scratch_reg) {
#if defined(CINDER_X86_64)
  if (!isGen()) {
    as_->push(scratch_reg);
    linkOnStackShadowFrame(tstate_reg, scratch_reg);
    as_->pop(scratch_reg);
  }
#else
  CINDER_UNSUPPORTED
#endif
}
#endif

int FrameAsm::frameHeaderSizeExcludingSpillSpace() const {
  return jit::frameHeaderSize(func_->code);
}

int FrameAsm::frameHeaderSize() {
#if defined(ENABLE_SHADOW_FRAMES)
  return frameHeaderSizeExcludingSpillSpace();
#else
  return frameHeaderSizeExcludingSpillSpace() + sizeof(void*);
#endif
}

} // namespace jit::codegen

// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/gen_asm.h"

#include <Python.h>
#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "internal/pycore_shadow_frame.h"
#endif
#include <Python.h>

#include "frameobject.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_allocator.h"
#include "cinderx/Jit/codegen/autogen.h"
#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/codegen/gen_asm_utils.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/hir/analysis.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/jit_gdb_support.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/dce.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/Jit/lir/regalloc.h"
#include "cinderx/Jit/lir/verify.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove
#include "cinderx/UpstreamBorrow/borrowed.h"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <list>
#include <unordered_map>
#include <vector>

using namespace asmjit;
using namespace jit;
using namespace jit::hir;
using namespace jit::lir;
using namespace jit::util;

namespace jit::codegen {

namespace {

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

#define ASM_CHECK_THROW(exp)                         \
  {                                                  \
    auto err = (exp);                                \
    if (err != kErrorOk) {                           \
      auto message = DebugUtils::errorAsString(err); \
      throw AsmJitException(err, #exp, message);     \
    }                                                \
  }

#define ASM_CHECK(exp, what)             \
  {                                      \
    auto err = (exp);                    \
    JIT_CHECK(                           \
        err == kErrorOk,                 \
        "Failed generating {}: {}",      \
        (what),                          \
        DebugUtils::errorAsString(err)); \
  }

// Scratch register used by the various deopt trampolines.
//
// NB: This MUST be r15. If you change the register you'll also need to change
// the deopt trampoline code that saves all registers.
const auto deopt_scratch_reg = x86::r15;

// Set RBP to "original RBP" value when called in the context of a generator.
void RestoreOriginalGeneratorRBP(x86::Emitter* as) {
  size_t original_rbp_offset = offsetof(GenDataFooter, originalRbp);
  as->mov(x86::rbp, x86::ptr(x86::rbp, original_rbp_offset));
}

void raiseUnboundLocalError(BorrowedRef<> name) {
  // name is converted into a `char*` in format_exc_check_arg
  Cix_format_exc_check_arg(
      _PyThreadState_GET(),
      PyExc_UnboundLocalError,
      "local variable '%.200s' referenced before assignment",
      name);
}

void raiseUnboundFreevarError(BorrowedRef<> name) {
  // name is converted into a `char*` in format_exc_check_arg
  Cix_format_exc_check_arg(
      _PyThreadState_GET(),
      PyExc_NameError,
      "free variable '%.200s' referenced before assignment in enclosing scope",
      name);
}

void raiseAttributeError(BorrowedRef<> receiver, BorrowedRef<> name) {
  PyErr_Format(
      PyExc_AttributeError,
      "'%.50s' object has no attribute '%U'",
      Py_TYPE(receiver)->tp_name,
      name);
}

CiPyFrameObjType*
prepareForDeopt(const uint64_t* regs, Runtime* runtime, std::size_t deopt_idx) {
  JIT_CHECK(deopt_idx != -1ull, "deopt_idx must be valid");
  const DeoptMetadata& deopt_meta = runtime->getDeoptMetadata(deopt_idx);
  PyThreadState* tstate = _PyThreadState_UncheckedGet();
#if PY_VERSION_HEX < 0x030C0000
  Ref<PyFrameObject> f = materializePyFrameForDeopt(tstate);

  PyFrameObject* frame = f.release();
  PyFrameObject* frame_iter = frame;
  _PyShadowFrame* sf_iter = tstate->shadow_frame;
  // Iterate one past the inline depth because that is the caller frame.
  for (int i = deopt_meta.inline_depth(); i >= 0; i--) {
    // Transfer ownership of shadow frame to the interpreter. The associated
    // Python frame will be ignored during future attempts to materialize the
    // stack.
    _PyShadowFrame_SetOwner(sf_iter, PYSF_INTERP);
    reifyFrame(frame_iter, deopt_meta, deopt_meta.frame_meta.at(i), regs);
    frame_iter = frame_iter->f_back;
    sf_iter = sf_iter->prev;
  }
#else
  _PyInterpreterFrame* frame = tstate->cframe->current_frame;
  reifyFrame(frame, deopt_meta, deopt_meta.frame_meta.at(0), regs);
  if (frame->f_code->co_flags & kCoFlagsAnyGenerator) {
    JitGenObject* gen = JitGenObject::cast(_PyFrame_GetGenerator(frame));
    JIT_CHECK(gen != nullptr, "Not a JIT generator");
    deopt_jit_gen_object_only(gen);
  }
  UPGRADE_NOTE(SUPPORT_JIT_INLINING, T198250666)
#endif
  // Clear our references now that we've transferred them to the frame
  MemoryView mem{regs};
  Ref<> deopt_obj = profileDeopt(deopt_idx, deopt_meta, mem);
  runtime->recordDeopt(deopt_idx, deopt_obj);
  releaseRefs(deopt_meta, mem);
  if (!PyErr_Occurred()) {
    auto reason = deopt_meta.reason;
    switch (reason) {
      case DeoptReason::kGuardFailure: {
        runtime->guardFailed(deopt_meta);
        break;
      }
      case DeoptReason::kYieldFrom: {
        break;
      }
      case DeoptReason::kUnhandledNullField:
        raiseAttributeError(deopt_obj, deopt_meta.eh_name);
        break;
      case DeoptReason::kUnhandledUnboundLocal:
        raiseUnboundLocalError(deopt_meta.eh_name);
        break;
      case DeoptReason::kUnhandledUnboundFreevar:
        raiseUnboundFreevarError(deopt_meta.eh_name);
        break;
      case DeoptReason::kUnhandledException:
        JIT_ABORT("unhandled exception without error set");
        break;
      case DeoptReason::kRaise:
        // This code mirrors what happens in _PyEval_EvalFrameDefault although
        // I'm not sure how to test it. Not clear it can happen with JIT.
#ifdef NDEBUG
        if (!PyErr_Occurred()) {
          PyErr_SetString(
              PyExc_SystemError, "error return without exception set");
        }
#else
        JIT_CHECK(PyErr_Occurred(), "Error return without exception set");
#endif
        break;
      case DeoptReason::kRaiseStatic:
        JIT_ABORT("Lost exception when raising static exception");
        break;
      case DeoptReason::kReraise:
        PyErr_SetString(PyExc_RuntimeError, "No active exception to reraise");
        break;
    }
  }
  return frame;
}

#if PY_VERSION_HEX < 0x030C0000
PyObject* resumeInInterpreter(
    PyFrameObject* frame,
    Runtime* runtime,
    std::size_t deopt_idx) {
  if (frame->f_gen) {
    auto gen = reinterpret_cast<PyGenObject*>(frame->f_gen);
    // It's safe to call jitgen_data_free directly here, rather than
    // through _PyJIT_GenDealloc. Ownership of all references have been
    // transferred to the frame.
    jitgen_data_free(gen);
  }
  PyThreadState* tstate = PyThreadState_Get();
  PyObject* result = nullptr;
  // Resume all of the inlined frames and the caller
  const DeoptMetadata& deopt_meta = runtime->getDeoptMetadata(deopt_idx);
  int inline_depth = deopt_meta.inline_depth();
  int err_occurred = (deopt_meta.reason != DeoptReason::kGuardFailure);
  while (inline_depth >= 0) {
    // TODO(emacs): Investigate skipping resuming frames that do not have
    // try/catch. Will require re-adding _PyShadowFrame_Pop back for
    // non-generators and unlinking the frame manually.

    // We need to maintain the invariant that there is at most one shadow frame
    // on the shadow stack for each frame on the Python stack. Unless we are a
    // a generator, the interpreter will insert a new entry on the shadow stack
    // when execution resumes there, so we remove our entry.
    if (!frame->f_gen) {
      _PyShadowFrame_Pop(tstate, tstate->shadow_frame);
    }
    // Resume one frame.
    PyFrameObject* prev_frame = frame->f_back;
    // Delegate management of `tstate->frame` to the interpreter loop. On
    // entry, it expects that tstate->frame points to the frame for the calling
    // function.
    JIT_CHECK(tstate->frame == frame, "unexpected frame at top of stack");
    tstate->frame = prev_frame;
    result = PyEval_EvalFrameEx(frame, err_occurred);
    JITRT_DecrefFrame(frame);
    frame = prev_frame;

    err_occurred = result == nullptr;
    // Push the previous frame's result onto the value stack. We can't push
    // after resuming because f_stacktop is nullptr during execution of a frame.
    if (!err_occurred) {
      if (inline_depth > 0) {
        // The caller is at inline depth 0, so we only attempt to push the
        // result onto the stack in the deeper (> 0) frames. Otherwise, we
        // should just return the value from the native code in the way our
        // native calling convention requires.
        frame->f_valuestack[frame->f_stackdepth++] = result;
      }
    }
    inline_depth--;
  }
  return result;
}

#else

PyObject* resumeInInterpreter(
    _PyInterpreterFrame* frame,
    Runtime* runtime,
    std::size_t deopt_idx) {
  UPGRADE_NOTE(SUPPORT_JIT_INLINING, T198250666)
  PyThreadState* tstate = PyThreadState_Get();

  const DeoptMetadata& deopt_meta = runtime->getDeoptMetadata(deopt_idx);
  int err_occurred = (deopt_meta.reason != DeoptReason::kGuardFailure);

  // Delegate management of the frame to the interpreter loop. On entry, it
  // expects tstate->cframe->current_frame points to the frame for the calling
  // function. We don't need to go back a tstate->cframe as we borrowed an
  // existing one when we linked our frame in.
  JIT_CHECK(
      tstate->cframe->current_frame == frame,
      "unexpected frame at top of stack");
  tstate->cframe->current_frame = frame->previous;
  return _PyEval_EvalFrameDefault(tstate, frame, err_occurred);
}

#endif

// Generate the final stage trampoline that is responsible for finishing
// execution in the interpreter and then returning the result to the caller.
void* generateDeoptTrampoline(bool generator_mode) {
  CodeHolder code;
  code.init(CodeAllocator::get()->asmJitEnvironment());
  x86::Builder a(&code);
  Annotations annot;

  auto annot_cursor = a.cursor();
  // When we get here the stack has the following layout. The space on the
  // stack for the call arg buffer / LOAD_METHOD scratch space is always safe
  // to read, but its contents will depend on the function being compiled as
  // well as the program point at which deopt occurs. We pass a pointer to it
  // into the frame reification code so that it can properly reconstruct the
  // interpreter's stack when the the result of a LOAD_METHOD is on the
  // stack. See the comments in reifyStack in deopt.cpp for more details.
  //
  // +-------------------------+
  // | ...                     |
  // | ? call arg buffer       |
  // | ^ LOAD_METHOD scratch   |
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved rip               |
  // | padding                 |
  // | address of epilogue     |
  // | r15                     | <-- rsp
  // +-------------------------+
  //
  // Save registers for use in frame reification. Once these are saved we're
  // free to clobber any caller-saved registers.
  //
  // IF YOU USE CALLEE-SAVED REGISTERS YOU HAVE TO RESTORE THEM MANUALLY BEFORE
  // THE EXITING THE TRAMPOLINE.
  a.push(x86::r14);
  a.push(x86::r13);
  a.push(x86::r12);
  a.push(x86::r11);
  a.push(x86::r10);
  a.push(x86::r9);
  a.push(x86::r8);
  a.push(x86::rdi);
  a.push(x86::rsi);
  a.push(x86::rbp);
  a.push(x86::rsp);
  a.push(x86::rbx);
  a.push(x86::rdx);
  a.push(x86::rcx);
  a.push(x86::rax);

  if (generator_mode) {
    // Restore original RBP for use in epilogue.
    RestoreOriginalGeneratorRBP(a.as<x86::Emitter>());
  }

  annot.add("Save registers", &a, annot_cursor);

  // Set up a stack frame for the trampoline so that:
  //
  // 1. Runtime code in the JIT that is used to update PyFrameObjects can find
  //    the saved rip at the expected location immediately following the end of
  //    the JIT's fixed frame.
  // 2. The JIT-compiled function shows up in C stack straces when it is
  //    deopting. Only the deopt trampoline will appear in the trace if
  //    we don't open a frame.
  //
  // Right now the stack has the following layout:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved rip               |
  // | padding                 |
  // | address of epilogue     |
  // | r15                     |
  // | ...                     |
  // | rax                     | <-- rsp
  // +-------------------------+
  //
  // We want our frame to look like:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | saved rip               |
  // | saved rbp               | <-- rbp
  // | index of deopt metadata |
  // | address of epilogue     |
  // | r15                     |
  // | ...                     |
  // | rax                     | <-- rsp
  // +-------------------------+
  //
  // Load the saved rip passed to us from the JIT-compiled function, which
  // resides where we're supposed to save rbp.
  annot_cursor = a.cursor();
  auto saved_rbp_addr = x86::ptr(x86::rsp, (NUM_GP_REGS + 2) * kPointerSize);
  a.mov(x86::rdi, saved_rbp_addr);
  // Save rbp and set up our frame
  a.mov(saved_rbp_addr, x86::rbp);
  a.lea(x86::rbp, saved_rbp_addr);
  // Load the index of the deopt metadata, which resides where we're supposed to
  // save rip.
  auto saved_rip_addr = x86::ptr(x86::rbp, kPointerSize);
  a.mov(x86::rsi, saved_rip_addr);
  a.mov(saved_rip_addr, x86::rdi);
  // Save the index of the deopt metadata
  auto deopt_meta_addr = x86::ptr(x86::rbp, -kPointerSize);
  a.mov(deopt_meta_addr, x86::rsi);
  annot.add("Shuffle rip, rbp, and deopt index", &a, annot_cursor);

  // Prep the frame for evaluation in the interpreter.
  //
  // We pass the array of saved registers, a pointer to the runtime, the index
  // of deopt metadata, and the call method kind.
  annot_cursor = a.cursor();
  a.mov(x86::rdi, x86::rsp);
  a.mov(x86::rsi, reinterpret_cast<uint64_t>(Runtime::get()));
  a.mov(x86::rdx, deopt_meta_addr);
  static_assert(
      std::is_same_v<
          decltype(prepareForDeopt),
          CiPyFrameObjType*(const uint64_t*, Runtime*, std::size_t)>,
      "prepareForDeopt has unexpected signature");
  a.call(reinterpret_cast<uint64_t>(prepareForDeopt));

  // Clean up saved registers.
  //
  // This isn't strictly necessary but saves 128 bytes on the stack if we end
  // up resuming in the interpreter.
  a.add(x86::rsp, (NUM_GP_REGS - 1) * kPointerSize);
  // We have to restore our scratch register manually since it's callee-saved
  // and the stage 2 trampoline used it to hold the address of this
  // trampoline. We can't rely on the JIT epilogue to restore it for us, as the
  // JIT-compiled code may not have spilled it.
  a.pop(deopt_scratch_reg);
  annot.add("prepareForDeopt", &a, annot_cursor);

  // Resume execution in the interpreter.
  annot_cursor = a.cursor();
  // First argument: frame returned from prepareForDeopt.
  a.mov(x86::rdi, x86::rax);
  // Second argument: runtime.
  a.mov(x86::rsi, reinterpret_cast<uint64_t>(Runtime::get()));
  // Third argument: DeoptMetadata index.
  a.mov(x86::rdx, x86::ptr(x86::rsp, kPointerSize));
  static_assert(
      std::is_same_v<
          decltype(resumeInInterpreter),
          PyObject*(CiPyFrameObjType*, Runtime*, std::size_t)>,
      "resumeInInterpreter has unexpected signature");
  a.call(reinterpret_cast<uint64_t>(resumeInInterpreter));

  // If we return a primitive and prepareForDeopt returned null, we need that
  // null in edx/xmm1 to signal error to our caller. Since this trampoline is
  // shared, we do this move unconditionally, but even if not needed, it's
  // harmless. (To eliminate it, we'd need another trampoline specifically for
  // deopt of primitive-returning functions, just to do this one move.)
  a.mov(x86::edx, x86::eax);
  a.movq(x86::xmm1, x86::eax);

  annot.add("resumeInInterpreter", &a, annot_cursor);

  // Now we're done. Get the address of the epilogue and jump there.
  annot_cursor = a.cursor();

  auto epilogue_addr = x86::ptr(x86::rbp, -2 * kPointerSize);
  a.mov(x86::rdi, epilogue_addr);
  // Remove our frame from the stack
  a.leave();
  // Clear the saved rip. Normally this would be handled by a `ret`; we must
  // clear it manually because we're jumping directly to the epilogue.
  a.sub(x86::rsp, -kPointerSize);
  a.jmp(x86::rdi);
  annot.add("Jump to real epilogue", &a, annot_cursor);

  auto name =
      generator_mode ? "deopt_trampoline_generators" : "deopt_trampoline";
  void* result{nullptr};
  ASM_CHECK(a.finalize(), name);
  ASM_CHECK(CodeAllocator::get()->addCode(&result, &code), name);
  JIT_LOGIF(
      g_dump_asm,
      "Disassembly for {}\n{}",
      name,
      annot.disassemble(result, code));

  auto code_size = code.codeSize();
  register_raw_debug_symbol(name, __FILE__, __LINE__, result, code_size, 0);

  std::vector<std::pair<void*, std::size_t>> code_sections;
  populateCodeSections(code_sections, code, result);
  code_sections.emplace_back(result, code_size);
  perf::registerFunction(code_sections, name);
  return result;
}

void* generateFailedDeferredCompileTrampoline() {
  CodeHolder code;
  code.init(CodeAllocator::get()->asmJitEnvironment());
  x86::Builder a(&code);
  Annotations annot;

  auto annot_cursor = a.cursor();

  a.push(x86::rbp);
  a.mov(x86::rbp, x86::rsp);

  // save incoming arg registers
  a.push(x86::r9);
  a.push(x86::r8);
  a.push(x86::rcx);
  a.push(x86::rdx);
  a.push(x86::rsi);
  a.push(x86::rdi);

  annot.add("saveRegisters", &a, annot_cursor);

  // r10 contains the function object from our stub
  a.mov(x86::rdi, x86::r10);
  a.mov(x86::rsi, x86::rsp);
  a.call(reinterpret_cast<uint64_t>(JITRT_FailedDeferredCompileShim));
  a.leave();
  a.ret();

  const char* name = "failedDeferredCompileTrampoline";
  ASM_CHECK(a.finalize(), name);
  void* result{nullptr};
  ASM_CHECK(CodeAllocator::get()->addCode(&result, &code), name);

  JIT_LOGIF(
      g_dump_asm,
      "Disassembly for {}\n{}",
      name,
      annot.disassemble(result, code));

  auto code_size = code.textSection()->realSize();
  register_raw_debug_symbol(name, __FILE__, __LINE__, result, code_size, 0);
  std::vector<std::pair<void*, std::size_t>> code_sections;
  forEachSection([&](CodeSection section) {
    auto asmjit_section = code.sectionByName(codeSectionName(section));
    if (asmjit_section == nullptr || asmjit_section->realSize() == 0) {
      return;
    }
    auto section_start = static_cast<char*>(result) + asmjit_section->offset();
    code_sections.emplace_back(
        reinterpret_cast<void*>(section_start), asmjit_section->realSize());
  });
  perf::registerFunction(code_sections, name);

  return result;
}

} // namespace

NativeGenerator::NativeGenerator(const hir::Function* func)
    : NativeGenerator{
          func,
          generateDeoptTrampoline(false),
          generateDeoptTrampoline(true),
          generateFailedDeferredCompileTrampoline()} {}

NativeGenerator::NativeGenerator(
    const hir::Function* func,
    void* deopt_trampoline,
    void* deopt_trampoline_generators,
    void* failed_deferred_compile_trampoline)
    : func_{func},
      deopt_trampoline_{deopt_trampoline},
      deopt_trampoline_generators_{deopt_trampoline_generators},
      failed_deferred_compile_trampoline_{failed_deferred_compile_trampoline},
#if PY_VERSION_HEX < 0x030C0000
      frame_header_size_{calcFrameHeaderSize(func)},
#endif
      max_inline_depth_{calcMaxInlineDepth(func)} {
  env_.has_inlined_functions = max_inline_depth_ > 0;
}

void NativeGenerator::generateEpilogueUnlinkFrame(
    x86::Gp tstate_r,
    bool is_generator) {
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

// these functions call int returning functions and convert their output from
// int (32 bits) to uint64_t (64 bits). This is solely because the code
// generator cannot support an operand size other than 64 bits at this moment. A
// future diff will make it support different operand sizes so that this
// function can be removed.
extern "C" uint64_t
_Invoke_PyObject_SetAttr(PyObject* v, PyObject* name, PyObject* value) {
  return PyObject_SetAttr(v, name, value);
}

extern "C" uint64_t
_Invoke_PyObject_SetItem(PyObject* container, PyObject* sub, PyObject* value) {
  return PyObject_SetItem(container, sub, value);
}

class AsmJitException : public std::exception {
 public:
  AsmJitException(Error err, std::string expr, std::string message) noexcept
      : err(err), expr(std::move(expr)), message(std::move(message)) {}

  const char* what() const noexcept override {
    return message.c_str();
  }

  Error const err;
  std::string const expr;
  std::string const message;
};

class ThrowableErrorHandler : public ErrorHandler {
 public:
  void handleError(Error err, const char* message, BaseEmitter*) override {
    throw AsmJitException(err, "<unknown>", message);
  }
};

#ifdef __ASM_DEBUG
extern "C" void ___debug_helper(const char* name) {
  fprintf(stderr, "Entering %s...\n", name);
}
#endif

PhyLocation get_arg_location_phy_location(int arg) {
  if (static_cast<size_t>(arg) < ARGUMENT_REGS.size()) {
    return ARGUMENT_REGS[arg];
  }

  JIT_ABORT("only six first registers should be used");
  return 0;
}

std::span<const std::byte> NativeGenerator::getCodeBuffer() const {
  return std::span{
      reinterpret_cast<const std::byte*>(code_start_), compiled_size_};
}

void* NativeGenerator::getVectorcallEntry() {
  if (vectorcall_entry_ != nullptr) {
    // already compiled
    return vectorcall_entry_;
  }

  JIT_CHECK(as_ == nullptr, "x86::Builder should not have been initialized.");

  CodeHolder code;
  code.init(CodeAllocator::get()->asmJitEnvironment());
  ThrowableErrorHandler eh;
  code.setErrorHandler(&eh);

  if (getConfig().multiple_code_sections) {
    Section* cold_text;
    ASM_CHECK_THROW(code.newSection(
        &cold_text,
        codeSectionName(CodeSection::kCold),
        SIZE_MAX,
        code.textSection()->flags(),
        code.textSection()->alignment()));
  }

  as_ = new x86::Builder(&code);

  env_.as = as_;
  env_.hard_exit_label = as_->newLabel();
  env_.gen_resume_entry_label = as_->newLabel();

  // Prepare the location for where our arguments will go.  This just
  // uses general purpose registers while available for non-floating
  // point values, and floating point values while available for fp
  // arguments.
  const std::vector<TypedArgument>& checks = GetFunction()->typed_args;

  // gp_index starts at 1 because the first argument is reserved for the
  // function
  for (size_t i = 0, check_index = 0, gp_index = 1, fp_index = 0;
       i < static_cast<size_t>(GetFunction()->numArgs());
       i++) {
    auto add_gp = [&]() {
      if (gp_index < ARGUMENT_REGS.size()) {
        env_.arg_locations.push_back(ARGUMENT_REGS[gp_index++]);
      } else {
        env_.arg_locations.push_back(PhyLocation::REG_INVALID);
      }
    };

    if (check_index < checks.size() &&
        checks[check_index].locals_idx == static_cast<int>(i)) {
      if (checks[check_index].jit_type <= TCDouble) {
        if (fp_index < FP_ARGUMENT_REGS.size()) {
          env_.arg_locations.push_back(FP_ARGUMENT_REGS[fp_index++]);
        } else {
          // The register will come in on the stack, and the backend
          // will access it via __asm_extra_args.
          env_.arg_locations.push_back(PhyLocation::REG_INVALID);
        }
      } else {
        add_gp();
      }
      check_index++;
      continue;
    }

    add_gp();
  }

  auto func = GetFunction();

  env_.rt = Runtime::get();
  env_.code_rt =
      env_.rt->allocateCodeRuntime(func->code, func->builtins, func->globals);

  for (auto& ref : func->env.references()) {
    env_.code_rt->addReference(ref);
  }

  lir::LIRGenerator lirgen(GetFunction(), &env_);
  std::unique_ptr<lir::Function> lir_func;

  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "Lowering into LIR",
      lir_func = lirgen.TranslateFunction())

  if (!g_dump_hir_passes_json.empty()) {
    lir::JSONPrinter lir_printer;
    (*json)["cols"].emplace_back(lir_printer.print(*lir_func, "Initial LIR"));
  }

  JIT_LOGIF(
      g_dump_lir,
      "LIR for {} after generation:\n{}",
      GetFunction()->fullname,
      *lir_func);

  PostGenerationRewrite post_gen(lir_func.get(), &env_);
  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "LIR transformations",
      post_gen.run())

  JIT_LOGIF(
      g_dump_lir,
      "LIR for {} after postgen rewrites:\n{}",
      GetFunction()->fullname,
      *lir_func);

  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "DeadCodeElimination",
      eliminateDeadCode(lir_func.get()))

  LinearScanAllocator lsalloc(
      lir_func.get(),
#if PY_VERSION_HEX < 0x030C0000
      frame_header_size_ + max_inline_depth_ * kJITShadowFrameSize);
#else
      0);
  UPGRADE_NOTE(SUPPORT_JIT_INLINING, T198250666)
#endif
  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "Register Allocation",
      lsalloc.run())

  if (!g_dump_hir_passes_json.empty()) {
    lir::JSONPrinter lir_printer;
    (*json)["cols"].emplace_back(
        lir_printer.print(*lir_func, "Register-allocated LIR"));
  }

  env_.shadow_frames_and_spill_size = lsalloc.getFrameSize();
  env_.changed_regs = lsalloc.getChangedRegs();
  env_.exit_label = as_->newLabel();
  env_.exit_for_yield_label = as_->newLabel();
  env_.frame_mode = GetFunction()->frameMode;
  if (GetFunction()->code->co_flags & kCoFlagsAnyGenerator) {
    env_.initial_yield_spill_size_ = lsalloc.initialYieldSpillSize();
  }

  JIT_LOGIF(
      g_dump_lir,
      "LIR for {} after register allocation:\n{}",
      GetFunction()->fullname,
      *lir_func);

  PostRegAllocRewrite post_rewrite(lir_func.get(), &env_);
  COMPILE_TIMER(
      GetFunction()->compilation_phase_timer,
      "Post Reg Alloc Rewrite",
      post_rewrite.run())

  JIT_LOGIF(
      g_dump_lir,
      "LIR for {} after postalloc rewrites:\n{}",
      GetFunction()->fullname,
      *lir_func);

  if (!verifyPostRegAllocInvariants(lir_func.get(), std::cerr)) {
    JIT_ABORT(
        "LIR for {} failed verification:\n{}",
        GetFunction()->fullname,
        *lir_func);
  }

  lir_func_ = std::move(lir_func);

  try {
    COMPILE_TIMER(
        GetFunction()->compilation_phase_timer,
        "Code Generation",
        generateCode(code))
  } catch (const AsmJitException& ex) {
    String s;
    FormatOptions formatOptions;
    Formatter::formatNodeList(s, formatOptions, as_);
    JIT_ABORT(
        "Failed to emit code for '{}': '{}' failed with '{}'\n\n"
        "Builder contents on failure:\n{}",
        GetFunction()->fullname,
        ex.expr,
        ex.message,
        s.data());
  }

  /* After code generation CodeHolder->codeSize() *should* return the actual
   * size of the generated code. This relies on the implementation of
   * JitRuntime::_add and may break in the future.
   */

  JIT_DCHECK(code.codeSize() < INT_MAX, "Code size is larger than INT_MAX");
  compiled_size_ = code.codeSize();
  env_.code_rt->set_frame_size(env_.stack_frame_size);
  return vectorcall_entry_;
}

void* NativeGenerator::getStaticEntry() {
  if (!hasStaticEntry()) {
    return nullptr;
  }
  // Force compile, if needed.
  getVectorcallEntry();
  return reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(vectorcall_entry_) +
      JITRT_STATIC_ENTRY_OFFSET);
}

int NativeGenerator::GetCompiledFunctionStackSize() const {
  return env_.stack_frame_size;
}

int NativeGenerator::GetCompiledFunctionSpillStackSize() const {
  return spill_stack_size_;
}

void NativeGenerator::generateFunctionEntry() {
  as_->push(x86::rbp);
  as_->mov(x86::rbp, x86::rsp);
}

void NativeGenerator::loadTState(x86::Gp dst_reg) {
#if PY_VERSION_HEX >= 0x030C0000
  UPGRADE_ASSERT(TSTATE_FROM_RUNTIME);
#else
  uint64_t tstate =
      reinterpret_cast<uint64_t>(&_PyRuntime.gilstate.tstate_current);
  if (fitsInt32(tstate)) {
    as_->mov(dst_reg, x86::ptr(tstate));
  } else {
    as_->mov(dst_reg, tstate);
    as_->mov(dst_reg, x86::ptr(dst_reg));
  }
#endif
}

#ifdef SHADOW_FRAMES
void NativeGenerator::linkOnStackShadowFrame(
    x86::Gp tstate_reg,
    x86::Gp scratch_reg) {
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

void NativeGenerator::initializeFrameHeader(
    x86::Gp tstate_reg,
    x86::Gp scratch_reg) {
  // Generator shadow frames live in generator objects and only get linked in
  // on the first resume.
  if (!isGen()) {
    linkOnStackShadowFrame(tstate_reg, scratch_reg);
  }
}
#endif

void NativeGenerator::setupFrameAndSaveCallerRegisters(
#ifdef SHADOW_FRAMES
    x86::Gp tstate_reg
#endif
) {
  // During execution, the stack looks like the diagram below. The column to
  // left indicates how many words on the stack each line occupies.
  //
  // Legend:
  //  - <empty> - 1 word
  //  - N       - A fixed number of words > 1
  //  - *       - 0 or more words
  //  - ?       - 0 or 1 words
  //
  // +-----------------------+
  // | * memory arguments    |
  // |   return address      |
  // |   saved rbp           | <-- rbp
  // | N frame header        | See frame.h
  // | * inl. shad. frame 0  |
  // | * inl. shad. frame 1  |
  // | * inl. shad. frame .  |
  // | * inl. shad. frame N  |
  // | * spilled values      |
  // | ? alignment padding   |
  // | * callee-saved regs   |
  // | ? call arg buffer     | <-- rsp
  // +-----------------------+
  auto saved_regs = env_.changed_regs & CALLEE_SAVE_REGS;
  int saved_regs_size = saved_regs.count() * 8;
  // Make sure we have at least one word for scratch in the epilogue.
  spill_stack_size_ = env_.shadow_frames_and_spill_size;
  // The frame header size and inlined shadow frames are already included in
  // env_.spill_size.
  int spill_stack = std::max(spill_stack_size_, 8);

  int arg_buffer_size = env_.max_arg_buffer_size;

  if ((spill_stack + saved_regs_size + arg_buffer_size) % 16 != 0) {
    spill_stack += 8;
  }

  // Allocate stack space and save the size of the function's stack.
  as_->sub(x86::rsp, spill_stack);
  env_.last_callee_saved_reg_off = spill_stack + saved_regs_size;

#ifdef SHADOW_FRAMES
  x86::Gp scratch_reg = x86::rax;
  as_->push(scratch_reg);
  initializeFrameHeader(tstate_reg, scratch_reg);
  as_->pop(scratch_reg);
#endif

  // Push used callee-saved registers.
  while (!saved_regs.Empty()) {
    as_->push(x86::gpq(saved_regs.GetFirst().loc));
    saved_regs.RemoveFirst();
  }

  if (arg_buffer_size > 0) {
    as_->sub(x86::rsp, arg_buffer_size);
  }

  env_.stack_frame_size = spill_stack + saved_regs_size + arg_buffer_size;
}

x86::Gp get_arg_location(int arg) {
  auto phyloc = get_arg_location_phy_location(arg);

  if (phyloc.is_register()) {
    return x86::gpq(phyloc.loc);
  }

  JIT_ABORT("should only be used with first six args");
}

constexpr size_t kConstStackAlignmentRequirement = 16;

void NativeGenerator::loadOrGenerateLinkFrame(
#if PY_VERSION_HEX >= 0x030C0000
    asmjit::x86::Gp func_reg,
#endif
    const std::vector<
        std::pair<const asmjit::x86::Reg&, const asmjit::x86::Reg&>>&
        save_regs) {
  x86::Gp tstate_reg = x86::gpq(INITIAL_TSTATE_REG.loc);
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

#if PY_VERSION_HEX < 0x030C0000
  // Prior to 3.12 we did not link a frame on initial generator entry.
  if (isGen()) {
    load_tstate_and_move();
    return;
  }
#endif

  switch (GetFunction()->frameMode) {
    case FrameMode::kShadow:
      load_tstate_and_move();
      break;
    case FrameMode::kNormal: {
      size_t rsp_offset = 0;
      for (const auto& pair : save_regs) {
        if (pair.first.isGpq()) {
          as_->push((asmjit::x86::Gpq&)pair.first);
        } else if (pair.first.isXmm()) {
          as_->sub(x86::rsp, pair.first.size());
          as_->movdqu(x86::dqword_ptr(x86::rsp), (asmjit::x86::Xmm&)pair.first);
        } else {
          JIT_ABORT("unsupported saved register type");
        }
        rsp_offset += pair.first.size();
      }
      bool align_stack = rsp_offset % kConstStackAlignmentRequirement;
      if (align_stack) {
        as_->push(x86::rax);
      }

#if PY_VERSION_HEX < 0x030C0000
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
#else
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
          as_->mov(
              x86::rsi, reinterpret_cast<intptr_t>(GetFunction()->code.get()));
          as_->call(reinterpret_cast<uint64_t>(
              JITRT_AllocateAndLinkInterpreterFrame_Debug));
        } else {
          as_->call(reinterpret_cast<uint64_t>(
              JITRT_AllocateAndLinkInterpreterFrame_Release));
        }
      }
#endif
      as_->mov(tstate_reg, x86::rax);

      if (align_stack) {
        as_->pop(x86::rax);
      }
      for (auto iter = save_regs.rbegin(); iter != save_regs.rend(); ++iter) {
        if (iter->second.isGpq()) {
          as_->pop((asmjit::x86::Gpq&)iter->second);
        } else if (iter->second.isXmm()) {
          as_->movdqu(
              (asmjit::x86::Xmm&)iter->second, x86::dqword_ptr(x86::rsp));
          as_->add(x86::rsp, 16);
        } else {
          JIT_ABORT("unsupported saved register type");
        }
      }
      break;
    }
  }
}

void NativeGenerator::generatePrologue(
    Label correct_arg_count,
    Label native_entry_point) {
  // The boxed return wrapper gets generated first, if it is necessary.
  auto [generic_entry_cursor, box_entry_cursor] = generateBoxedReturnWrapper();

  generateFunctionEntry();

  // Verify arguments have been passed in correctly.
  if (func_->has_primitive_args) {
    generatePrimitiveArgsPrologue();
  } else {
    generateArgcountCheckPrologue(correct_arg_count);
  }
  as_->bind(correct_arg_count);

  Label setup_frame = as_->newLabel();

  if (hasStaticEntry()) {
    if (!func_->has_primitive_args) {
      // We weren't called statically, but we've now resolved all arguments to
      // fixed offsets.  Validate that the arguments are correctly typed.
      generateStaticMethodTypeChecks(setup_frame);
    } else if (func_->has_primitive_first_arg) {
      as_->mov(x86::rdx, 0);
    }
  }

  env_.addAnnotation("Generic entry", generic_entry_cursor);

  if (box_entry_cursor) {
    env_.addAnnotation(
        "Generic entry (box primitive return)", box_entry_cursor);
  }

  // Args are now validated, setup frame.
  constexpr auto kFuncPtrReg = x86::gpq(INITIAL_FUNC_REG.loc);
  constexpr auto kArgsReg = x86::gpq(INITIAL_EXTRA_ARGS_REG.loc);
  constexpr auto kArgsPastSixReg = kArgsReg;

  asmjit::BaseNode* frame_cursor = as_->cursor();
  as_->bind(setup_frame);
  std::vector<std::pair<const x86::Reg&, const x86::Reg&>> save_regs;
  save_regs.emplace_back(x86::rsi, kArgsReg);
  if (GetFunction()->uses_runtime_func) {
    save_regs.emplace_back(x86::rdi, kFuncPtrReg);
  }
  loadOrGenerateLinkFrame(
#if PY_VERSION_HEX >= 0x030C0000
      kFuncPtrReg,
#endif
      save_regs);
  env_.addAnnotation("Link frame", frame_cursor);

  asmjit::BaseNode* load_args_cursor = as_->cursor();
  // Move arguments into their expected registers and then set a register as the
  // base for additional args.
  bool has_extra_args = false;
  for (size_t i = 0; i < env_.arg_locations.size(); i++) {
    PhyLocation arg = env_.arg_locations[i];
    if (arg == PhyLocation::REG_INVALID) {
      has_extra_args = true;
      continue;
    }
    if (arg.is_gp_register()) {
      as_->mov(x86::gpq(arg.loc), x86::ptr(kArgsReg, i * sizeof(void*)));
    } else {
      as_->movsd(x86::xmm(arg.loc), x86::ptr(kArgsReg, i * sizeof(void*)));
    }
  }
  if (has_extra_args) {
    // Load the location of the remaining args, the backend will deal with
    // loading them from here...
    as_->lea(
        kArgsPastSixReg,
        x86::ptr(kArgsReg, (ARGUMENT_REGS.size() - 1) * sizeof(void*)));
  }
  env_.addAnnotation("Load arguments", load_args_cursor);

  // Finally allocate the saved space required for the actual function.
  auto native_entry_cursor = as_->cursor();
  as_->bind(native_entry_point);

  setupFrameAndSaveCallerRegisters(
#ifdef SHADOW_FRAMES
      x86::r11
#endif
  );

  env_.addAnnotation("Native entry", native_entry_cursor);
}

static void
emitCompare(x86::Builder* as, x86::Gp lhs, void* rhs, x86::Gp scratch) {
  uint64_t rhsi = reinterpret_cast<uint64_t>(rhs);
  if (!fitsInt32(rhsi)) {
    // in shared mode type can be in a high address
    as->mov(scratch, rhsi);
    as->cmp(lhs, scratch);
  } else {
    as->cmp(lhs, rhsi);
  }
}

void NativeGenerator::generateStaticMethodTypeChecks(Label setup_frame) {
  // JITRT_CallWithIncorrectArgcount uses the fact that our checks are set up
  // from last to first argument - we order the jumps so that the common case of
  // no defaulted arguments comes first, and end up with the following
  // structure: generic entry: compare defaulted arg count to 0 if zero: go to
  // first check compare defaulted arg count to 1 if zero: go to second check
  // ...
  // This is complicated a bit by the fact that not every argument will have a
  // check, as we elide the dynamic ones. For that, we do bookkeeping and assign
  // all defaulted arg counts up to the next local to the same label.
  const std::vector<TypedArgument>& checks = GetFunction()->typed_args;
  env_.static_arg_typecheck_failed_label = as_->newLabel();
  if (!checks.size()) {
    return;
  }
  // We build a vector of labels corresponding to [first_check, second_check,
  // ..., setup_frame] which will have |checks| + 1 elements, and the
  // first_check label will precede the first check.
  auto table_label = as_->newLabel();
  as_->lea(x86::r8, x86::ptr(table_label));
  as_->lea(x86::r8, x86::ptr(x86::r8, x86::rcx, 3));
  as_->jmp(x86::r8);
  auto jump_table_cursor = as_->cursor();
  as_->align(AlignMode::kCode, 8);
  as_->bind(table_label);
  std::vector<Label> arg_labels;
  int defaulted_arg_count = 0;
  Py_ssize_t check_index = checks.size() - 1;
  // Each check might be a label that hosts multiple arguments, as dynamic
  // arguments aren't checked. We need to account for this in our bookkeeping.
  auto next_arg = as_->newLabel();
  arg_labels.emplace_back(next_arg);
  while (defaulted_arg_count < GetFunction()->numArgs()) {
    as_->align(AlignMode::kCode, 8);
    as_->jmp(next_arg);

    if (check_index >= 0) {
      long local = checks.at(check_index).locals_idx;
      if (GetFunction()->numArgs() - defaulted_arg_count - 1 == local) {
        if (check_index == 0) {
          next_arg = setup_frame;
        } else {
          check_index--;
          next_arg = as_->newLabel();
        }
        arg_labels.emplace_back(next_arg);
      }
    }

    defaulted_arg_count++;
  }
  env_.addAnnotation(
      fmt::format("Jump to first non-defaulted argument"), jump_table_cursor);

  as_->align(AlignMode::kCode, 8);
  as_->bind(arg_labels[0]);
  for (Py_ssize_t i = checks.size() - 1; i >= 0; i--) {
    auto check_cursor = as_->cursor();
    const TypedArgument& arg = checks.at(i);
    env_.code_rt->addReference(BorrowedRef(arg.pytype));
    next_arg = arg_labels[checks.size() - i];

    as_->mov(x86::r8, x86::ptr(x86::rsi, arg.locals_idx * 8)); // load local
    as_->mov(
        x86::r8, x86::ptr(x86::r8, offsetof(PyObject, ob_type))); // load type
    if (arg.optional) {
      // check if the value is None
      emitCompare(as_, x86::r8, Py_TYPE(Py_None), x86::rax);
      as_->je(next_arg);
    }

    // common case: check if we have the exact right type
    emitCompare(as_, x86::r8, arg.pytype, x86::rax);
    as_->je(next_arg);

    if (!arg.exact && (arg.threadSafeTpFlags() & Py_TPFLAGS_BASETYPE)) {
      // We need to check the object's MRO and see if the declared type
      // is present in it.  Technically we don't need to check the last
      // entry that will be object but the code gen is a little bit simpler
      // if we include it.
      Label arg_loop = as_->newLabel();
      as_->mov(x86::r10, reinterpret_cast<uint64_t>(arg.pytype.get()));

      // PyObject *r8 = r8->tp_mro;
      as_->mov(x86::r8, x86::ptr(x86::r8, offsetof(PyTypeObject, tp_mro)));
      // Py_ssize_t r11 = r8->ob_size;
      as_->mov(x86::r11, x86::ptr(x86::r8, offsetof(PyVarObject, ob_size)));
      // PyObject *r8 = &r8->ob_item[0];
      as_->add(x86::r8, offsetof(PyTupleObject, ob_item));
      // PyObject *r11 = &r8->ob_item[r11];
      as_->lea(x86::r11, x86::ptr(x86::r8, x86::r11, 3));

      as_->bind(arg_loop);
      as_->cmp(x86::ptr(x86::r8), x86::r10);
      as_->je(next_arg);
      as_->add(x86::r8, sizeof(PyObject*));
      as_->cmp(x86::r8, x86::r11);
      as_->jne(arg_loop);
    }

    // no args match, bail to normal vector call to report error
    as_->jmp(env_.static_arg_typecheck_failed_label);
    bool last_check = i == 0;
    if (!last_check) {
      as_->bind(next_arg);
    }
    env_.addAnnotation(
        fmt::format("StaticTypeCheck[{}]", arg.pytype->tp_name), check_cursor);
  }
}

void NativeGenerator::generateEpilogue(BaseNode* epilogue_cursor) {
  as_->setCursor(epilogue_cursor);

  // now we can use all the caller save registers except for RAX
  as_->bind(env_.exit_label);

  bool is_gen = GetFunction()->code->co_flags & kCoFlagsAnyGenerator;
  if (is_gen) {
#if PY_VERSION_HEX < 0x030C0000
    // Set generator state to "completed". We access the state via RBP which
    // points to the of spill data and bottom of GenDataFooter.
    auto state_offs = offsetof(GenDataFooter, state);
    as_->mov(
        x86::ptr(x86::rbp, state_offs, sizeof(GenDataFooter::state)),
        Ci_JITGenState_Completed);
#else
    // ((GenDataFooter*)rbp)->gen->gi_frame_state = FRAME_COMPLETED
    // RDX is an arbitrary scratch register - any caller saved reg is fine.
    auto gen_offs = offsetof(GenDataFooter, gen);
    as_->mov(x86::rdx, x86::ptr(x86::rbp, gen_offs));
    as_->mov(
        x86::ptr(
            x86::rdx,
            offsetof(PyGenObject, gi_frame_state),
            sizeof(PyGenObject::gi_frame_state)),
        FRAME_COMPLETED);
#endif
    as_->bind(env_.exit_for_yield_label);
    RestoreOriginalGeneratorRBP(as_->as<x86::Emitter>());
  }

#if PY_VERSION_HEX >= 0x030C0000
  // Generator frame linkage for resumed generators is handled by the generator
  // object i.e. in generators_rt. For the initial yield unlinking happens as
  // part of the YieldInitial LIR instruction.
  if (!is_gen) {
    generateEpilogueUnlinkFrame(x86::rdi, false);
  }
#else
  // Ideally this would also be the same in 3.10 as well but I spent maybe half
  // a day trying to change things and gave up. Our implementation is really
  // wonky and a clear ownership model is made difficult by shadow frames. It's
  // probably subtly broken somewhere.
  generateEpilogueUnlinkFrame(x86::rdi, is_gen);
#endif

  // If we return a primitive, set edx/xmm1 to 1 to indicate no error (in case
  // of error, deopt will set it to 0 and jump to hard_exit_label, skipping
  // this.)
  if (func_->returnsPrimitive()) {
    JIT_CHECK(!is_gen, "generators can't return primitives");
    if (func_->returnsPrimitiveDouble()) {
      // Loads an *integer* 1 in XMM1.. value doesn't matter,
      // but it needs to be non-zero. See pg 124,
      // https://www.agner.org/optimize/optimizing_assembly.pdf
      as_->pcmpeqw(x86::xmm1, x86::xmm1);
      as_->psrlq(x86::xmm1, 63);
    } else {
      as_->mov(x86::edx, 1);
    }
  }

  as_->bind(env_.hard_exit_label);
  asmjit::BaseNode* epilogue_error_cursor = as_->cursor();

  auto saved_regs = env_.changed_regs & CALLEE_SAVE_REGS;
  if (!saved_regs.Empty()) {
    // Reset rsp to point at our callee-saved registers and restore them.
    JIT_CHECK(
        env_.last_callee_saved_reg_off != -1,
        "offset to callee saved regs not initialized");
    as_->lea(x86::rsp, x86::ptr(x86::rbp, -env_.last_callee_saved_reg_off));

    std::vector<int> pop_regs;
    while (!saved_regs.Empty()) {
      int reg = saved_regs.GetFirst().loc;
      pop_regs.push_back(reg);
      saved_regs.RemoveFirst();
    }
    for (auto riter = pop_regs.rbegin(); riter != pop_regs.rend(); ++riter) {
      as_->pop(x86::gpq(*riter));
    }
  }

  as_->leave();
  as_->ret();

  env_.addAnnotation(
      "Epilogue (restore regs; pop native frame; error exit)",
      epilogue_error_cursor);
  env_.addAnnotation("Epilogue", epilogue_cursor);
  if (env_.function_indirections.size()) {
    auto jit_helpers = as_->cursor();
    for (auto& x : env_.function_indirections) {
      Label trampoline = as_->newLabel();
      as_->bind(trampoline);
      as_->mov(x86::r10, reinterpret_cast<uint64_t>(x.first));
      as_->jmp(reinterpret_cast<uint64_t>(failed_deferred_compile_trampoline_));
      x.second.trampoline = trampoline;
    }
    env_.addAnnotation("JitHelpers", jit_helpers);
  }
}

void NativeGenerator::generateDeoptExits(const asmjit::CodeHolder& code) {
  if (env_.deopt_exits.empty()) {
    return;
  }

  // Always place the deopt exit call to the cold section, and revert to the
  // previous section at the end of this scope.
  CodeSectionOverride override{as_, &code, &metadata_, CodeSection::kCold};

  auto& deopt_exits = env_.deopt_exits;

  auto deopt_cursor = as_->cursor();
  auto deopt_exit = as_->newLabel();
  std::sort(deopt_exits.begin(), deopt_exits.end(), [](auto& a, auto& b) {
    return a.deopt_meta_index < b.deopt_meta_index;
  });
  // Generate stage 1 trampolines (one per guard). These push the index of the
  // appropriate `DeoptMetadata` and then jump to the stage 2 trampoline.
  for (const auto& exit : deopt_exits) {
    as_->bind(exit.label);
    as_->push(exit.deopt_meta_index);
    emitCall(env_, deopt_exit, exit.instr);
  }
  // Generate the stage 2 trampoline (one per function). This saves the address
  // of the final part of the JIT-epilogue that is responsible for restoring
  // callee-saved registers and returning, our scratch register, whose original
  // contents may be needed during frame reification, and jumps to the final
  // trampoline.
  //
  // Right now the top of the stack looks like:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved rip               |
  // +-------------------------+
  //
  // and we need to pass our scratch register and the address of the epilogue
  // to the global deopt trampoline. The code below leaves the stack with the
  // following layout:
  //
  // +-------------------------+ <-- end of JIT's fixed frame
  // | index of deopt metadata |
  // | saved rip               |
  // | padding                 |
  // | address of epilogue     |
  // | r15                     |
  // +-------------------------+
  //
  // The global deopt trampoline expects that our scratch register is at the
  // top of the stack so that it can save the remaining registers immediately
  // after it, forming a contiguous array of all registers.
  //
  // If you change this make sure you update that code!
  as_->bind(deopt_exit);
  // Add padding to keep the stack aligned
  as_->push(deopt_scratch_reg);
  // Save space for the epilogue
  as_->push(deopt_scratch_reg);
  // Save our scratch register
  as_->push(deopt_scratch_reg);
  // Save the address of the epilogue
  as_->lea(deopt_scratch_reg, x86::ptr(env_.hard_exit_label));
  as_->mov(x86::ptr(x86::rsp, kPointerSize), deopt_scratch_reg);
  auto trampoline = GetFunction()->code->co_flags & kCoFlagsAnyGenerator
      ? deopt_trampoline_generators_
      : deopt_trampoline_;
  as_->mov(deopt_scratch_reg, reinterpret_cast<uint64_t>(trampoline));
  as_->jmp(deopt_scratch_reg);
  env_.addAnnotation("Deoptimization exits", deopt_cursor);
}

void NativeGenerator::linkDeoptPatchers(const asmjit::CodeHolder& code) {
  JIT_CHECK(code.hasBaseAddress(), "code not generated!");
  uint64_t base = code.baseAddress();
  for (const auto& udp : env_.pending_deopt_patchers) {
    uint64_t patchpoint = base + code.labelOffsetFromBase(udp.patchpoint);
    uint64_t deopt_exit = base + code.labelOffsetFromBase(udp.deopt_exit);
    udp.patcher->link(patchpoint, deopt_exit);

    // Register patcher with the runtime if it is type-based.
    if (auto typed_patcher = dynamic_cast<TypeDeoptPatcher*>(udp.patcher)) {
      env_.rt->watchType(typed_patcher->type(), typed_patcher);
    }
  }
}

void NativeGenerator::generateResumeEntry() {
  // Arbitrary scratch register for use throughout this function. Can be changed
  // to pretty much anything which doesn't conflict with arg registers.
  const auto scratch_r = x86::r8;

  // arg #1 - rdi = PyGenObject/JitGenObject* generator
  // arg #2 - rsi = PyObject* sent_value
  // arg #3 - rdx = finish_yield_from
  // arg #4 - rcx = tstate
  // Arg regs must not be modified as they may be used by the next resume stage.
  auto cursor = as_->cursor();
  as_->bind(env_.gen_resume_entry_label);

  generateFunctionEntry();
  setupFrameAndSaveCallerRegisters(
#ifdef SHADOW_FRAMES
      x86::rcx
#endif
  );

  // Setup RBP to use storage in generator rather than stack.

  // Pointer to GenDataFooter. Could be any conflict-free register.
  const auto jit_data_r = x86::r9;

  // jit_data_r = gen->gi_jit_data
#if PY_VERSION_HEX < 0x030C0000
  auto gi_jit_data_offset = offsetof(PyGenObject, gi_jit_data);
  as_->mov(jit_data_r, x86::ptr(x86::rdi, gi_jit_data_offset));
#else
  // TODO(T209501671): Bake offsets in so we don't need this call.
  as_->mov(x86::rbx, x86::rsi);
  as_->mov(x86::r12, x86::rdx);
  as_->mov(x86::r13, x86::rcx);
  as_->call(reinterpret_cast<uint64_t>(JITRT_GetJITDataFromGen));
  as_->mov(x86::rsi, x86::rbx);
  as_->mov(x86::rdx, x86::r12);
  as_->mov(x86::rcx, x86::r13);
  as_->mov(jit_data_r, x86::rax);
#endif

  // Store linked frame address
  size_t link_address_offset = offsetof(GenDataFooter, linkAddress);
  as_->mov(scratch_r, x86::ptr(x86::rbp));
  as_->mov(x86::ptr(jit_data_r, link_address_offset), scratch_r);

  // Store return address
  size_t return_address_offset = offsetof(GenDataFooter, returnAddress);
  as_->mov(scratch_r, x86::ptr(x86::rbp, 8));
  as_->mov(x86::ptr(jit_data_r, return_address_offset), scratch_r);

  // Store "original" RBP
  size_t original_rbp_offset = offsetof(GenDataFooter, originalRbp);
  as_->mov(x86::ptr(jit_data_r, original_rbp_offset), x86::rbp);

  // RBP = gen->gi_jit_data
  as_->mov(x86::rbp, jit_data_r);

  // Resume generator execution: load and clear yieldPoint, then jump to the
  // resume target.
  size_t yield_point_offset = offsetof(GenDataFooter, yieldPoint);
  as_->mov(scratch_r, x86::ptr(x86::rbp, yield_point_offset));
  as_->mov(x86::qword_ptr(x86::rbp, yield_point_offset), 0);
  size_t resume_target_offset = GenYieldPoint::resumeTargetOffset();
  as_->jmp(x86::ptr(scratch_r, resume_target_offset));

  env_.addAnnotation("Resume entry point", cursor);
}

void NativeGenerator::generateStaticEntryPoint(
    Label native_entry_point,
    Label static_jmp_location) {
  // Static entry point is the first thing in the method, we'll
  // jump back to hit it so that we have a fixed offset to jump from
  auto static_link_cursor = as_->cursor();
  Label static_entry_point = as_->newLabel();
  as_->bind(static_entry_point);

  generateFunctionEntry();

  // Save incoming args across link call...
  size_t total_args = (size_t)GetFunction()->numArgs();

  const std::vector<TypedArgument>& checks = GetFunction()->typed_args;
  std::vector<std::pair<const x86::Reg&, const x86::Reg&>> save_regs;

  if (!isGen()) {
    save_regs.emplace_back(x86::rdi, x86::rdi);
    for (size_t i = 0, check_index = 0, arg_index = 0, fp_index = 0;
         i < total_args;
         i++) {
      if (check_index < checks.size() &&
          checks[check_index].locals_idx == (int)i) {
        if (checks[check_index++].jit_type <= TCDouble &&
            fp_index < FP_ARGUMENT_REGS.size()) {
          switch (FP_ARGUMENT_REGS[fp_index++].loc) {
            case XMM0.loc:
              save_regs.emplace_back(x86::xmm0, x86::xmm0);
              break;
            case XMM1.loc:
              save_regs.emplace_back(x86::xmm1, x86::xmm1);
              break;
            case XMM2.loc:
              save_regs.emplace_back(x86::xmm2, x86::xmm2);
              break;
            case XMM3.loc:
              save_regs.emplace_back(x86::xmm3, x86::xmm3);
              break;
            case XMM4.loc:
              save_regs.emplace_back(x86::xmm4, x86::xmm4);
              break;
            case XMM5.loc:
              save_regs.emplace_back(x86::xmm5, x86::xmm5);
              break;
            case XMM6.loc:
              save_regs.emplace_back(x86::xmm6, x86::xmm6);
              break;
            case XMM7.loc:
              save_regs.emplace_back(x86::xmm7, x86::xmm7);
              break;
            default:
              break;
          }
          continue;
        }
      }

      if (arg_index + 1 < ARGUMENT_REGS.size()) {
        switch (ARGUMENT_REGS[++arg_index].loc) {
          case RDI.loc:
            save_regs.emplace_back(x86::rdi, x86::rdi);
            break;
          case RSI.loc:
            save_regs.emplace_back(x86::rsi, x86::rsi);
            break;
          case RDX.loc:
            save_regs.emplace_back(x86::rdx, x86::rdx);
            break;
          case RCX.loc:
            save_regs.emplace_back(x86::rcx, x86::rcx);
            break;
          case R8.loc:
            save_regs.emplace_back(x86::r8, x86::r8);
            break;
          case R9.loc:
            save_regs.emplace_back(x86::r9, x86::r9);
            break;
          default:
            break;
        }
      }
    }
  }

#if PY_VERSION_HEX < 0x030C0000
  loadOrGenerateLinkFrame(save_regs);
#else
  UPGRADE_ASSERT(FRAME_HANDLING_CHANGED);
#endif

  if (total_args + 1 > ARGUMENT_REGS.size()) {
    as_->lea(x86::r10, x86::ptr(x86::rbp, 16));
  }
  as_->jmp(native_entry_point);
  env_.addAnnotation("StaticLinkFrame", static_link_cursor);
  auto static_entry_point_cursor = as_->cursor();

  as_->bind(static_jmp_location);
  // force a long jump even if the static entry point is small so that we get
  // a consistent offset for the static entry point from the normal entry point.
  as_->long_().jmp(static_entry_point);
  env_.addAnnotation("StaticEntryPoint", static_entry_point_cursor);
}

bool NativeGenerator::hasStaticEntry() const {
  PyCodeObject* code = GetFunction()->code;
  return (code->co_flags & CI_CO_STATICALLY_COMPILED);
}

void NativeGenerator::generateCode(CodeHolder& codeholder) {
  // The body must be generated before the prologue to determine how much spill
  // space to allocate.
  auto prologue_cursor = as_->cursor();
  generateAssemblyBody(codeholder);

  auto epilogue_cursor = as_->cursor();

  as_->setCursor(prologue_cursor);

  Label correct_arg_count = as_->newLabel();
  Label native_entry_point = as_->newLabel();
  Label static_jmp_location = as_->newLabel();

  bool has_static_entry = hasStaticEntry();
  if (has_static_entry) {
    // Setup an entry point for direct static to static
    // calls using the native calling convention
    generateStaticEntryPoint(native_entry_point, static_jmp_location);
  }

  // Setup an entry for when we have the correct number of arguments
  // This will be dispatched back to from JITRT_CallWithIncorrectArgcount and
  // JITRT_CallWithKeywordArgs when we need to perform complicated
  // argument binding.
  auto arg_reentry_cursor = as_->cursor();
  Label correct_args_entry = as_->newLabel();
  as_->bind(correct_args_entry);
  generateFunctionEntry();
  as_->short_().jmp(correct_arg_count);
  env_.addAnnotation("Reentry with processed args", arg_reentry_cursor);

  // Setup the normal entry point that expects that implements the
  // vectorcall convention
  Label vectorcall_entry_label = as_->newLabel();
  as_->bind(vectorcall_entry_label);
  generatePrologue(correct_arg_count, native_entry_point);

  generateEpilogue(epilogue_cursor);

  if (GetFunction()->code->co_flags & kCoFlagsAnyGenerator) {
    generateResumeEntry();
  }

  if (env_.static_arg_typecheck_failed_label.isValid()) {
    auto static_typecheck_cursor = as_->cursor();
    as_->bind(env_.static_arg_typecheck_failed_label);
    if (GetFunction()->returnsPrimitive()) {
      if (GetFunction()->returnsPrimitiveDouble()) {
        as_->call(reinterpret_cast<uint64_t>(
            JITRT_ReportStaticArgTypecheckErrorsWithDoubleReturn));
      } else {
        as_->call(reinterpret_cast<uint64_t>(
            JITRT_ReportStaticArgTypecheckErrorsWithPrimitiveReturn));
      }
    } else {
      as_->call(
          reinterpret_cast<uint64_t>(JITRT_ReportStaticArgTypecheckErrors));
    }
    as_->leave();
    as_->ret();
    env_.addAnnotation(
        "Static argument typecheck failure stub", static_typecheck_cursor);
  }

  generateDeoptExits(codeholder);

  ASM_CHECK_THROW(as_->finalize());
  ASM_CHECK_THROW(CodeAllocator::get()->addCode(&code_start_, &codeholder));

  // ------------- code_start_
  // ^
  // | JITRT_STATIC_ENTRY_OFFSET (2 bytes, optional)
  // | JITRT_CALL_REENTRY_OFFSET (6 bytes)
  // v
  // ------------- vectorcall_entry_
  if (has_static_entry) {
    JIT_CHECK(
        codeholder.labelOffsetFromBase(static_jmp_location) ==
            codeholder.labelOffsetFromBase(vectorcall_entry_label) +
                JITRT_STATIC_ENTRY_OFFSET,
        "bad static-entry offset {} ",
        codeholder.labelOffsetFromBase(vectorcall_entry_label) -
            codeholder.labelOffsetFromBase(static_jmp_location));
  }
  JIT_CHECK(
      codeholder.labelOffset(correct_args_entry) ==
          codeholder.labelOffset(vectorcall_entry_label) +
              JITRT_CALL_REENTRY_OFFSET,
      "bad re-entry offset");

  linkDeoptPatchers(codeholder);
  env_.code_rt->debug_info()->resolvePending(
      env_.pending_debug_locs, *GetFunction(), codeholder);

  vectorcall_entry_ = static_cast<char*>(code_start_) +
      codeholder.labelOffsetFromBase(vectorcall_entry_label);

  for (auto& entry : env_.unresolved_gen_entry_labels) {
    entry.first->setResumeTarget(
        codeholder.labelOffsetFromBase(entry.second) +
        codeholder.baseAddress());
  }

  // After code generation CodeHolder->codeSize() *should* return the actual
  // size of the generated code and associated data. This relies on the
  // implementation of asmjit::JitRuntime::_add and may break in the future.
  JIT_DCHECK(
      codeholder.codeSize() < INT_MAX, "Code size is larger than INT_MAX");
  compiled_size_ = codeholder.codeSize();

  if (!g_dump_hir_passes_json.empty()) {
    env_.annotations.disassembleJSON(*json, code_start_, codeholder);
  }

  JIT_LOGIF(
      g_dump_asm,
      "Disassembly for {}\n{}",
      GetFunction()->fullname,
      env_.annotations.disassemble(code_start_, codeholder));
  {
    ThreadedCompileSerialize guard;
    for (auto& x : env_.function_indirections) {
      Label trampoline = x.second.trampoline;
      *x.second.indirect = reinterpret_cast<void*>(
          codeholder.labelOffsetFromBase(trampoline) +
          codeholder.baseAddress());
    }
  }

  const hir::Function* func = GetFunction();
  std::string prefix = [&] {
    switch (func->frameMode) {
      case FrameMode::kNormal:
        return perf::kFuncSymbolPrefix;
      case FrameMode::kShadow:
        return perf::kShadowFrameSymbolPrefix;
    }
    JIT_ABORT("Invalid frame mode");
  }();
  // For perf, we want only the size of the code, so we get that directly from
  // the text sections.
  std::vector<std::pair<void*, std::size_t>> code_sections;
  populateCodeSections(code_sections, codeholder, code_start_);
  perf::registerFunction(code_sections, func->fullname, prefix);
}

#ifdef __ASM_DEBUG
const char* NativeGenerator::GetPyFunctionName() const {
  return PyUnicode_AsUTF8(GetFunction()->code->co_name);
}
#endif

void NativeGenerator::generateAssemblyBody(const asmjit::CodeHolder& code) {
  auto as = env_.as;
  auto& blocks = lir_func_->basicblocks();
  for (auto& basicblock : blocks) {
    env_.block_label_map.emplace(basicblock, as->newLabel());
  }

  for (lir::BasicBlock* basicblock : blocks) {
    CodeSection section = basicblock->section();
    CodeSectionOverride section_override{as, &code, &metadata_, section};
    as->bind(map_get(env_.block_label_map, basicblock));
    for (auto& instr : basicblock->instructions()) {
      asmjit::BaseNode* cursor = as->cursor();
      autogen::AutoTranslator::getInstance().translateInstr(&env_, instr.get());
      if (instr->origin() != nullptr) {
        env_.addAnnotation(instr.get(), cursor);
      }
    }
  }
}

void NativeGenerator::generatePrimitiveArgsPrologue() {
  JIT_CHECK(
      hasStaticEntry(),
      "Functions with primitive arguments must have been statically compiled");

  // If we've been invoked statically we can skip all of the argument checking
  // because we know our args have been provided correctly.  But if we have
  // primitives we need to unbox them.  We usually get to avoid this by doing
  // direct invokes from JITed code.
  BorrowedRef<_PyTypedArgsInfo> info = func_->prim_args_info;
  env_.code_rt->addReference(info);
  as_->mov(x86::r8, reinterpret_cast<uint64_t>(info.get()));
  auto helper = func_->returnsPrimitiveDouble()
      ? reinterpret_cast<uint64_t>(JITRT_CallStaticallyWithPrimitiveSignatureFP)
      : reinterpret_cast<uint64_t>(JITRT_CallStaticallyWithPrimitiveSignature);
  as_->call(helper);
  as_->leave();
  as_->ret();
}

std::pair<asmjit::BaseNode*, asmjit::BaseNode*>
NativeGenerator::generateBoxedReturnWrapper() {
  asmjit::BaseNode* entry_cursor = as_->cursor();

  if (!func_->returnsPrimitive()) {
    return {entry_cursor, nullptr};
  }

  Label generic_entry = as_->newLabel();
  Label box_done = as_->newLabel();
  Label error = as_->newLabel();
  hir::Type ret_type = func_->return_type;
  uint64_t box_func;

  generateFunctionEntry();
  as_->call(generic_entry);

  // If there was an error, there's nothing to box.
  bool returns_double = func_->returnsPrimitiveDouble();
  if (returns_double) {
    as_->ptest(x86::xmm1, x86::xmm1);
    as_->je(error);
  } else {
    as_->test(x86::edx, x86::edx);
    as_->je(box_done);
  }

  if (ret_type <= TCBool) {
    as_->movzx(x86::edi, x86::al);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxBool);
  } else if (ret_type <= TCInt8) {
    as_->movsx(x86::edi, x86::al);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxI32);
  } else if (ret_type <= TCUInt8) {
    as_->movzx(x86::edi, x86::al);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxU32);
  } else if (ret_type <= TCInt16) {
    as_->movsx(x86::edi, x86::ax);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxI32);
  } else if (ret_type <= TCUInt16) {
    as_->movzx(x86::edi, x86::ax);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxU32);
  } else if (ret_type <= TCInt32) {
    as_->mov(x86::edi, x86::eax);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxI32);
  } else if (ret_type <= TCUInt32) {
    as_->mov(x86::edi, x86::eax);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxU32);
  } else if (ret_type <= TCInt64) {
    as_->mov(x86::rdi, x86::rax);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxI64);
  } else if (ret_type <= TCUInt64) {
    as_->mov(x86::rdi, x86::rax);
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxU64);
  } else if (returns_double) {
    // xmm0 already contains the return value
    box_func = reinterpret_cast<uint64_t>(JITRT_BoxDouble);
  } else {
    JIT_ABORT("Unsupported primitive return type {}", ret_type.toString());
  }

  as_->call(box_func);

  as_->bind(box_done);
  as_->leave();
  as_->ret();

  if (returns_double) {
    as_->bind(error);
    as_->xor_(x86::rax, x86::rax);
    as_->leave();
    as_->ret();
  }

  as_->bind(generic_entry);

  // New generic entry is after the boxed wrapper.
  return {as_->cursor(), entry_cursor};
}

void NativeGenerator::generateArgcountCheckPrologue(Label correct_arg_count) {
  BorrowedRef<PyCodeObject> code = GetFunction()->code;

  Label arg_check = as_->newLabel();
  bool have_varargs = code->co_flags & (CO_VARARGS | CO_VARKEYWORDS);

  // If the code object expects *args or **kwargs we need to dispatch
  // through our helper regardless if they are provided to create the *args
  // tuple and the **kwargs dict and free them on exit.
  //
  // Similarly, if the function expects keyword-only args, we dispatch
  // through the helper to check that they were, in fact, passed via keyword
  // arguments.
  //
  // There's a lot of other things that happen in the helper so there is
  // potentially a lot of room for optimization here.
  bool will_check_argcount = !have_varargs && code->co_kwonlyargcount == 0;
  if (will_check_argcount) {
    as_->test(x86::rcx, x86::rcx);
    as_->je(arg_check);
  }

  // We don't check the length of the kwnames tuple here, normal callers will
  // never pass the empty tuple.  It is possible for odd callers to still pass
  // the empty tuple in which case we'll just go through the slow binding
  // path.
  as_->call(reinterpret_cast<uint64_t>(JITRT_CallWithKeywordArgs));
  as_->leave();
  as_->ret();

  // Check that we have a valid number of args.
  if (will_check_argcount) {
    as_->bind(arg_check);
    asmjit::BaseNode* arg_check_cursor = as_->cursor();
    as_->cmp(x86::edx, GetFunction()->numArgs());

    // We don't have the correct number of arguments. Call a helper to either
    // fix them up with defaults or raise an approprate exception.
    as_->jz(correct_arg_count);
    as_->mov(x86::rcx, GetFunction()->numArgs());
    auto helper = func_->returnsPrimitiveDouble()
        ? reinterpret_cast<uint64_t>(JITRT_CallWithIncorrectArgcountFPReturn)
        : reinterpret_cast<uint64_t>(JITRT_CallWithIncorrectArgcount);
    as_->call(helper);
    as_->leave();
    as_->ret();
    env_.addAnnotation(
        "Check if called with correct argcount", arg_check_cursor);
  }
}

#if PY_VERSION_HEX < 0x030C0000
int NativeGenerator::calcFrameHeaderSize(const hir::Function* func) {
  if (func == nullptr || func->code->co_flags & kCoFlagsAnyGenerator) {
    return 0;
  }
  return sizeof(FrameHeader);
}
#endif

// calcMaxInlineDepth must work with nullptr HIR functions because it's valid
// to call NativeGenerator with only LIR (e.g., from a test). In the case of an
// LIR-only function, there is no HIR inlining.
int NativeGenerator::calcMaxInlineDepth(const hir::Function* func) {
  if (func == nullptr) {
    return 0;
  }
  int result = 0;
  for (const auto& block : func->cfg.blocks) {
    for (const auto& instr : block) {
      if (instr.opcode() != Opcode::kBeginInlinedFunction) {
        continue;
      }
      auto bif = dynamic_cast<const BeginInlinedFunction*>(&instr);
      int depth = bif->inlineDepth();
      result = std::max(depth, result);
    }
  }
  return result;
}

NativeGeneratorFactory::NativeGeneratorFactory()
    : deopt_trampoline_{generateDeoptTrampoline(false)},
      deopt_trampoline_generators_{generateDeoptTrampoline(true)},
      failed_deferred_compile_trampoline_{
          generateFailedDeferredCompileTrampoline()} {}

std::unique_ptr<NativeGenerator> NativeGeneratorFactory::operator()(
    const hir::Function* func) const {
  return std::make_unique<NativeGenerator>(
      func,
      deopt_trampoline_,
      deopt_trampoline_generators_,
      failed_deferred_compile_trampoline_);
}

} // namespace jit::codegen

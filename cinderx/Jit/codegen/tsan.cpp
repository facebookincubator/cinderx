// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/tsan.h"

#if CINDER_JIT_TSAN_ENABLED

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/lir/operand.h"

#include <asmjit/asmjit.h>
#include <sanitizer/tsan_interface_atomic.h>

#include <algorithm>
#include <array>

extern "C" {
void __tsan_read1(void* addr);
void __tsan_read2(void* addr);
void __tsan_read4(void* addr);
void __tsan_read8(void* addr);
void __tsan_read16(void* addr);
void __tsan_write1(void* addr);
void __tsan_write2(void* addr);
void __tsan_write4(void* addr);
void __tsan_write8(void* addr);
void __tsan_write16(void* addr);
}

namespace jit::codegen {
namespace {

constexpr int kTsanXmmRegCount = 16;
constexpr int kXmmRegSize = 16;
constexpr int kTsanXmmSaveSize = kTsanXmmRegCount * kXmmRegSize;
constexpr int kStackAlign = 16;
constexpr int kWordSize = 8;
constexpr int kNoExcludedGp = PhyLocation::REG_INVALID;

struct SavedCallerState {
  int excluded_loc{kNoExcludedGp};
  int align_padding{0};
};

bool isCallerSavedGp(int loc) {
  return std::find(
             CALLER_SAVE_GP_REGS.begin(), CALLER_SAVE_GP_REGS.end(), loc) !=
      CALLER_SAVE_GP_REGS.end();
}

constexpr int savedGpCount(int excluded_loc) {
  return static_cast<int>(CALLER_SAVE_GP_REGS.size()) -
      (excluded_loc == kNoExcludedGp ? 0 : 1);
}

// TSAN instrumentation is emitted as a runtime call in generated code. The
// selected helper depends on the access kind and width, but every path ends by
// loading that helper's address into RAX and calling it with the x86-64 ABI
// arguments already prepared.
template <typename Func>
const void* tsanFunc(Func func) {
  return reinterpret_cast<const void*>(func);
}

const void* getTsanReadFunc(size_t access_size_in_bytes) {
  switch (access_size_in_bytes) {
    case 1:
      return tsanFunc(__tsan_read1);
    case 2:
      return tsanFunc(__tsan_read2);
    case 4:
      return tsanFunc(__tsan_read4);
    case 8:
      return tsanFunc(__tsan_read8);
    case 16:
      return tsanFunc(__tsan_read16);
    default:
      JIT_ABORT("Unexpected TSAN read size {}", access_size_in_bytes);
  }
}

const void* getTsanWriteFunc(size_t access_size_in_bytes) {
  switch (access_size_in_bytes) {
    case 1:
      return tsanFunc(__tsan_write1);
    case 2:
      return tsanFunc(__tsan_write2);
    case 4:
      return tsanFunc(__tsan_write4);
    case 8:
      return tsanFunc(__tsan_write8);
    case 16:
      return tsanFunc(__tsan_write16);
    default:
      JIT_ABORT("Unexpected TSAN write size {}", access_size_in_bytes);
  }
}

const void* getTsanAtomicLoadFunc(size_t access_size_in_bytes) {
  switch (access_size_in_bytes) {
    case 1:
      return tsanFunc(__tsan_atomic8_load);
    case 2:
      return tsanFunc(__tsan_atomic16_load);
    case 4:
      return tsanFunc(__tsan_atomic32_load);
    case 8:
      return tsanFunc(__tsan_atomic64_load);
    default:
      JIT_ABORT("Unexpected TSAN atomic load size {}", access_size_in_bytes);
  }
}

const void* getTsanAtomicStoreFunc(size_t access_size_in_bytes) {
  switch (access_size_in_bytes) {
    case 1:
      return tsanFunc(__tsan_atomic8_store);
    case 2:
      return tsanFunc(__tsan_atomic16_store);
    case 4:
      return tsanFunc(__tsan_atomic32_store);
    case 8:
      return tsanFunc(__tsan_atomic64_store);
    default:
      JIT_ABORT("Unexpected TSAN atomic store size {}", access_size_in_bytes);
  }
}

bool isSupportedTsanAtomicSize(size_t access_size_in_bytes) {
  return access_size_in_bytes == 1 || access_size_in_bytes == 2 ||
      access_size_in_bytes == 4 || access_size_in_bytes == 8;
}

int alignPadding(int gp_reg_count) {
  int save_size = kWordSize + gp_reg_count * kWordSize + kTsanXmmSaveSize;
  return roundUp(save_size, kStackAlign) - save_size;
}

// Compute the memory operand's effective address into RDI, the first TSAN
// runtime argument register.
void emitTsanAddress(Environ& env, const jit::lir::OperandBase* mem_operand) {
  if (mem_operand->isMem()) {
    auto addr = reinterpret_cast<uint64_t>(mem_operand->getMemoryAddress());
    env.as->mov(asmjit::x86::rdi, addr);
    return;
  }

  if (mem_operand->isInd()) {
    auto indirect = mem_operand->getMemoryIndirect();
    auto* base = indirect->getBaseRegOperand();
    auto* index = indirect->getIndexRegOperand();

    JIT_CHECK(
        base != nullptr,
        "Unexpected indirect operand without base register in TSAN emitter, "
        "operand type {}",
        mem_operand->type());

    if (index == nullptr) {
      env.as->lea(
          asmjit::x86::rdi,
          asmjit::x86::ptr(
              asmjit::x86::gpq(base->getPhyRegister().loc),
              indirect->getOffset()));
    } else {
      env.as->lea(
          asmjit::x86::rdi,
          asmjit::x86::ptr(
              asmjit::x86::gpq(base->getPhyRegister().loc),
              asmjit::x86::gpq(index->getPhyRegister().loc),
              indirect->getMultiplier(),
              indirect->getOffset()));
    }
    return;
  }

  JIT_ABORT(
      "Unsupported operand type for TSAN emitter: {}", mem_operand->type());
}

// Emit an ABI-aligned indirect call to a TSAN runtime function. Callers set up
// the TSAN arguments before reaching this helper.
void emitTsanRuntimeCall(Environ& env, const void* tsan_func) {
  // TSAN runtime code expects the ABI stack alignment. Align dynamically rather
  // than depending on every current and future LIR lowering to enter with
  // 16-byte-aligned RSP.
  env.as->mov(asmjit::x86::rax, asmjit::x86::rsp);
  env.as->and_(asmjit::x86::rsp, -kStackAlign);
  env.as->sub(asmjit::x86::rsp, kStackAlign);
  env.as->mov(asmjit::x86::ptr(asmjit::x86::rsp), asmjit::x86::rax);

  // TSAN runtime symbols may live outside the rel32 reach of JIT memory.
  env.as->mov(asmjit::x86::rax, reinterpret_cast<uint64_t>(tsan_func));
  env.as->call(asmjit::x86::rax);
  env.as->mov(asmjit::x86::rsp, asmjit::x86::ptr(asmjit::x86::rsp));
}

void emitTsanRelaxedAtomicLoadCall(Environ& env, const void* tsan_func) {
  // TSAN declares memory order as int, so clearing the 32-bit arg register is
  // intentional.
  env.as->xor_(asmjit::x86::esi, asmjit::x86::esi);
  emitTsanRuntimeCall(env, tsan_func);
}

void emitTsanRelaxedAtomicStoreCall(Environ& env, const void* tsan_func) {
  // TSAN declares memory order as int, so clearing the 32-bit arg register is
  // intentional.
  env.as->xor_(asmjit::x86::edx, asmjit::x86::edx);
  emitTsanRuntimeCall(env, tsan_func);
}

SavedCallerState saveCallerSavedState(
    Environ& env,
    int excluded_caller_saved_gp = kNoExcludedGp) {
  JIT_DCHECK(
      excluded_caller_saved_gp == kNoExcludedGp ||
          isCallerSavedGp(excluded_caller_saved_gp),
      "TSAN excluded GP register must be caller-saved");

  SavedCallerState state;

  // Preserve flags so Move/MoveRelaxed keep their FlagEffects::kNone contract.
  env.as->pushfq();

  for (PhyLocation reg : CALLER_SAVE_GP_REGS) {
    if (reg.loc == excluded_caller_saved_gp) {
      continue;
    }
    env.as->push(asmjit::x86::gpq(reg.loc));
  }

  state.excluded_loc = excluded_caller_saved_gp;
  state.align_padding = alignPadding(savedGpCount(state.excluded_loc));
  env.as->sub(asmjit::x86::rsp, kTsanXmmSaveSize + state.align_padding);

  // Save caller-saved XMM state. If JIT code starts using YMM/AVX values, this
  // must preserve the upper lanes too.
  for (int i = 0; i < kTsanXmmRegCount; i++) {
    env.as->movups(
        asmjit::x86::ptr(asmjit::x86::rsp, i * kXmmRegSize),
        asmjit::x86::xmm(i));
  }
  return state;
}

void restoreCallerSavedState(Environ& env, const SavedCallerState& state) {
  // Restore all caller-saved XMM registers.
  for (int i = 0; i < kTsanXmmRegCount; i++) {
    env.as->movups(
        asmjit::x86::xmm(i),
        asmjit::x86::ptr(asmjit::x86::rsp, i * kXmmRegSize));
  }

  env.as->add(asmjit::x86::rsp, kTsanXmmSaveSize + state.align_padding);

  for (auto it = CALLER_SAVE_GP_REGS.rbegin(); it != CALLER_SAVE_GP_REGS.rend();
       ++it) {
    if (it->loc == state.excluded_loc) {
      continue;
    }
    env.as->pop(asmjit::x86::gpq(it->loc));
  }
  env.as->popfq();
}

int savedGpOffset(const SavedCallerState& state, int loc) {
  int saved_index = 0;
  for (PhyLocation saved_loc : CALLER_SAVE_GP_REGS) {
    if (saved_loc.loc == state.excluded_loc) {
      continue;
    }
    if (saved_loc.loc == loc) {
      return kTsanXmmSaveSize + state.align_padding +
          (savedGpCount(state.excluded_loc) - 1 - saved_index) * kWordSize;
    }
    saved_index++;
  }
  // The requested register must have been pushed by saveCallerSavedState().
  JIT_ABORT("Register {} was not saved for TSAN call", loc);
}

asmjit::x86::Gp sizedGp(int loc, size_t access_size_in_bytes) {
  switch (access_size_in_bytes) {
    case 1:
      return asmjit::x86::gpb(loc);
    case 2:
      return asmjit::x86::gpw(loc);
    case 4:
      return asmjit::x86::gpd(loc);
    case 8:
      return asmjit::x86::gpq(loc);
    default:
      JIT_ABORT("Unexpected TSAN access size {}", access_size_in_bytes);
  }
}

asmjit::x86::Gp gpForArgSize(int loc, size_t access_size_in_bytes) {
  if (access_size_in_bytes <= 4) {
    return asmjit::x86::gpd(loc);
  }
  if (access_size_in_bytes == 8) {
    return asmjit::x86::gpq(loc);
  }
  JIT_ABORT("Unexpected TSAN argument size {}", access_size_in_bytes);
}

void emitMovArgImm(Environ& env, asmjit::x86::Gp dst, uint64_t imm) {
  if (dst.size() <= 4) {
    env.as->mov(dst, static_cast<uint32_t>(imm));
  } else {
    env.as->mov(dst, imm);
  }
}

void emitTsanLoadResult(
    Environ& env,
    const jit::lir::OperandBase* output_operand,
    size_t access_size_in_bytes) {
  JIT_CHECK(
      output_operand->isReg(),
      "Expected register output for TSAN atomic load, got {}",
      output_operand->type());

  int output_loc = output_operand->getPhyRegister().loc;
  env.as->mov(
      sizedGp(output_loc, access_size_in_bytes),
      sizedGp(RAX.loc, access_size_in_bytes));
}

void emitTsanStoreValue(
    Environ& env,
    const jit::lir::OperandBase* value_operand,
    size_t access_size_in_bytes,
    const SavedCallerState& state) {
  auto dst = gpForArgSize(RSI.loc, access_size_in_bytes);
  if (value_operand->isReg()) {
    int value_loc = value_operand->getPhyRegister().loc;
    if (value_loc == RDI.loc) {
      env.as->mov(
          dst,
          asmjit::x86::ptr(asmjit::x86::rsp, savedGpOffset(state, value_loc)));
    } else {
      env.as->mov(dst, gpForArgSize(value_loc, access_size_in_bytes));
    }
    return;
  }

  JIT_DCHECK(
      value_operand->isImm(),
      "Expected immediate value operand for TSAN atomic store, got {}",
      value_operand->type());
  emitMovArgImm(env, dst, value_operand->getConstant());
}

void emitTsanCall(
    Environ& env,
    const jit::lir::OperandBase* mem_operand,
    const void* tsan_func) {
  auto state = saveCallerSavedState(env);
  emitTsanAddress(env, mem_operand);
  emitTsanRuntimeCall(env, tsan_func);
  restoreCallerSavedState(env, state);
}

} // namespace

void emitTsanRead(
    Environ& env,
    const jit::lir::OperandBase* mem_operand,
    size_t access_size_in_bytes) {
  if (mem_operand->isStack()) {
    return;
  }
  emitTsanCall(env, mem_operand, getTsanReadFunc(access_size_in_bytes));
}

void emitTsanWrite(
    Environ& env,
    const jit::lir::OperandBase* mem_operand,
    size_t access_size_in_bytes) {
  if (mem_operand->isStack()) {
    return;
  }
  emitTsanCall(env, mem_operand, getTsanWriteFunc(access_size_in_bytes));
}

bool tryEmitTsanRelaxedAtomicRead(
    Environ& env,
    const jit::lir::OperandBase* output_operand,
    const jit::lir::OperandBase* mem_operand,
    size_t access_size_in_bytes) {
  JIT_CHECK(
      output_operand->isReg(), "Expected register output for TSAN atomic load");

  if (mem_operand->isStack() ||
      !isSupportedTsanAtomicSize(access_size_in_bytes)) {
    return false;
  }

  const void* tsan_func = getTsanAtomicLoadFunc(access_size_in_bytes);
  int output_loc = output_operand->getPhyRegister().loc;
  auto state = saveCallerSavedState(
      env, isCallerSavedGp(output_loc) ? output_loc : kNoExcludedGp);
  emitTsanAddress(env, mem_operand);
  emitTsanRelaxedAtomicLoadCall(env, tsan_func);
  emitTsanLoadResult(env, output_operand, access_size_in_bytes);
  restoreCallerSavedState(env, state);
  return true;
}

bool tryEmitTsanRelaxedAtomicWrite(
    Environ& env,
    const jit::lir::OperandBase* mem_operand,
    const jit::lir::OperandBase* value_operand,
    size_t access_size_in_bytes) {
  if (mem_operand->isStack() ||
      !isSupportedTsanAtomicSize(access_size_in_bytes)) {
    return false;
  }

  const void* tsan_func = getTsanAtomicStoreFunc(access_size_in_bytes);
  auto state = saveCallerSavedState(env);
  emitTsanAddress(env, mem_operand);
  emitTsanStoreValue(env, value_operand, access_size_in_bytes, state);
  emitTsanRelaxedAtomicStoreCall(env, tsan_func);
  restoreCallerSavedState(env, state);
  return true;
}

} // namespace jit::codegen

#endif // CINDER_JIT_TSAN_ENABLED

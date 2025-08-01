// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/register_preserver.h"

#include "cinderx/Common/util.h"

namespace jit::codegen {

// Stack alignment requirement for x86-64
constexpr size_t kConstStackAlignmentRequirement = 16;

RegisterPreserver::RegisterPreserver(
    asmjit::x86::Builder* as,
    const std::vector<
        std::pair<const asmjit::x86::Reg&, const asmjit::x86::Reg&>>& save_regs)
    : as_(as), save_regs_(save_regs), align_stack_(false) {}

void RegisterPreserver::preserve() {
  size_t rsp_offset = 0;
  for (const auto& pair : save_regs_) {
    if (pair.first.isGpq()) {
      as_->push((asmjit::x86::Gpq&)pair.first);
    } else if (pair.first.isXmm()) {
      as_->sub(asmjit::x86::rsp, pair.first.size());
      as_->movdqu(
          asmjit::x86::dqword_ptr(asmjit::x86::rsp),
          (asmjit::x86::Xmm&)pair.first);
    } else {
      JIT_ABORT("unsupported saved register type");
    }
    rsp_offset += pair.first.size();
  }
  align_stack_ = rsp_offset % kConstStackAlignmentRequirement;
  if (align_stack_) {
    as_->push(asmjit::x86::rax);
  }
}

void RegisterPreserver::remap() {
  for (const auto& pair : save_regs_) {
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
}

void RegisterPreserver::restore() {
  if (align_stack_) {
    as_->add(asmjit::x86::rsp, 8);
  }
  for (auto iter = save_regs_.rbegin(); iter != save_regs_.rend(); ++iter) {
    if (iter->second.isGpq()) {
      as_->pop((asmjit::x86::Gpq&)iter->second);
    } else if (iter->second.isXmm()) {
      as_->movdqu(
          (asmjit::x86::Xmm&)iter->second,
          asmjit::x86::dqword_ptr(asmjit::x86::rsp));
      as_->add(asmjit::x86::rsp, 16);
    } else {
      JIT_ABORT("unsupported saved register type");
    }
  }
}

} // namespace jit::codegen

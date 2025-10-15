// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/code_runtime.h"

#include "cinderx/Common/util.h"

namespace jit {

GenYieldPoint::GenYieldPoint(std::size_t deopt_idx, ptrdiff_t yield_from_offset)
    : deopt_idx_{deopt_idx}, yield_from_offset_{yield_from_offset} {}

void GenYieldPoint::setResumeTarget(uintptr_t resume_target) {
  resume_target_ = resume_target;
}

uintptr_t GenYieldPoint::resumeTarget() const {
  return resume_target_;
}

std::size_t GenYieldPoint::deoptIdx() const {
  return deopt_idx_;
}

bool GenYieldPoint::isYieldFrom() const {
  return yield_from_offset_ != kInvalidYieldFromOffset;
}

ptrdiff_t GenYieldPoint::yieldFromOffset() const {
  return yield_from_offset_;
}

bool RuntimeFrameState::isGen() const {
  return code()->co_flags & kCoFlagsAnyGenerator;
}

BorrowedRef<PyCodeObject> RuntimeFrameState::code() const {
  return code_;
}

BorrowedRef<PyDictObject> RuntimeFrameState::builtins() const {
  return builtins_;
}

BorrowedRef<PyDictObject> RuntimeFrameState::globals() const {
  return globals_;
}

BorrowedRef<PyFunctionObject> RuntimeFrameState::func() const {
  return func_;
}

CodeRuntime::CodeRuntime(BorrowedRef<PyFunctionObject> func)
    : CodeRuntime{
          BorrowedRef<PyCodeObject>{func->func_code},
          func->func_builtins,
          func->func_globals} {}

CodeRuntime::CodeRuntime(
    BorrowedRef<PyCodeObject> code,
    BorrowedRef<PyDictObject> builtins,
    BorrowedRef<PyDictObject> globals)
    : frame_state_{code, builtins, globals},
      deopt_stats_{std::make_unique<DeoptStatMap>()} {
  // Ensure code, globals, and builtins objects live as long as their compiled
  // functions.
  addReference(code);
  addReference(builtins);
  addReference(globals);
}

void CodeRuntime::addReference(BorrowedRef<> obj) {
  // Serialize as we modify the ref-count to obj which may be widely accessible.
  ThreadedCompileSerialize guard;
  references_.emplace(ThreadedRef<>::create(obj));
}

void CodeRuntime::releaseReferences() {
  // Serialize as we modify ref-counts which may be widely accessible.
  ThreadedCompileSerialize guard;
  references_.clear();
}

GenYieldPoint* CodeRuntime::addGenYieldPoint(GenYieldPoint&& gen_yield_point) {
  gen_yield_points_.emplace_back(std::move(gen_yield_point));
  return &gen_yield_points_.back();
}

std::size_t CodeRuntime::addDeoptMetadata(DeoptMetadata&& deopt_meta) {
  deopt_metadatas_.emplace_back(std::move(deopt_meta));
  return deopt_metadatas_.size() - 1;
}

DeoptMetadata& CodeRuntime::getDeoptMetadata(std::size_t id) {
  return deopt_metadatas_[id];
}

const DeoptMetadata& CodeRuntime::getDeoptMetadata(std::size_t id) const {
  return deopt_metadatas_[id];
}

const std::vector<DeoptMetadata>& CodeRuntime::deoptMetadatas() const {
  return deopt_metadatas_;
}

void CodeRuntime::recordDeopt(std::size_t idx, BorrowedRef<> guilty_value) {
  DeoptStat& stat = (*deopt_stats_)[idx];
  stat.recordDeopt(guilty_value);
}

const DeoptStat* CodeRuntime::deoptStat(std::size_t idx) const {
  auto iter = deopt_stats_->find(idx);
  return iter != deopt_stats_->end() ? &iter->second : nullptr;
}

void CodeRuntime::clearDeoptStats() {
  deopt_stats_->clear();
}

const RuntimeFrameState* CodeRuntime::frameState() const {
  return &frame_state_;
}

int CodeRuntime::frameSize() const {
  return frame_size_;
}

void CodeRuntime::setFrameSize(int size) {
  frame_size_ = size;
}

DebugInfo* CodeRuntime::debugInfo() {
  return &debug_info_;
}

} // namespace jit

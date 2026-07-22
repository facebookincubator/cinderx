// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/code_runtime.h"

#include "cinderx/Common/util.h"

namespace cinderx::jit {

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

bool CodeRuntime::isGen() const {
  return code()->co_flags & kCoFlagsAnyGenerator;
}

BorrowedRef<PyCodeObject> CodeRuntime::code() const {
  return code_;
}

BorrowedRef<PyDictObject> CodeRuntime::builtins() const {
  return builtins_;
}

BorrowedRef<PyDictObject> CodeRuntime::globals() const {
  return globals_;
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
    : code_{code}, builtins_{builtins}, globals_{globals} {
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
  // We want to be careful here with the freeing of these references. Freeing
  // the objects could cause our CompiledFunction to be freed as well so first
  // we grab the references and then clear them.
  ThreadedCompileSerialize guard;
  std::unordered_set<ThreadedRef<>> refs;
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  ThreadedRef<> tmp;
#endif
  {
    refs = std::move(references_);
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
    reifier_.reset(nullptr);
#endif
  }
  // and then we let the dtors clean everything up
}

GenYieldPoint* CodeRuntime::addGenYieldPoint(GenYieldPoint&& gen_yield_point) {
  gen_yield_points_.emplace_back(std::move(gen_yield_point));
  return &gen_yield_points_.back();
}

std::size_t CodeRuntime::addRawDeoptMetadata(DeoptMetadata&& deopt_meta) {
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

int CodeRuntime::frameSize() const {
  return frame_size_;
}

void CodeRuntime::setFrameSize(int size) {
  frame_size_ = size;
}

uint32_t CodeRuntime::spillWords() const {
  return spill_words_;
}

void CodeRuntime::setSpillWords(uint32_t words) {
  spill_words_ = words;
}

DebugInfo* CodeRuntime::debugInfo() {
  return &debug_info_;
}

void** CodeRuntime::allocateTypeCheckJumpTable(size_t num_entries) {
  type_check_jump_table_ = std::make_unique<void*[]>(num_entries);
  return type_check_jump_table_.get();
}

bool CodeRuntime::isCleared() const {
  // We always add some references when we first create the CodeRuntime, so we
  // know if no references are left we've been cleared.
  return references_.empty();
}

int CodeRuntime::traverse(visitproc visit, void* arg) {
  // Only traverse objects that this CodeRuntime owns strong references to.
  // The references_ set contains ThreadedRef which hold strong references.
  // code_, builtins_, globals_ are BorrowedRef pointing to the same objects
  // already in references_ - don't double-count.
  for (const auto& ref : references_) {
    Py_VISIT(ref.get());
  }
  if (auto ref = reifier()) {
    Py_VISIT(ref.get());
  }

  return 0;
}

std::optional<UnitCallStack> CodeRuntime::getUnitCallStackFromDeoptIdx(
    std::size_t deopt_idx) const {
  if (deopt_idx >= deopt_metadatas_.size()) {
    return std::nullopt;
  }
  const DeoptMetadata& meta = deopt_metadatas_[deopt_idx];
  UnitCallStack stack;
  stack.reserve(meta.frame_meta.size());
  for (const auto& frame : meta.frame_meta) {
    stack.emplace_back(frame.code, frame.cause_instr_idx);
  }
  return stack;
}

std::optional<uintptr_t> CodeRuntime::getCallsiteDeoptExit(
    uintptr_t return_addr) const {
  auto it = callsite_deopt_exits_.find(return_addr);
  if (it != callsite_deopt_exits_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void CodeRuntime::addCallsiteDeoptExit(
    uintptr_t return_addr,
    uintptr_t deopt_exit_addr) {
  callsite_deopt_exits_[return_addr] = deopt_exit_addr;
}

void CodeRuntime::setReifier([[maybe_unused]] ThreadedRef<>&& reifier) {
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  reifier_ = std::move(reifier);
#endif
}

BorrowedRef<> CodeRuntime::reifier() {
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  return reifier_;
#else
  return nullptr;
#endif
}

void CodeRuntime::setCompiledFunction(
    BorrowedRef<CompiledFunction> compiled_func) {
  compiled_function_ = compiled_func;
}

BorrowedRef<CompiledFunction> CodeRuntime::compiledFunction() const {
  return compiled_function_;
}

} // namespace cinderx::jit

// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/debug_info.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/threaded_compile.h"

#include <deque>
#include <limits>
#include <unordered_set>

namespace jit {

constexpr ptrdiff_t kInvalidYieldFromOffset =
    std::numeric_limits<ptrdiff_t>::max();

// Information about how a specific yield instruction should resume.
class GenYieldPoint {
 public:
  static constexpr int resumeTargetOffset() {
    return offsetof(GenYieldPoint, resume_target_);
  }

  GenYieldPoint(std::size_t deopt_idx, ptrdiff_t yield_from_offset);

  // Get and set what address the yield should resume from.
  uintptr_t resumeTarget() const;
  void setResumeTarget(uintptr_t resume_target);

  std::size_t deoptIdx() const;
  bool isYieldFrom() const;
  ptrdiff_t yieldFromOffset() const;

 private:
  uintptr_t resume_target_{0};
  const std::size_t deopt_idx_;
  const ptrdiff_t yield_from_offset_;
};

class alignas(16) RuntimeFrameState {
 public:
  static constexpr int64_t codeOffset() {
    return offsetof(RuntimeFrameState, code_);
  }

  RuntimeFrameState(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      BorrowedRef<PyFunctionObject> func = nullptr)
      : code_{code}, builtins_{builtins}, globals_{globals}, func_{func} {}

  // Check if this is a generator frame.
  bool isGen() const;

  BorrowedRef<PyCodeObject> code() const;
  BorrowedRef<PyDictObject> builtins() const;
  BorrowedRef<PyDictObject> globals() const;
  BorrowedRef<PyFunctionObject> func() const;

 private:
  // All fields are owned by the CodeRuntime that owns this RuntimeFrameState.

  BorrowedRef<PyCodeObject> code_;
  BorrowedRef<PyDictObject> builtins_;
  BorrowedRef<PyDictObject> globals_;
  // The function is only set for inlined frames.
  BorrowedRef<PyFunctionObject> func_;
};

// Runtime data for a PyCodeObject object, containing caches and any other data
// associated with a JIT-compiled function.
class alignas(16) CodeRuntime {
 public:
  static constexpr int64_t frameStateOffset() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    return offsetof(CodeRuntime, frame_state_);
#pragma GCC diagnostic pop
  }

  static constexpr int64_t codeOffset() {
    return CodeRuntime::frameStateOffset() + RuntimeFrameState::codeOffset();
  }

  explicit CodeRuntime(BorrowedRef<PyFunctionObject> func);
  CodeRuntime(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals);

  template <typename... Args>
  RuntimeFrameState* allocateRuntimeFrameState(Args&&... args) {
    return inlined_frame_states_
        .emplace_back(
            std::make_unique<RuntimeFrameState>(std::forward<Args>(args)...))
        .get();
  }

  // Ensure that this CodeRuntime owns a reference to the given borrowed
  // object, keeping it alive for use by the compiled code. Make CodeRuntime a
  // new owner of the object.
  void addReference(BorrowedRef<> obj);

  // Release any references this CodeRuntime holds to Python objects.
  void releaseReferences();

  // Store meta-data about generator yield point.
  GenYieldPoint* addGenYieldPoint(GenYieldPoint&& gen_yield_point);

  // Add metadata used during a deopt.  Return an ID that can be used to fetch
  // the metadata from generated code.
  std::size_t addDeoptMetadata(DeoptMetadata&& deopt_meta);

  // Get a reference to the DeoptMetadata with the given ID.
  DeoptMetadata& getDeoptMetadata(std::size_t id);
  const DeoptMetadata& getDeoptMetadata(std::size_t id) const;

  // Get all deopt metadatas for the given CodeRuntime.
  const std::vector<DeoptMetadata>& deoptMetadatas() const;

  // Get the top-level runtime frame state for this CodeRuntime's PyCodeObject.
  const RuntimeFrameState* frameState() const;

  // Get and set the total size of a stack frame for this compiled code object.
  int frameSize() const;
  void setFrameSize(int size);

  DebugInfo* debugInfo();

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  void setReifier(BorrowedRef<> reifier) {
    ThreadedCompileSerialize guard;
    reifier_ = ThreadedRef<>::create(reifier);
  }
  BorrowedRef<> reifier() {
    return reifier_;
  }
#else
  BorrowedRef<> reifier() {
    return nullptr;
  }
#endif
 private:
  RuntimeFrameState frame_state_;
  std::vector<std::unique_ptr<RuntimeFrameState>> inlined_frame_states_;

  // References owned by this CodeRuntime.
  std::unordered_set<ThreadedRef<PyObject>> references_;

  // Metadata about yield points. Deque so we can have raw pointers to content.
  std::deque<GenYieldPoint> gen_yield_points_;

  // Metadata about deopt points.  Safe to use a vector as these are always
  // accessed by index.
  std::vector<DeoptMetadata> deopt_metadatas_;

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  ThreadedRef<> reifier_;
#endif

  int frame_size_{-1};
  DebugInfo debug_info_;
};

} // namespace jit

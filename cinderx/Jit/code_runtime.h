// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/genobject_jit.h"
#endif

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/debug_info.h"
#include "cinderx/Jit/threaded_compile.h"

#include <deque>
#include <unordered_map>

namespace jit {

class GenYieldPoint {
 public:
  explicit GenYieldPoint(
      std::size_t deopt_idx,
      bool is_yield_from,
      ptrdiff_t yield_from_offs)
      : deopt_idx_(deopt_idx),
        isYieldFrom_(is_yield_from),
        yieldFromOffs_(yield_from_offs) {}

  void setResumeTarget(uint64_t resume_target) {
    resume_target_ = resume_target;
  }

  uint64_t resumeTarget() const {
    return resume_target_;
  }

  std::size_t deoptIdx() const {
    return deopt_idx_;
  }

  bool isYieldFrom() const {
    return isYieldFrom_;
  }

  ptrdiff_t yieldFromOffset() const {
    return yieldFromOffs_;
  }

  static constexpr int resumeTargetOffset() {
    return offsetof(GenYieldPoint, resume_target_);
  }

 private:
  uint64_t resume_target_{0};
  const std::size_t deopt_idx_;
  const bool isYieldFrom_;
  const ptrdiff_t yieldFromOffs_;
};

class alignas(16) RuntimeFrameState {
 public:
  RuntimeFrameState(
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<> builtins,
      BorrowedRef<> globals,
      BorrowedRef<PyFunctionObject> func = nullptr)
      : code_(code), builtins_(builtins), globals_(globals), func_(func) {}

  bool isGen() const {
    return code()->co_flags & kCoFlagsAnyGenerator;
  }

  BorrowedRef<PyCodeObject> code() const {
    return code_;
  }

  BorrowedRef<> builtins() const {
    return builtins_;
  }

  BorrowedRef<> globals() const {
    return globals_;
  }

  BorrowedRef<PyFunctionObject> func() const {
    return func_;
  }

  static constexpr int64_t codeOffset() {
    return offsetof(RuntimeFrameState, code_);
  }

 private:
  // These are owned by the CodeRuntime that owns this RuntimeFrameState.
  BorrowedRef<PyCodeObject> code_;
  BorrowedRef<> builtins_;
  BorrowedRef<> globals_;
  BorrowedRef<PyFunctionObject> func_;
};

// Runtime data for a PyCodeObject object, containing caches and any other data
// associated with a JIT-compiled function.
class alignas(16) CodeRuntime {
 public:
  CodeRuntime(
      PyCodeObject* code,
      PyObject* builtins,
      PyObject* globals,
      PyFunctionObject* func = nullptr)
      : frame_state_(code, builtins, globals, func) {
    // TASK(T88040922): Until we work out something smarter, force code,
    // globals, and builtins objects for compiled functions to live as long as
    // the JIT is initialized.
    addReference(BorrowedRef(code));
    addReference(builtins);
    addReference(globals);
    if (func != nullptr) {
      addReference(&func->ob_base);
    }
  }

  explicit CodeRuntime(PyFunctionObject* func)
      : CodeRuntime(
            reinterpret_cast<PyCodeObject*>(func->func_code),
            func->func_builtins,
            func->func_globals) {}

  template <typename... Args>
  RuntimeFrameState* allocateRuntimeFrameState(Args&&... args) {
    // Serialize as we modify the globally shared runtimes data.
    ThreadedCompileSerialize guard;
    inlined_frame_states_.emplace_back(
        std::make_unique<RuntimeFrameState>(std::forward<Args>(args)...));
    return inlined_frame_states_.back().get();
  }

  const RuntimeFrameState* frameState() const {
    return &frame_state_;
  }

  // Release any references this CodeRuntime holds to Python objects.
  void releaseReferences();

  // Ensure that this CodeRuntime owns a reference to the given owned object,
  // keeping it alive for use by the compiled code. Transfer ownership of the
  // object to the CodeRuntime.
  void addReference(Ref<>&& obj);

  // Ensure that this CodeRuntime owns a reference to the given borrowed
  // object, keeping it alive for use by the compiled code. Make CodeRuntime a
  // new owner of the object.
  void addReference(BorrowedRef<> obj);

  // Store meta-data about generator yield point.
  GenYieldPoint* addGenYieldPoint(GenYieldPoint&& gen_yield_point) {
    gen_yield_points_.emplace_back(std::move(gen_yield_point));
    return &gen_yield_points_.back();
  }

  void set_frame_size(int size) {
    frame_size_ = size;
  }
  int frame_size() const {
    return frame_size_;
  }

  DebugInfo* debug_info() {
    return &debug_info_;
  }

  static constexpr int64_t frameStateOffset() {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
    return offsetof(CodeRuntime, frame_state_);
#pragma GCC diagnostic pop
  }

  static const int64_t kPyCodeOffset;

 private:
  RuntimeFrameState frame_state_;
  std::vector<std::unique_ptr<RuntimeFrameState>> inlined_frame_states_;

  std::unordered_set<ThreadedRef<PyObject>> references_;

  // Metadata about yield points. Deque so we can have raw pointers to content.
  std::deque<GenYieldPoint> gen_yield_points_;

  int frame_size_{-1};

  DebugInfo debug_info_;
};

} // namespace jit

// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/code_patcher.h"

namespace cinderx::jit {

// Patch a DeoptPatchpoint when the watched function's __code__ is swapped out
// from under compiled code that inlined it.
//
// The watched function outlives this object: the inlined caller holds a
// strong reference to the callee for as long as the compiled code (and hence
// this patcher) is alive.
class FuncCodeDeoptPatcher : public JumpPatcher {
 public:
  FuncCodeDeoptPatcher(
      BorrowedRef<PyFunctionObject> func,
      BorrowedRef<PyCodeObject> expected_code);

  // Re-check, without patching, that the function still has the expected
  // code object. Used to validate watches installed during background
  // compilation.
  bool assumptionsStillValid() const;

  // Access the function being watched.
  BorrowedRef<PyFunctionObject> func() const;

 private:
  // The function being watched. Kept alive by the compiled caller (see the
  // class comment).
  BorrowedRef<PyFunctionObject> func_;

  // The code object the compiled code inlined.
  BorrowedRef<PyCodeObject> expected_code_;
};

} // namespace cinderx::jit

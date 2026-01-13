// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/genobject_jit.h"
#endif

#include "cinderx/Common/util.h"
#include "cinderx/Jit/code_runtime.h"
#include "cinderx/Jit/frame_header.h"

namespace jit {

// In a regular JIT function spill-data is stored at negative offsets from the
// frame pointer and the frame pointer points into the system stack. In JIT
// generators spilled data is still stored backwards from the frame pointer, but
// the frame pointer points to a heap allocated block and this persists when the
// generator is suspended.
//
// While the content of spill data is arbitrary depending on the function, we
// also have a few items of data about the current generator we want to access
// quickly. We can do this via positive offsets from the frame pointer into the
// GenDataFooter struct defined below.
//
// Together the spill data and GenDataFooter make up the complete JIT-specific
// data needed for a generator. PyGenObject::gi_jit_data points above the _top_
// of the spill data (i.e. at the start of the footer). This allows us to
// easily set the frame pointer to the pointer value on generator resume.
//
// The base address of the complete heap allocated suspend data is:
//   PyGenObject::gi_jit_data - GenDataFooter::spillWords
//
// TASK(T209500214): In 3.12 we should roll this data directly into memory
// allocated for a generator rather than having it in a separate heap object.
struct GenDataFooter {
  // Tools which examine/walk the stack expect the following two values to be
  // ahead of the frame pointer.
  uint64_t linkAddress{};
  uint64_t returnAddress{};

  // The frame pointer that was swapped out to point to this spill-data.
  uint64_t originalFramePointer{};

  // Static data specific to the current yield point. Only non-null when we are
  // suspended.
  GenYieldPoint* yieldPoint{};

#if PY_VERSION_HEX < 0x030C0000
  // Current overall state of the JIT.
  // In 3.12+ we use the new PyGenObject::gi_frame_state field instead.
  CiJITGenState state{};
#endif

  // Allocated space before this struct in 64-bit words.
  size_t spillWords{};

  // Entry-point to resume a JIT generator.
  GenResumeFunc resumeEntry{};

  // Associated generator object
  PyGenObject* gen{};

  // JIT metadata for associated code object
  CodeRuntime* code_rt{nullptr};

#if defined(ENABLE_LIGHTWEIGHT_FRAMES)
  // Frame header used for tracking the current frame.
  FrameHeader frame_header;
#endif
};

#if PY_VERSION_HEX >= 0x030C0000
GenDataFooter** jitGenDataFooterPtr(PyGenObject* gen, PyCodeObject* gen_code);

GenDataFooter** jitGenDataFooterPtr(PyGenObject* gen);

GenDataFooter* jitGenDataFooter(PyGenObject* gen);
#endif

} // namespace jit

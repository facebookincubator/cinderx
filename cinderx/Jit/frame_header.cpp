// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/frame_header.h"

#if PY_VERSION_HEX < 0x030C0000
#include "internal/pycore_shadow_frame.h"
#endif

#include <cinderx/Common/code.h>
#include <cinderx/Common/util.h>
#include <cinderx/Jit/config.h>

#include <unordered_set>
#include <vector>

namespace jit {

#if PY_VERSION_HEX < 0x030C0000

int frameHeaderSize(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return 0;
  }

#ifdef ENABLE_SHADOW_FRAMES
  return sizeof(FrameHeader);
#else
  return 0;
#endif
}

void assertShadowCallStackConsistent(PyThreadState* tstate) {
  PyFrameObject* py_frame = tstate->frame;
  _PyShadowFrame* shadow_frame = tstate->shadow_frame;

  std::vector<_PyShadowFrame*> frames;
  while (shadow_frame) {
    frames.push_back(shadow_frame);
    if (_PyShadowFrame_GetPtrKind(shadow_frame) == PYSF_PYFRAME) {
      if (py_frame != _PyShadowFrame_GetPyFrame(shadow_frame)) {
        std::fprintf(stderr, "topmost:\n");
        for (size_t i = 0; i < frames.size(); i++) {
          _PyShadowFrame* sf = frames.at(i);
          Ref<> sf_name =
              Ref<>::steal(_PyShadowFrame_GetFullyQualifiedName(sf));
          const char* sf_name_str =
              sf_name == nullptr ? "<null>" : PyUnicode_AsUTF8(sf_name);
          if (sf_name_str == nullptr) {
            sf_name_str = "<null>";
          }
          std::fprintf(
              stderr,
              "  %s prev=%p data=%p name=%s\n",
              shadowFrameKind(sf),
              reinterpret_cast<void*>(shadow_frame->prev),
              reinterpret_cast<void*>(shadow_frame->data),
              sf_name_str);
        }
      }
      JIT_CHECK(
          py_frame == _PyShadowFrame_GetPyFrame(shadow_frame),
          "Inconsistent shadow and py frame ({} vs {})",
          codeName(py_frame->f_code),
          codeName(_PyShadowFrame_GetPyFrame(shadow_frame)->f_code));
      py_frame = py_frame->f_back;
    }
    shadow_frame = shadow_frame->prev;
  }

  if (py_frame != nullptr) {
    std::unordered_set<PyFrameObject*> seen;
    JIT_LOG(
        "Stack walk didn't consume entire python stack! Here's what's left:");
    PyFrameObject* left = py_frame;
    while (left && !seen.count(left)) {
      JIT_LOG("{}", PyUnicode_AsUTF8(left->f_code->co_name));
      seen.insert(left);
      left = left->f_back;
    }
    JIT_ABORT("stack walk didn't consume entire python stack");
  }
}

const char* shadowFrameKind(_PyShadowFrame* sf) {
  switch (_PyShadowFrame_GetPtrKind(sf)) {
    case PYSF_PYFRAME:
      return "fra";
    case PYSF_CODE_RT:
      return "crt";
    case PYSF_RTFS:
      return "inl";
    case PYSF_DUMMY:
      return "<dummy>";
  }
  JIT_ABORT("Unknown shadow frame kind {}", _PyShadowFrame_GetPtrKind(sf));
}

#else

int frameHeaderSize(BorrowedRef<PyCodeObject> code) {
  if (code->co_flags & kCoFlagsAnyGenerator) {
    return 0;
  }

  if (getConfig().frame_mode == FrameMode::kLightweight) {
    return sizeof(FrameHeader) + sizeof(PyObject*) * code->co_framesize;
  }

  return 0;
}

#endif

} // namespace jit

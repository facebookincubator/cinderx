// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/frame_header.h"
#include "cinderx/Jit/lir/type.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cinderx::jit::lir {

// Describes what to store for each _PyInterpreterFrame / FrameHeader field
// during lightweight frame setup. Offsets are relative to the
// _PyInterpreterFrame pointer (negative for FrameHeader fields).

enum class FrameFieldKind : uint8_t {
  kExecutable,
  kPrevFrame,
  kFuncObj,
  kFrameReifier,
  kInstrPtr,
  kStackPointer,
  kBuiltins,
  kGlobals,
  kZero,
  kOwnerThread,
  kFrameHeaderFunc,
  // The debug-only byte holding the visited/stackpointer_valid/lltrace
  // bitfields. Initialized so that stackpointer_valid=1 (others 0), matching
  // _PyFrame_Initialize, so the interpreter's first StackPointerInvalidate on a
  // JIT-built frame does not assert. Only emitted on Py_DEBUG 3.16+ builds.
  kDebugFrameByte,
};

constexpr const std::string_view frameFieldKindName(FrameFieldKind kind) {
  switch (kind) {
    case FrameFieldKind::kExecutable:
      return "executable";
    case FrameFieldKind::kPrevFrame:
      return "previous";
    case FrameFieldKind::kFuncObj:
      return "funcobj";
    case FrameFieldKind::kFrameReifier:
      return "frame_reifier";
    case FrameFieldKind::kInstrPtr:
      return "instr_ptr";
    case FrameFieldKind::kStackPointer:
      return "stackpointer";
    case FrameFieldKind::kBuiltins:
      return "builtins";
    case FrameFieldKind::kGlobals:
      return "globals";
    case FrameFieldKind::kZero:
      return "zero";
    case FrameFieldKind::kOwnerThread:
      return "owner";
    case FrameFieldKind::kFrameHeaderFunc:
      return "frame_header_func";
    case FrameFieldKind::kDebugFrameByte:
      return "debug_frame_byte";
  }
  return "unknown";
}

struct FrameFieldInit {
  int32_t offset;
  FrameFieldKind kind;
  DataType data_type;
};

struct FrameStoreGroup {
  uint8_t start;
  uint8_t count;
};

constexpr size_t kMaxFrameFields = 16;
constexpr size_t kMaxFrameGroups = 16;

struct FrameInitTable {
  // using std::array's so that we can sort at compile time, we
  // use some minimum size which will likely cover all cases
  // (_PyInterpreterFrame currently has 15 fields on 3.14) and the compile will
  // fail if we have too many.
  std::array<FrameFieldInit, kMaxFrameFields> fields{};
  size_t num_fields{0};
  std::array<FrameStoreGroup, kMaxFrameGroups> groups{};
  size_t num_groups{0};
};

// Version/config-specific field kinds, resolved at compile time.
// These aliases map semantic fields (f_executable, f_funcobj) to the
// FrameFieldKind that describes what value goes there on each version.
#ifndef ENABLE_LIGHTWEIGHT_FRAMES
// No lightweight frames, everything goes where you'd expect
inline constexpr auto kFuncObjKind = FrameFieldKind::kFuncObj;
inline constexpr auto kExecutableKind = FrameFieldKind::kExecutable;
#elif PY_VERSION_HEX >= 0x030E0000
// 3.14: store the reifier in the executable, function goes in normal spot
constexpr auto kFuncObjKind = FrameFieldKind::kFuncObj;
constexpr auto kExecutableKind = FrameFieldKind::kFrameReifier;
#else
// 3.12: store the function in the FrameHeader, reifier in function, executable
// is normal
constexpr auto kFuncObjKind = FrameFieldKind::kFrameReifier;
constexpr auto kExecutableKind = FrameFieldKind::kExecutable;
#endif

consteval FrameInitTable buildFrameInitTable() {
  FrameInitTable t;

  auto add = [&](int32_t offset, FrameFieldKind kind, DataType dt) {
    t.fields[t.num_fields++] = {offset, kind, dt};
  };

  // All offsets are relative to the _PyInterpreterFrame pointer.
  // FrameHeader fields have negative offsets.

  constexpr int32_t fh = -static_cast<int32_t>(sizeof(jit::FrameHeader));

  add(fh + static_cast<int32_t>(offsetof(jit::FrameHeader, frame_status)),
      FrameFieldKind::kFrameHeaderFunc,
      DataType::kObject);
  add(FRAME_EXECUTABLE_OFFSET, kExecutableKind, DataType::kObject);
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, previous)),
      FrameFieldKind::kPrevFrame,
      DataType::kObject);
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, f_funcobj)),
      kFuncObjKind,
      DataType::kObject);
  add(FRAME_INSTR_OFFSET, FrameFieldKind::kInstrPtr, DataType::kObject);
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, f_locals)),
      FrameFieldKind::kZero,
      DataType::kObject);

#if PY_VERSION_HEX >= 0x030E0000
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, stackpointer)),
      FrameFieldKind::kStackPointer,
      DataType::kObject);
#endif

#ifdef Py_GIL_DISABLED
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, tlbc_index)),
      FrameFieldKind::kZero,
      DataType::k32bit);
#ifdef ENABLE_LIGHTWEIGHT_FRAMES
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, frame_obj)),
      FrameFieldKind::kZero,
      DataType::kObject);
#endif
#endif

  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, owner)),
      FrameFieldKind::kOwnerThread,
      DataType::k8bit);

#ifndef ENABLE_LIGHTWEIGHT_FRAMES
  // Without ENABLE_LIGHTWEIGHT_FRAMES there is no lazy reification so
  // we must initialize every field the interpreter expects.
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, f_globals)),
      FrameFieldKind::kGlobals,
      DataType::kObject);
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, f_builtins)),
      FrameFieldKind::kBuiltins,
      DataType::kObject);
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, frame_obj)),
      FrameFieldKind::kZero,
      DataType::kObject);

  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, return_offset)),
      FrameFieldKind::kZero,
      DataType::k16bit);
#if PY_VERSION_HEX >= 0x030E0000
  // ugly, visited is a bitfield on debug builds and we can't use offset of on
  // it.
#if defined(Py_DEBUG) && PY_VERSION_HEX >= 0x03100000
  // On 3.16+ debug builds this byte also holds stackpointer_valid, which the
  // interpreter asserts is 1 on a freshly-spilled frame. Zeroing it (as below)
  // would trip _PyFrame_StackPointerInvalidate, so set stackpointer_valid=1.
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, owner) + 1),
      FrameFieldKind::kDebugFrameByte,
      DataType::k8bit);
#else
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, owner) + 1),
      FrameFieldKind::kZero,
      DataType::k8bit);
#endif
#else
  add(static_cast<int32_t>(offsetof(_PyInterpreterFrame, stacktop)),
      FrameFieldKind::kStackPointer,
      DataType::k32bit);
#endif

#endif

  // Sort by offset (insertion sort for consteval compatibility).
  for (size_t i = 1; i < t.num_fields; i++) {
    auto key = t.fields[i];
    size_t j = i;
    while (j > 0 && t.fields[j - 1].offset > key.offset) {
      t.fields[j] = t.fields[j - 1];
      j--;
    }
    t.fields[j] = key;
  }

  // Compute groups of consecutive pointer-sized stores.
  size_t i = 0;
  while (i < t.num_fields) {
    if (t.fields[i].data_type != DataType::kObject) {
      t.groups[t.num_groups++] = {
          static_cast<uint8_t>(i), static_cast<uint8_t>(1)};
      i++;
      continue;
    }
    size_t start = i;
    int32_t next_offset = t.fields[i].offset + kPointerSize;
    i++;
    while (i < t.num_fields && t.fields[i].data_type == DataType::kObject &&
           t.fields[i].offset == next_offset) {
      next_offset += kPointerSize;
      i++;
    }
    t.groups[t.num_groups++] = {
        static_cast<uint8_t>(start), static_cast<uint8_t>(i - start)};
  }

  return t;
}

constexpr FrameInitTable kFrameInitTable = buildFrameInitTable();

} // namespace cinderx::jit::lir

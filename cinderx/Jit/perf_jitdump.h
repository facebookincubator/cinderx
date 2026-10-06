// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace cinderx::jit::perf {

// Prefix to use for JIT-compiled Python functions.
constexpr std::string_view kFuncSymbolPrefix{"__CINDER_JIT"};

// Prefix to use for JIT-compiled code internal to CinderX, such as shared
// trampolines.
constexpr std::string_view kInternalSymbolPrefix{"__CINDER_INFRA_JIT"};

// Entry that gets written to a perf map or a JIT dump file.
struct Entry {
  const char* name{};
  const void* addr{};
  size_t size{};
};

// General-purpose writer of perf JIT events.
//
// Subclasses of this are only supported on Linux and macOS.  They will be nops
// on Windows.
struct Writer {
  virtual ~Writer() = default;

  // Emit a perf event for a range of JIT-compiled code.
  virtual void writeEntry(const Entry& entry) = 0;

  // Flush all events to whatever output stream backs this writer.
  virtual void flush() = 0;

  // After-fork callback for child processes.  Performs any cleanup necessary
  // for per-process state, including handling of Linux perf pid maps.
  virtual void afterForkChild() {}
};

// Implements writing to perf map files.
std::unique_ptr<Writer> makePerfMapWriter(bool prefork_enabled);

// Implements writing to perf JIT dump files.
//
// Used by 'perf inject' and 'perf report'. The format is documented here:
// https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Documentation/jitdump-specification.txt.
std::unique_ptr<Writer> makeJitDumpWriter(std::string_view jit_dump_dir);

} // namespace cinderx::jit::perf

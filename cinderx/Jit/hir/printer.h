// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/function.h"
#include "cinderx/Jit/hir/hir.h"
#include "fmt/ostream.h"

#include <sstream>
#include <string>
#include <string_view>

namespace jit::hir {

// Helper class for pretty printing IR.
//
// This works, but feels horribly kludgy. This should be possible using a custom
// streambuf for indentation and via overloads for <<.
class HIRPrinter {
 public:
  // Construct an HIRPrinter.
  //
  // If full_snapshots is false, Snapshot instructions printed as part of a
  // BasicBlock, CFG, or Function, will be printed only as the opcode name,
  // with no FrameState. When printing individual instructions, each caller can
  // specify whether or not the full instruction should be printed.
  HIRPrinter() = default;

  void Print(std::ostream& os, const Function& func);
  void Print(std::ostream& os, const BasicBlock& block);
  void Print(std::ostream& os, const Instr& instr);
  void Print(std::ostream& os, const CFG& cfg);
  void Print(std::ostream& os, const FrameState& state);

  template <class T>
  std::string ToString(const T& obj) {
    std::ostringstream os;
    Print(os, obj);
    return os.str();
  }

  HIRPrinter& setFullSnapshots(bool full);
  HIRPrinter& setLinePrefix(std::string_view prefix);

 private:
  void Indent();
  void Dedent();
  std::ostream& Indented(std::ostream& os);

  const Function* func_{nullptr};
  std::string line_prefix_;
  int indent_level_{0};
  bool full_snapshots_{false};
};

std::ostream& operator<<(std::ostream& os, const Function& func);
std::ostream& operator<<(std::ostream& os, const CFG& cfg);
std::ostream& operator<<(std::ostream& os, const BasicBlock& block);
std::ostream& operator<<(std::ostream& os, const Instr& instr);
std::ostream& operator<<(std::ostream& os, const FrameState& state);

} // namespace jit::hir

template <>
struct fmt::formatter<jit::hir::Function> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<jit::hir::CFG> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<jit::hir::BasicBlock> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<jit::hir::Instr> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<jit::hir::DeoptBase> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<jit::hir::Phi> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<jit::hir::FrameState> : fmt::ostream_formatter {};

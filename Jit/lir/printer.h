// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/lir/block.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/operand.h"
#include "fmt/ostream.h"

#include <iosfwd>

namespace jit::lir {

class Printer {
 public:
  void print(std::ostream& out, const Function& func);
  void print(std::ostream& out, const BasicBlock& block);
  void print(std::ostream& out, const Instruction& instr);
  void print(std::ostream& out, const OperandBase& operand);
  void print(std::ostream& out, const MemoryIndirect& memind);

 private:
  const Function* getFunction(const BasicBlock& block) {
    return block.function();
  }
  const Function* getFunction(const Instruction& instr) {
    return getFunction(*instr.basicblock());
  }
  const Function* getFunction(const OperandBase& opnd) {
    return getFunction(*opnd.instr());
  }

  hir::HIRPrinter hir_printer_{false, "# "};
};

inline std::ostream& operator<<(std::ostream& out, const Function& func) {
  Printer().print(out, func);
  return out;
}

inline std::ostream& operator<<(std::ostream& out, const BasicBlock& block) {
  Printer().print(out, block);
  return out;
}

inline std::ostream& operator<<(std::ostream& out, const Instruction& instr) {
  Printer().print(out, instr);
  return out;
}

inline std::ostream& operator<<(std::ostream& out, const OperandBase& operand) {
  Printer().print(out, operand);
  return out;
}

inline std::ostream& operator<<(
    std::ostream& out,
    const MemoryIndirect& memind) {
  Printer().print(out, memind);
  return out;
}

} // namespace jit::lir

template <>
struct fmt::formatter<jit::lir::Function> : fmt::ostream_formatter {};
template <>
struct fmt::formatter<jit::lir::BasicBlock> : fmt::ostream_formatter {};
template <>
struct fmt::formatter<jit::lir::Instruction> : fmt::ostream_formatter {};
template <>
struct fmt::formatter<jit::lir::OperandBase> : fmt::ostream_formatter {};
template <>
struct fmt::formatter<jit::lir::MemoryIndirect> : fmt::ostream_formatter {};

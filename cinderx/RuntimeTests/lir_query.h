// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <gtest/gtest.h>

#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/type.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace cinderx::jit::lir {

// Fluent query over a LIR Function for unit tests, replacing brittle string
// matching. Predicates are ANDed; exists() walks basic blocks in sorted order
// then instructions in program order and reports whether any matches. Use the
// EXPECT_LIR / EXPECT_NO_LIR macros below to assert on a query.
//
// Example:
//   EXPECT_LIR(Query(func).opcode(Instruction::kMove)
//                  .outType(DataType::k64bit).inImm(0, 12));
class Query {
 public:
  explicit Query(const Function& func);

  Query& opcode(Instruction::Opcode op);
  // Match the output operand's data type / LIR id (`%id`).
  Query& outType(DataType dt);
  Query& outVreg(int id);
  // Match input operand `index` as the immediate `v`.
  Query& inImm(size_t index, uint64_t v);
  // Match input operand `index` as the immediate or memory address `addr`.
  Query& inAddr(size_t index, uint64_t addr);
  // Match input operand `index` as a reference to the definition `%id`.
  Query& inVreg(size_t index, int id);
  // Match input operand `index`'s data type.
  Query& inType(size_t index, DataType dt);
  // Arbitrary extra predicate on the instruction.
  Query& with(std::function<bool(const Instruction*)> pred);

  bool exists() const;

  const Function& func() const;

 private:
  struct InputMatch {
    size_t index{0};
    std::optional<uint64_t> imm;
    std::optional<uint64_t> addr;
    std::optional<int> vreg;
    std::optional<DataType> type;
  };

  InputMatch& input(size_t index);
  bool matches(const Instruction& ins) const;
  bool matchesOutput(const Instruction& ins) const;
  bool matchesInputs(const Instruction& ins) const;
  bool matchesInput(const Instruction& ins, const InputMatch& im) const;
  const Instruction* find() const;

  const Function& func_;
  std::optional<Instruction::Opcode> opcode_;
  std::optional<DataType> out_type_;
  std::optional<int> out_vreg_;
  std::vector<InputMatch> inputs_;
  std::function<bool(const Instruction*)> extra_;
};

// Format a LIR function for an EXPECT_LIR failure message.
std::string lirFuncString(const Function& func);

} // namespace cinderx::jit::lir

// Assert that a Query matches (or does not match) an instruction, dumping
// the queried function on failure. A custom message can still be chained:
//   EXPECT_LIR(Query(func).opcode(Instruction::kMove).inImm(0, 12));
//   EXPECT_NO_LIR(Query(func).opcode(Instruction::kCall)) << "unexpected";
// The function dump (and any chained message) is only evaluated on failure.
#define EXPECT_LIR(query)       \
  EXPECT_TRUE((query).exists()) \
      << ::cinderx::jit::lir::lirFuncString((query).func())
#define EXPECT_NO_LIR(query)     \
  EXPECT_FALSE((query).exists()) \
      << ::cinderx::jit::lir::lirFuncString((query).func())

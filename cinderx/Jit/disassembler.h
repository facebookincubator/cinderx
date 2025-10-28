// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <ostream>

namespace jit {

struct Disassembler {
  Disassembler(const char* buf, size_t size);

  // Disassemble a single instruction.
  void disassembleOne(std::ostream& os);

  // Disassemble the entire buffer.
  void disassembleAll(std::ostream& os);

  // Get the address the disassembler is currently pointing at.
  const char* cursor() const;

  void setPrintAddr(bool print);
  void setPrintInstBytes(bool print);

 private:
  const char* const buf_;
  size_t start_{0};
  size_t const size_;
  size_t addr_len_{16};
  bool print_addr_{true};
  bool print_instr_bytes_{true};

  size_t disassemblerHandle();
  void disassemble(std::ostream& os, size_t handle);
};

} // namespace jit

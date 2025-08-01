// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/util.h"

#include <ostream>

// Uses i386-dis internally but that's not exposed here.
struct disassemble_info;

namespace jit {

struct Disassembler {
  Disassembler(const char* buf, size_t size);
  ~Disassembler();

  // Disassemble a single instruction.
  void disassembleOne(std::ostream& os);

  // Disassemble the entire buffer.
  void disassembleAll(std::ostream& os);

  // Format the current code address as a string.
  void codeAddress(std::ostream& os);

  // Get the address the disassembler is currently pointing at.
  const char* cursor() const;

  void setPrintAddr(bool print);
  void setPrintInstBytes(bool print);

 private:
  std::unique_ptr<disassemble_info> info_;
  const char* const buf_;
  size_t start_{0};
  size_t const size_;
  size_t addr_len_{16};
  jit_string_t* const sfile_;
  bool print_addr_{true};
  bool print_instr_bytes_{true};
};

} // namespace jit

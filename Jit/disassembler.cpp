// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/disassembler.h"

#include "cinderx/Jit/symbolizer.h"
#include "i386-dis/dis-asm.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>

namespace jit {

namespace {

std::mutex dis_mtx;

size_t get_address_hex_length(vma_t vma) {
  size_t i = sizeof(vma_t) * 8 - __builtin_clzl(vma);
  size_t j = i % 4;
  return (j > 0 ? i / 4 + 1 : i / 4) + 2;
}

void print_address(vma_t vma, disassemble_info* info) {
  size_t addr_len = get_address_hex_length(info->stop_vma);
  info->fprintf_func(info->stream, "%#0*lx", addr_len, vma);
}

void print_symbol(vma_t addr, disassemble_info* info) {
  // At some point in the future we may want a more complete solution like
  // https://github.com/facebook/hhvm/blob/0ff8dca4f1174f3ffa9c5d282ae1f5b5523fe56c/hphp/util/abi-cxx.cpp#L64
  std::optional<std::string> symbol =
      symbolize(reinterpret_cast<const void*>(addr));
  if (symbol.has_value()) {
    info->fprintf_func(info->stream, " (%s)", symbol->c_str());
  }
}

} // namespace

Disassembler::Disassembler(const char* buf, size_t size)
    : info_{std::make_unique<disassemble_info>()},
      buf_(buf),
      size_{size},
      sfile_{ss_alloc()} {
  auto vma = reinterpret_cast<vma_t>(buf);

  memset(info_.get(), 0, sizeof(*info_));

  info_->fprintf_func = (int (*)(void*, const char*, ...))ss_sprintf;
  info_->stream = sfile_;
  info_->octets_per_byte = 1;
  info_->read_memory_func = buffer_read_memory;
  info_->memory_error_func = perror_memory;
  info_->print_address_func = print_address;
  info_->print_symbol_func = print_symbol;
  info_->stop_vma = (uintptr_t)vma + size;
  info_->buffer = (unsigned char*)buf;
  info_->buffer_length = size;
  info_->buffer_vma = vma;
}

Disassembler::~Disassembler() {
  ss_free(sfile_);
}

void Disassembler::codeAddress(std::ostream& os) {
  size_t addr_len = get_address_hex_length(info_->stop_vma);
  fmt::print(os, "{:#0{}x}", reinterpret_cast<vma_t>(cursor()), addr_len);
}

void Disassembler::disassembleOne(std::ostream& os) {
  if (print_addr_) {
    codeAddress(os);
    fmt::print(os, ":{:8}", "");
  }

  int length = 0;
  {
    // i386-dis is not thread-safe.
    std::lock_guard<std::mutex> lock{dis_mtx};
    length = print_insn(reinterpret_cast<vma_t>(cursor()), info_.get());
  }

  if (print_instr_bytes_) {
    for (long i = start_; i < start_ + 8; i++) {
      if (i < start_ + length) {
        fmt::print(os, "{:02x} ", static_cast<unsigned char>(buf_[i]));
      } else {
        fmt::print(os, "   ");
      }
    }
  }

  os << ss_get_string(sfile_);
  ss_reset(sfile_);

  start_ += length;
}

void Disassembler::disassembleAll(std::ostream& os) {
  while (start_ < size_) {
    disassembleOne(os);
    os << '\n';
  }
}

const char* Disassembler::cursor() const {
  return buf_ + start_;
}

void Disassembler::setPrintAddr(bool print) {
  print_addr_ = print;
}

void Disassembler::setPrintInstBytes(bool print) {
  print_instr_bytes_ = print;
}

} // namespace jit

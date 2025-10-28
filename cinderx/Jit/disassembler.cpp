// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/disassembler.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/symbolizer.h"
#include "fmt/ostream.h"

#if defined(__x86_64__)
#include "cinderx/Jit/config.h"
#endif

#ifndef ENABLE_DISASSEMBLER
namespace jit {

Disassembler::Disassembler(const char* buf, size_t size)
    : buf_(buf), size_{size} {}

void Disassembler::disassembleOne(std::ostream& os) {}
void Disassembler::disassembleAll(std::ostream& os) {}

const char* Disassembler::cursor() const {
  return nullptr;
}

void Disassembler::setPrintAddr(bool print) {}
void Disassembler::setPrintInstBytes(bool print) {}

size_t Disassembler::disassemblerHandle() {
  return 0;
}

void Disassembler::disassemble(std::ostream& os, size_t handle) {}

} // namespace jit
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wduplicate-enum"
#include "capstone/capstone.h"
#pragma GCC diagnostic pop

namespace jit {

Disassembler::Disassembler(const char* buf, size_t size)
    : buf_(buf), size_{size} {}

void Disassembler::disassembleOne(std::ostream& os) {
  csh handle = disassemblerHandle();
  disassemble(os, handle);
  cs_close(&handle);
}

void Disassembler::disassembleAll(std::ostream& os) {
  csh handle = disassemblerHandle();
  while (start_ < size_) {
    disassemble(os, handle);
    os << '\n';
  }
  cs_close(&handle);
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

size_t Disassembler::disassemblerHandle() {
  cs_arch cs_arch;
  cs_mode cs_mode;
  csh handle;
  cs_err err;

#if defined(__x86_64__)
  cs_arch = CS_ARCH_X86;
  cs_mode = CS_MODE_64;
#elif defined(__aarch64__)
  cs_arch = CS_ARCH_ARM64;
  cs_mode = CS_MODE_ARM;
#else
  throw std::runtime_error{"Unsupported architecture"};
#endif

  err = cs_open(cs_arch, cs_mode, &handle);
  JIT_CHECK(err == CS_ERR_OK, "Failed to open Capstone handle");

  return handle;
}

void Disassembler::disassemble(std::ostream& os, size_t handle) {
  if (print_addr_) {
    fmt::print(
        os,
        fmt::runtime("{:#0{}x}:{:8}"),
        reinterpret_cast<size_t>(cursor()),
        addr_len_,
        "");
  }

  cs_err err = cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
  JIT_CHECK(err == CS_ERR_OK, "Failed to enable Capstone detail");

#if defined(__x86_64__)
  err = cs_option(
      handle,
      CS_OPT_SYNTAX,
      getConfig().asm_syntax == AsmSyntax::ATT ? CS_OPT_SYNTAX_ATT
                                               : CS_OPT_SYNTAX_INTEL);
  JIT_CHECK(err == CS_ERR_OK, "Failed to set Capstone syntax");
#endif

  auto code = reinterpret_cast<const uint8_t*>(cursor());
  size_t size = size_ - start_;
  uint64_t address = reinterpret_cast<uint64_t>(cursor());

  auto insn_deleter = [](cs_insn* insn) { cs_free(insn, 1); };
  auto insn = std::unique_ptr<cs_insn, decltype(insn_deleter)>(
      cs_malloc(handle), insn_deleter);

  if (!cs_disasm_iter(handle, &code, &size, &address, insn.get())) {
    throw std::runtime_error{"Failed to disassemble instruction"};
  }

  fmt::print(os, "{} {}", insn->mnemonic, insn->op_str);
  const void* symbol = nullptr;

#if defined(__x86_64__)
  uint8_t* opcode = insn->detail->x86.opcode;

  // Check for all of the variants of a call instruction. On x86-64, call
  // instructions can take a raw pointer to the function to call, or they can
  // accept a RIP-relative offset to the function to call. Either way, we want
  // to add that symbol into the disassembly.
  if (opcode[0] == 0xe8 || opcode[0] == 0x9a || opcode[0] == 0xff) {
    cs_x86_op* opnd = &insn->detail->x86.operands[0];

    switch (opnd->type) {
      case X86_OP_IMM:
        symbol = reinterpret_cast<const void*>(opnd->imm);
        break;
      case X86_OP_MEM:
        if (opnd->mem.base == X86_REG_RIP) {
          symbol = reinterpret_cast<const void*>(address + opnd->mem.disp);
        }
        break;
      default:
        break;
    }
  }
#endif

  if (symbol) {
    std::optional<std::string> symbol_name = symbolize(symbol);
    if (symbol_name.has_value()) {
      fmt::print(os, "\t({})", symbol_name.value());
    }
  }

  if (print_instr_bytes_) {
    for (long i = start_; i < start_ + 8; i++) {
      if (i < start_ + insn->size) {
        fmt::print(os, "{:02x} ", static_cast<unsigned char>(buf_[i]));
      } else {
        fmt::print(os, "   ");
      }
    }
  }

  start_ += insn->size;
}

} // namespace jit
#endif

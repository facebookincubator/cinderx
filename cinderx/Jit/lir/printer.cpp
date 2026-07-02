// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/printer.h"

#include "cinderx/Jit/codegen/code_section.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/lir/arch.h"
#include "cinderx/Jit/lir/operand.h"

#include <fmt/ostream.h>

#include <iomanip>

namespace cinderx::jit::lir {

Printer::Printer() {
  hir_printer_.setFullSnapshots(true);
  hir_printer_.setLinePrefix("# ");
}

void Printer::print(std::ostream& out, const Function& func) {
  out << "Function:\n";
  for (auto& block : func.basicblocks()) {
    print(out, *block);
    out << '\n';
  }
}

void Printer::print(std::ostream& out, const BasicBlock& block) {
  out << "BB %" << block.id();

  auto print_blocks = [&](const char* which,
                          std::vector<BasicBlock*> blocks,
                          bool is_sorted = true) {
    if (is_sorted) {
      std::sort(blocks.begin(), blocks.end(), [](auto& a, auto& b) {
        return a->id() < b->id();
      });
    }
    if (!blocks.empty()) {
      out << which;
      for (auto& b : blocks) {
        out << " %" << b->id();
      }
    }
  };

  print_blocks(" - preds:", block.predecessors());
  print_blocks(" - succs:", block.successors(), false);
  auto section = block.section();
  // Avoid printing hot sections to keep the printouts a bit less noisy.
  if (section != codegen::CodeSection::kHot) {
    out << " - section: " << codeSectionName(section);
  }
  out << '\n';

  const hir::Instr* prev_instr = nullptr;
  for (auto& instr : block.instructions()) {
    if (getConfig().log.lir_origin && instr->origin() != prev_instr) {
      if (instr->origin()) {
        out << '\n';
        hir_printer_.print(out, *instr->origin());
        out << '\n';
      }
      prev_instr = instr->origin();
    }
    print(out, *instr);
    out << '\n';
  }
}

void Printer::print(std::ostream& out, const Instruction& instr) {
  auto output_opnd = instr.output();
  if (output_opnd->type() == Operand::kNone) {
    fmt::print(out, "{:>16}   ", "");
  } else {
    std::stringstream ss;
    print(ss, *output_opnd);
    out << std::setw(16) << ss.str() << " = ";
  }
  out << InstrProperty::getProperties(&instr).name;
  const char* sep = " ";
  if (instr.opcode() == Instruction::kPhi) {
    auto num_inputs = instr.getNumInputs();
    for (size_t i = 0; i < num_inputs; i += 2) {
      out << sep << "(";
      print(out, *(instr.getInput(i)));
      sep = ", ";
      out << sep;
      print(out, *(instr.getInput(i + 1)));
      out << ")";
    }
  } else {
    instr.foreachInputOperand([&sep, &out, this](const Operand* operand) {
      out << sep;
      print(out, *operand);
      sep = ", ";
    });
  }
}

void Printer::print(std::ostream& out, const Operand& operand) {
  if (operand.isLinked()) {
    print(out, *operand.getLinkedOperand());
    return;
  }

  switch (operand.type()) {
    case Operand::kVreg:
      out << "%" << operand.instr()->id();
      break;
    case Operand::kReg:
      out << PhyLocation(operand.getPhyRegister());
      break;
    case Operand::kStack:
      out << PhyLocation(operand.getStackSlot());
      break;
    case Operand::kMem:
      out << "[" << std::hex << operand.getMemoryAddress() << "]" << std::dec;
      break;
    case Operand::kInd:
      out << *operand.getMemoryIndirect();
      break;
    case Operand::kImm:
      fmt::print(out, "{0}({0:#x})", operand.getConstant());
      break;
    case Operand::kLabel:
      out << "BB%" << operand.getBasicBlock()->id();
      break;
    case Operand::kNone:
      out << "<!!!None!!!>";
      break;
  }

  if (!operand.isLabel()) {
    out << ":" << operand.dataType();
  }
}

void Printer::print(std::ostream& out, const MemoryIndirect& ind) {
  fmt::print(out, "[{}", *ind.getBaseRegOperand());

  auto index_reg = ind.getIndexRegOperand();
  if (index_reg != nullptr) {
    fmt::print(out, " + {}", *index_reg);

    int multiplier = ind.getMultiplier();
    if (multiplier > 0) {
      fmt::print(out, " * {}", 1 << multiplier);
    }
  }

  auto offset = ind.getOffset();
  if (offset != 0) {
    if (offset > 0) {
      fmt::print(out, " + {0:#x}", offset);
    } else {
      fmt::print(out, " - {0:#x}", -offset);
    }
  }

  fmt::print(out, "]");
}

} // namespace cinderx::jit::lir

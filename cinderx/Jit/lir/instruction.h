// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/define.h"
#include "cinderx/Jit/lir/operand.h"
#include "cinderx/Jit/lir/ops.h"

#include <memory>
#include <string_view>
#include <vector>

namespace cinderx::jit {
namespace hir {
class Instr;
}

namespace lir {

class BasicBlock;

// Instruction class defines instructions in LIR.
// Every instruction can have no more than one output, but arbitrary
// number of inputs. The instruction logically has no output also
// has an output data member with the type kNone.
class Instruction {
 public:
#define DECL_OPCODE_TEST(v, ...)     \
  bool is##v() const {               \
    return opcode() == Opcode::k##v; \
  }
  FOREACH_LIR_OPCODE(DECL_OPCODE_TEST)
#undef DECL_OPCODE_TEST

  Instruction(BasicBlock* basic_block, Opcode opcode, const hir::Instr* origin);

  // Copies another instruction's opcode and simple fields from its output.  The
  // inputs are not copied.
  Instruction(BasicBlock* block, Instruction* instr, const hir::Instr* origin);

  // Get the unique ID representing this instruction within its function.
  int id() const;

  // Change the instruction's ID.  This is only meant to be used by the LIR
  // parser.  LIR strongly expects unique instruction IDs.
  void setId(int id);

  // Get the output of this function.
  //
  // All functions have an output object, even if they don't use it.
  Operand* output();
  const Operand* output() const;

  // Get the HIR instruction that this LIR instruction was lowered from.
  const hir::Instr* origin() const;

  // Get the number of inputs passed into this instruction.
  size_t getNumInputs() const;

  // Change the number of inputs passed into this instruction.  Will add nullptr
  // Operand objects if the number increases.
  void setNumInputs(size_t n);

  // Get the number of outputs set by this instruction.
  size_t getNumOutputs() const;

  // Get an input by index.
  Operand* getInput(size_t i);
  const Operand* getInput(size_t i) const;

  Operand* allocateImmediateInput(
      uint64_t n,
      DataType data_type = DataType::k64bit);
  Operand* allocateFPImmediateInput(double n);
  Operand* allocateLinkedInput(Instruction* def_instr);
  Operand* allocatePhyRegisterInput(PhyLocation loc);
  Operand* allocateStackInput(PhyLocation stack);
  Operand* allocatePhyRegOrStackInput(PhyLocation loc);
  Operand* allocateAddressInput(void* address);
  Operand* allocateLabelInput(BasicBlock* block);
  Operand* allocateAsmLabelInput(const asmjit::Label& label);

  template <typename... Args>
  Operand* allocateMemoryIndirectInput(Args&&... args) {
    auto operand = std::make_unique<Operand>(this);
    auto operand_ptr = operand.get();
    operand->setMemoryIndirect(std::forward<Args>(args)...);
    inputs_.push_back(std::move(operand));
    return operand_ptr;
  }

  // add operands to the instruction. The arguments can be one
  // of the following:
  // - [Out]PhyReg(phyreg, size): a physical register
  // - [Out]Imm(imm, size): an immediate
  // - [Out]Stack(slot, size): a stack slot
  // - [Out]Lbl(Basicblock): a basic block target
  // - VReg(instr), OutVReg(size): a virtual register
  // the arguments with the names prefixed with `Out` are output operands.
  // the output operand must be the first argument of this function.
  template <typename FirstT, typename... T>
  Instruction* addOperands(FirstT&& first_arg, T&&... args) {
    static_assert(
        !(std::decay_t<decltype(args)>::is_output || ... || false),
        "output must be the first argument.");

    using FT = std::decay_t<FirstT>;

    if constexpr (std::is_same_v<FT, PhyReg>) {
      allocatePhyRegisterInput(first_arg.value)
          ->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, Stk>) {
      allocateStackInput(first_arg.value)->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, Imm>) {
      allocateImmediateInput(first_arg.value)->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, FPImm>) {
      allocateFPImmediateInput(first_arg.value)->setDataType(Operand::kDouble);
    } else if constexpr (std::is_same_v<FT, MemImm>) {
      allocateAddressInput(first_arg.value)->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, Lbl>) {
      allocateLabelInput(first_arg.value);
    } else if constexpr (std::is_same_v<FT, AsmLbl>) {
      allocateAsmLabelInput(first_arg.value);
    } else if constexpr (std::is_same_v<FT, VReg>) {
      allocateLinkedInput(first_arg.value);
    } else if constexpr (std::is_same_v<FT, Ind>) {
      allocateMemoryIndirectInput(
          first_arg.base,
          first_arg.index,
          first_arg.multiplier,
          first_arg.offset);
    } else if constexpr (std::is_same_v<FT, OutPhyReg>) {
      output()->setPhyRegister(first_arg.value);
      output()->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, OutStk>) {
      output()->setStackSlot(first_arg.value);
      output()->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, OutImm>) {
      output()->setConstant(first_arg.value);
      output()->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, OutFPImm>) {
      output()->setFPConstant(first_arg.value);
      output()->setDataType(Operand::kDouble);
    } else if constexpr (std::is_same_v<FT, OutMemImm>) {
      output()->setMemoryAddress(first_arg.value);
      output()->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, OutLbl>) {
      output()->setBasicBlock(first_arg.value);
    } else if constexpr (std::is_same_v<FT, OutVReg>) {
      output()->setVirtualRegister();
      output()->setDataType(first_arg.data_type);
    } else if constexpr (std::is_same_v<FT, OutInd>) {
      output()->setMemoryIndirect(
          first_arg.base,
          first_arg.index,
          first_arg.multiplier,
          first_arg.offset);
    } else {
      static_assert(!sizeof(FT*), "Bad argument type.");
    }

    return addOperands(std::forward<T>(args)...);
  }

  constexpr Instruction* addOperands() {
    return this;
  }

  void setBasicBlock(BasicBlock* bb);

  BasicBlock* basicBlock();
  const BasicBlock* basicBlock() const;

  Opcode opcode() const;
  void setOpcode(Opcode opcode);

  // Get the name of this instruction's opcode.  This is a null-terminated
  // literal value.
  std::string_view opname() const;

  template <typename Func>
  void foreachInputOperand(const Func& f) const {
    for (size_t i = 0; i < this->getNumInputs(); i++) {
      auto operand = getInput(i);
      f(operand);
    }
  }

  template <typename Func>
  void foreachInputOperand(const Func& f) {
    for (size_t i = 0; i < this->getNumInputs(); i++) {
      auto operand = getInput(i);
      f(operand);
    }
  }

  // Set an input by index, deleting the previous input.  Does not resize the
  // inputs list.
  void setInput(size_t index, std::unique_ptr<Operand> input);

  // Remove an input by index, shifting all other inputs to the left.
  std::unique_ptr<Operand> removeInput(size_t index);

  // Release the input operand at index from the instruction without
  // deallocating it.  The original input slot will be left with a nullptr,
  // which is meant be removed afterwards.
  std::unique_ptr<Operand> releaseInput(size_t index);

  // Add a new input to the end of this instruction's input list.
  Operand* appendInput(std::unique_ptr<Operand> operand);

  // Add a new input to the beginning of this instruction's input list.
  Operand* prependInput(std::unique_ptr<Operand> operand);

  // get the operand associated to a given predecessor in a phi instruction
  // returns nullptr if not found.
  Operand* getOperandByPredecessor(const BasicBlock* pred);

  int getOperandIndexByPredecessor(const BasicBlock* pred) const;

  const Operand* getOperandByPredecessor(const BasicBlock* pred) const;

  // Accessors for some of the instruction's attributes.
  bool getOutputPhyRegUse() const;
  bool getInputPhyRegUse(size_t i) const;
  bool inputsLiveAcross() const;

 private:
  template <typename FType, typename... AType>
  Operand* allocateOperand(FType&& set_func, AType&&... arg) {
    auto operand = std::make_unique<Operand>(this);
    auto operand_ptr = operand.get();
    (operand_ptr->*set_func)(std::forward<AType>(arg)...);
    inputs_.push_back(std::move(operand));
    return operand_ptr;
  }

  int id_;
  Opcode opcode_;
  Operand output_;
  BasicBlock* basic_block_;
  const hir::Instr* origin_;
  std::vector<std::unique_ptr<Operand>> inputs_;
};

// Kind of condition that a Guard instruction will execute.
enum InstrGuardKind {
  kAlwaysFail,
  kHasType,
  kIs,
  kNotNegative,
  kNotZero,
  kZero,
};

} // namespace lir
} // namespace cinderx::jit

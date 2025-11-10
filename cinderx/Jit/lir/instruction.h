// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/lir/operand.h"

#include <memory>
#include <string_view>
#include <vector>

namespace jit {
namespace hir {
class Instr;
}

namespace lir {

class BasicBlock;

/*
 * FlagEffects describes the effect an LIR instruction has on the machine's
 * status flags.
 */
enum class FlagEffects {
  /* The instruction does not modify flags. */
  kNone,

  /* The instruction sets flags to a meaningful value (e.g., a comparison
   * instruction). */
  kSet,

  /* The instruction clobbers flags (e.g., a call instruction). */
  kInvalidate,
};

/* OperandSizeType describes how an LIR instruction's operand sizes are
 * determined. */
enum OperandSizeType {
  /* Every operand uses the size determined by its DataType. */
  kDefault,

  /* Every operand is 64 bits. */
  kAlways64,

  /* Every operand is the same size as the output, or the first input (when
   * there is no output). */
  kOut,
};

/*
 * FOREACH_INSTR_TYPE defines all LIR instructions and their attributes. Every
 * argument after the name is optional, and each call to X expects:
 * X(name, inputs_live_across, flag_effects, opnd_size_type, out_phy_use,
 *   in_phy_uses, is_essential)
 *
 * - inputs_live_across: bool, default false. When false, the instruction's
 *   operands will only be considered live until the beginning of the
 *   instruction, meaning the output may be assigned to the same register as
 *   one of the inputs (if no other instruction keeps them alive longer). When
 *   true, the operands will be considered live until the end of the
 *   instruction, which allows codegen for the instruction to read its inputs
 *   after writing to its output, at the expense of slightly increased register
 *   pressure.
 *
 * - flag_effects: FlagEffects, default kNone. Specifies the instruction's
 *   effects on the processor's status flags. See FlagEffects for details.
 *
 * - opnd_size_type: OperandSizeType, default kDefault. Specifies the size of
 *   operands. See OperandSizeType for details.
 *
 * - out_phy_use: bool, default true. When true, the output must be allocated
 *   to a physical register. When false, it may be allocated to a stack slot.
 *
 * - in_phy_uses: vector<bool>, default {false, ...}. Any true slots indicate
 *   inputs that must be allocated to physical registers (as opposed to stack
 *   slots).
 *
 * - is_essential: bool, default false. When true, indicates that the
 *   instruction has side-effects and should never be removed by dead code
 *   elimination. Any instruction with no output must be marked as essential
 *   (if it doesn't define an output and has no side-effects, what does it
 *   do?).
 */
#define FOREACH_INSTR_TYPE(X)                                                 \
  /* Bind is not used to generate any machine code. Its sole      */          \
  /* purpose is to associate a physical register with a predefined */         \
  /* value to virtual register for register allocator. */                     \
  X(Bind)                                                                     \
  X(Nop)                                                                      \
  X(Unreachable, false, FlagEffects::kNone, kDefault, 0, {}, 1)               \
  X(Call, false, FlagEffects::kInvalidate, kAlways64, 1, {}, 1)               \
  X(VectorCall, false, FlagEffects::kInvalidate, kAlways64, 1, {1}, 1)        \
  X(VarArgCall, false, FlagEffects::kInvalidate, kDefault, 1, {1})            \
  X(Guard, false, FlagEffects::kInvalidate, kDefault, 1, {0, 0, 1, 1}, 1)     \
  X(DeoptPatchpoint, false, FlagEffects::kInvalidate, kDefault, 0, {1, 1}, 1) \
  X(Sext)                                                                     \
  X(Zext)                                                                     \
  X(Negate, false, FlagEffects::kSet, kOut)                                   \
  X(Invert, false, FlagEffects::kNone, kOut)                                  \
  X(Add, false, FlagEffects::kSet, kOut, 1, {1})                              \
  X(Sub, true, FlagEffects::kSet, kOut, 1, {1})                               \
  X(And, false, FlagEffects::kSet, kOut, 1, {1})                              \
  X(Xor, false, FlagEffects::kSet, kOut, 1, {1})                              \
  X(Div, false, FlagEffects::kSet, kDefault, 1, {1})                          \
  X(DivUn, false, FlagEffects::kSet, kDefault, 1, {1})                        \
  X(Mul, false, FlagEffects::kSet, kOut, 1, {1})                              \
  X(Or, false, FlagEffects::kSet, kOut, 1, {1})                               \
  X(Fadd, false, FlagEffects::kNone, kAlways64, 1, {1, 1})                    \
  X(Fsub, true, FlagEffects::kNone, kAlways64, 1, {1, 1})                     \
  X(Fmul, false, FlagEffects::kNone, kAlways64, 1, {1, 1})                    \
  X(Fdiv, true, FlagEffects::kNone, kAlways64, 1, {1, 1})                     \
  X(LShift, false, FlagEffects::kSet)                                         \
  X(RShift, false, FlagEffects::kSet)                                         \
  X(RShiftUn, false, FlagEffects::kSet)                                       \
  X(Test, false, FlagEffects::kSet, kDefault, 0, {1, 1})                      \
  X(Test32, false, FlagEffects::kSet, kDefault, 0, {1, 1})                    \
  X(Equal, false, FlagEffects::kSet, kDefault, 1, {1, 1})                     \
  X(NotEqual, false, FlagEffects::kSet, kDefault, 1, {1, 1})                  \
  X(GreaterThanSigned, false, FlagEffects::kSet, kDefault, 1, {1, 1})         \
  X(LessThanSigned, false, FlagEffects::kSet, kDefault, 1, {1, 1})            \
  X(GreaterThanEqualSigned, false, FlagEffects::kSet, kDefault, 1, {1, 1})    \
  X(LessThanEqualSigned, false, FlagEffects::kSet, kDefault, 1, {1, 1})       \
  X(GreaterThanUnsigned, false, FlagEffects::kSet, kDefault, 1, {1, 1})       \
  X(LessThanUnsigned, false, FlagEffects::kSet, kDefault, 1, {1, 1})          \
  X(GreaterThanEqualUnsigned, false, FlagEffects::kSet, kDefault, 1, {1, 1})  \
  X(LessThanEqualUnsigned, false, FlagEffects::kSet, kDefault, 1, {1, 1})     \
  X(Cmp, false, FlagEffects::kSet, kOut, 1, {1, 1})                           \
  X(Lea, false, FlagEffects::kNone, kAlways64, 1, {1, 1})                     \
  X(LoadArg, false, FlagEffects::kNone, kAlways64)                            \
  X(LoadSecondCallResult, false, FlagEffects::kNone, kDefault, 0, {}, 0)      \
  X(Exchange, false, FlagEffects::kNone, kAlways64, 1, {1, 1})                \
  X(Move, false, FlagEffects::kNone, kOut)                                    \
  X(Push, false, FlagEffects::kNone, kDefault, 1, {}, 1)                      \
  X(Pop, false, FlagEffects::kNone, kDefault, 0, {}, 1)                       \
  X(Cdq, false, FlagEffects::kNone, kDefault, 1, {}, 1)                       \
  X(Cwd, false, FlagEffects::kNone, kDefault, 1, {}, 1)                       \
  X(Cqo, false, FlagEffects::kNone, kDefault, 1, {}, 1)                       \
  X(Branch)                                                                   \
  X(BranchNZ)                                                                 \
  X(BranchZ)                                                                  \
  X(BranchA)                                                                  \
  X(BranchB)                                                                  \
  X(BranchAE)                                                                 \
  X(BranchBE)                                                                 \
  X(BranchG)                                                                  \
  X(BranchL)                                                                  \
  X(BranchGE)                                                                 \
  X(BranchLE)                                                                 \
  X(BranchC)                                                                  \
  X(BranchNC)                                                                 \
  X(BranchO)                                                                  \
  X(BranchNO)                                                                 \
  X(BranchS)                                                                  \
  X(BranchNS)                                                                 \
  X(BranchE)                                                                  \
  X(BranchNE)                                                                 \
  X(BitTest, false, FlagEffects::kSet, kDefault, 1, {1})                      \
  X(Inc, false, FlagEffects::kSet)                                            \
  X(Dec, false, FlagEffects::kSet)                                            \
  X(CondBranch, false, FlagEffects::kInvalidate, kDefault, 0, {1})            \
  X(Select, true, FlagEffects::kInvalidate, kDefault, 1, {1, 1, 1})           \
  X(Phi)                                                                      \
  X(Return, false, FlagEffects::kInvalidate)                                  \
  X(MovZX)                                                                    \
  X(MovSX)                                                                    \
  X(MovSXD)                                                                   \
  X(IntToBool, false, FlagEffects::kSet, kDefault, 1, {1})                    \
  X(YieldInitial, false, FlagEffects::kInvalidate, kDefault, 0, {}, 1)        \
  X(YieldFrom, false, FlagEffects::kInvalidate, kDefault, 0, {}, 1)           \
  X(YieldFromSkipInitialSend,                                                 \
    false,                                                                    \
    FlagEffects::kInvalidate,                                                 \
    kDefault,                                                                 \
    0,                                                                        \
    {},                                                                       \
    1)                                                                        \
  X(YieldFromHandleStopAsyncIteration,                                        \
    false,                                                                    \
    FlagEffects::kInvalidate,                                                 \
    kDefault,                                                                 \
    0,                                                                        \
    {},                                                                       \
    1)                                                                        \
  X(YieldValue, false, FlagEffects::kInvalidate, kDefault, 0, {}, 1)

// Instruction class defines instructions in LIR.
// Every instruction can have no more than one output, but arbitrary
// number of inputs. The instruction logically has no output also
// has an output data member with the type kNone.
class Instruction {
 public:
  // instruction type
  enum Opcode : int {
    kNone = -1,
#define INSTR_DECL_TYPE(v, ...) k##v,
    FOREACH_INSTR_TYPE(INSTR_DECL_TYPE)
#undef INSTR_DECL_TYPE
  };

#define DECL_OPCODE_TEST(v, ...) \
  bool is##v() const {           \
    return opcode() == k##v;     \
  }
  FOREACH_INSTR_TYPE(DECL_OPCODE_TEST)
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
  OperandBase* getInput(size_t i);
  const OperandBase* getInput(size_t i) const;

  Operand* allocateImmediateInput(
      uint64_t n,
      DataType data_type = DataType::k64bit);
  Operand* allocateFPImmediateInput(double n);
  LinkedOperand* allocateLinkedInput(Instruction* def_instr);
  Operand* allocatePhyRegisterInput(PhyLocation loc);
  Operand* allocateStackInput(PhyLocation stack);
  Operand* allocatePhyRegOrStackInput(PhyLocation loc);
  Operand* allocateAddressInput(void* address);
  Operand* allocateLabelInput(BasicBlock* block);

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
      allocateFPImmediateInput(first_arg.value)
          ->setDataType(OperandBase::kDouble);
    } else if constexpr (std::is_same_v<FT, MemImm>) {
      allocateAddressInput(first_arg.value);
    } else if constexpr (std::is_same_v<FT, Lbl>) {
      allocateLabelInput(first_arg.value);
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
      output()->setDataType(OperandBase::kDouble);
    } else if constexpr (std::is_same_v<FT, OutMemImm>) {
      output()->setMemoryAddress(first_arg.value);
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

  void setbasicblock(BasicBlock* bb);

  BasicBlock* basicblock();
  const BasicBlock* basicblock() const;

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
  void setInput(size_t index, std::unique_ptr<OperandBase> input);

  // Remove an input by index, shifting all other inputs to the left.
  std::unique_ptr<OperandBase> removeInput(size_t index);

  // Release the input operand at index from the instruction without
  // deallocating it.  The original input slot will be left with a nullptr,
  // which is meant be removed afterwards.
  std::unique_ptr<OperandBase> releaseInput(size_t index);

  // Add a new input to the end of this instruction's input list.
  OperandBase* appendInput(std::unique_ptr<OperandBase> operand);

  // Add a new input to the beginning of this instruction's input list.
  OperandBase* prependInput(std::unique_ptr<OperandBase> operand);

  // get the operand associated to a given predecessor in a phi instruction
  // returns nullptr if not found.
  OperandBase* getOperandByPredecessor(const BasicBlock* pred);

  int getOperandIndexByPredecessor(const BasicBlock* pred) const;

  const OperandBase* getOperandByPredecessor(const BasicBlock* pred) const;

  // Accessors for some of the instruction's attributes. See details in the
  // comment above FOREACH_INSTR_TYPE().
  bool getOutputPhyRegUse() const;
  bool getInputPhyRegUse(size_t i) const;
  bool inputsLiveAcross() const;

  bool isCompare() const;
  bool isBranchCC() const;
  bool isAnyBranch() const;
  bool isTerminator() const;
  bool isAnyYield() const;

  // negate the branch condition:
  // e.g. A >= B -> !(A < B)
  static Opcode negateBranchCC(Opcode opcode);

  // flipping the direction of comparison:
  // e.g. A >= B -> B <= A
  static Opcode flipBranchCCDirection(Opcode opcode);

  static Opcode flipComparisonDirection(Opcode opcode);

  static Opcode compareToBranchCC(Opcode opcode);

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
  std::vector<std::unique_ptr<OperandBase>> inputs_;
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

// This class defines instruction properties for different types of
// instructions.
class InstrProperty {
 public:
  struct InstrInfo;
  static InstrInfo& getProperties(Instruction::Opcode opcode);
  static InstrInfo& getProperties(const Instruction* instr) {
    return getProperties(instr->opcode());
  }

 private:
  static std::vector<InstrInfo> prop_map_;
};

// Initialize instruction properties
#define BEGIN_INSTR_PROPERTY_FIELD struct InstrProperty::InstrInfo {
#define END_INSTR_PROPERTY_FIELD \
  }                              \
  ;
#define FIELD_DEFAULT(__t, __n, __d) __t __n{__d};
#define FIELD_NO_DEFAULT(__t, __n) __t __n;

// clang-format off
// This table contains definitions of all the instruction property field.
BEGIN_INSTR_PROPERTY_FIELD
  FIELD_NO_DEFAULT(std::string_view, name)
  FIELD_DEFAULT(bool, inputs_live_across, false)
  FIELD_DEFAULT(FlagEffects, flag_effects, FlagEffects::kNone)
  FIELD_DEFAULT(OperandSizeType, opnd_size_type, kDefault)
  FIELD_DEFAULT(bool, output_phy_use, true)
  FIELD_DEFAULT(std::vector<int>, input_phy_uses, std::vector<int>{})
  FIELD_DEFAULT(bool, is_essential, false)
END_INSTR_PROPERTY_FIELD
// clang-format on

} // namespace lir
} // namespace jit

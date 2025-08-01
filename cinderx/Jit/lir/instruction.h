// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/util.h"
#include "cinderx/Jit/lir/operand.h"

#include <memory>
#include <unordered_map>
#include <unordered_set>
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
  X(BatchDecref, false, FlagEffects::kInvalidate, kDefault, 1, {1})           \
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

  static constexpr const char* kOpcodeNames[] = {
#define INSTR_DECL_TYPE(v, ...) #v,
      FOREACH_INSTR_TYPE(INSTR_DECL_TYPE)
#undef INSTR_DECL_TYPE
  };

#define COUNT_INSTR(...) +1
  static constexpr size_t kNumOpcodes = FOREACH_INSTR_TYPE(COUNT_INSTR);
#undef COUNT_INSTR

#define DECL_OPCODE_TEST(v, ...) \
  bool is##v() const {           \
    return opcode() == k##v;     \
  }
  FOREACH_INSTR_TYPE(DECL_OPCODE_TEST)
#undef DECL_OPCODE_TEST

  Instruction(BasicBlock* basic_block, Opcode opcode, const hir::Instr* origin);

  // Only copies simple fields (opcode_, basic_block_, origin_) from instr.
  // The output_ only has its simple fields copied.
  // The inputs are not copied.
  Instruction(BasicBlock* bb, Instruction* instr, const hir::Instr* origin);

  int id() const {
    return id_;
  }

  Operand* output() {
    return &output_;
  }
  const Operand* output() const {
    return &output_;
  }

  const hir::Instr* origin() const {
    return origin_;
  }

  size_t getNumInputs() const {
    return inputs_.size();
  }

  void setNumInputs(size_t n) {
    inputs_.resize(n);
  }

  size_t getNumOutputs() const {
    return output_.type() == OperandBase::kNone ? 0 : 1;
  }

  OperandBase* getInput(size_t i) {
    return inputs_.at(i).get();
  }

  const OperandBase* getInput(size_t i) const {
    return inputs_.at(i).get();
  }

  Operand* allocateImmediateInput(
      uint64_t n,
      OperandBase::DataType data_type = OperandBase::k64bit);
  Operand* allocateFPImmediateInput(double n);
  LinkedOperand* allocateLinkedInput(Instruction* def_instr);
  Operand* allocatePhyRegisterInput(PhyLocation loc) {
    return allocateOperand(&Operand::setPhyRegister, loc);
  }
  Operand* allocateStackInput(PhyLocation stack) {
    return allocateOperand(&Operand::setStackSlot, stack);
  }
  Operand* allocatePhyRegOrStackInput(PhyLocation loc) {
    return allocateOperand(&Operand::setPhyRegOrStackSlot, loc);
  }
  Operand* allocateAddressInput(void* address) {
    return allocateOperand(&Operand::setMemoryAddress, address);
  }
  Operand* allocateLabelInput(BasicBlock* block) {
    return allocateOperand(&Operand::setBasicBlock, block);
  }

  template <typename... Args>
  Operand* allocateMemoryIndirectInput(Args&&... args) {
    auto operand = std::make_unique<Operand>(this);
    auto opnd = operand.get();
    operand->setMemoryIndirect(std::forward<Args>(args)...);
    inputs_.push_back(std::move(operand));

    return opnd;
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

  void setbasicblock(BasicBlock* bb) {
    basic_block_ = bb;
  }

  BasicBlock* basicblock() {
    return basic_block_;
  }
  const BasicBlock* basicblock() const {
    return basic_block_;
  }

  Opcode opcode() const {
    return opcode_;
  }

  std::string opname() const {
    return kOpcodeNames[opcode_];
  }

  void setOpcode(Opcode opcode) {
    opcode_ = opcode;
  }

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

  // replace the input operand at index with operand.
  void replaceInputOperand(size_t index, std::unique_ptr<OperandBase> operand) {
    inputs_[index] = std::move(operand);
  }

  std::unique_ptr<OperandBase> removeInputOperand(size_t index) {
    auto opnd = std::move(inputs_.at(index));
    inputs_.erase(inputs_.begin() + index);
    return opnd;
  }

  // Release the input operand at index from the instruction without
  // deallocating it. The original index of inputs_ will be left with
  // a null std::unique_ptr, which is supposed be removed from inputs_
  // by an operation to follow.
  std::unique_ptr<OperandBase> releaseInputOperand(size_t index) {
    auto& operand = inputs_.at(index);
    operand->releaseFromInstr();
    return std::move(inputs_.at(index));
  }

  OperandBase* appendInputOperand(std::unique_ptr<OperandBase> operand) {
    auto opnd = operand.get();
    opnd->assignToInstr(this);
    inputs_.push_back(std::move(operand));
    return opnd;
  }

  OperandBase* prependInputOperand(std::unique_ptr<OperandBase> operand) {
    auto opnd = operand.get();
    opnd->assignToInstr(this);
    inputs_.insert(inputs_.begin(), std::move(operand));
    return opnd;
  }

  // get the operand associated to a given predecessor in a phi instruction
  // returns nullptr if not found.
  OperandBase* getOperandByPredecessor(const BasicBlock* pred) {
    auto index = getOperandIndexByPredecessor(pred);
    return index == -1 ? nullptr : inputs_.at(index).get();
  }

  int getOperandIndexByPredecessor(const BasicBlock* pred) const {
    JIT_DCHECK(opcode_ == kPhi, "The current instruction must be Phi.");
    size_t num_inputs = getNumInputs();
    for (size_t i = 0; i < num_inputs; i += 2) {
      if (getInput(i)->getBasicBlock() == pred) {
        return i + 1;
      }
    }
    return -1;
  }

  const OperandBase* getOperandByPredecessor(const BasicBlock* pred) const {
    return const_cast<Instruction*>(this)->getOperandByPredecessor(pred);
  }

  // Accessors for some of the instruction's attributes. See details in the
  // comment above FOREACH_INSTR_TYPE().
  bool getOutputPhyRegUse() const;
  bool getInputPhyRegUse(size_t i) const;
  bool inputsLiveAcross() const;

  bool isCompare() const {
    switch (opcode_) {
      case kEqual:
      case kNotEqual:
      case kGreaterThanSigned:
      case kLessThanSigned:
      case kGreaterThanEqualSigned:
      case kLessThanEqualSigned:
      case kGreaterThanUnsigned:
      case kLessThanUnsigned:
      case kGreaterThanEqualUnsigned:
      case kLessThanEqualUnsigned:
        return true;
      default:
        return false;
    }
  }

  bool isBranchCC() const {
    switch (opcode_) {
      case kBranchC:
      case kBranchNC:
      case kBranchO:
      case kBranchNO:
      case kBranchS:
      case kBranchNS:
      case kBranchZ:
      case kBranchNZ:
      case kBranchA:
      case kBranchB:
      case kBranchBE:
      case kBranchAE:
      case kBranchL:
      case kBranchG:
      case kBranchLE:
      case kBranchGE:
      case kBranchE:
      case kBranchNE:
        return true;
      default:
        return false;
    }
  }

  bool isAnyBranch() const {
    return (opcode_ == kCondBranch) || isBranchCC();
  }

  bool isTerminator() const {
    switch (opcode_) {
      case kReturn:
        return true;
      default:
        return false;
    }
  }

  bool isAnyYield() const {
    switch (opcode_) {
      case kYieldFrom:
      case kYieldFromHandleStopAsyncIteration:
      case kYieldFromSkipInitialSend:
      case kYieldInitial:
      case kYieldValue:
        return true;
      default:
        return false;
    }
  }

#define CASE_FLIP(op1, op2) \
  case op1:                 \
    return op2;             \
  case op2:                 \
    return op1;

  // negate the branch condition:
  // e.g. A >= B -> !(A < B)
  static Opcode negateBranchCC(Opcode opcode) {
    switch (opcode) {
      CASE_FLIP(kBranchC, kBranchNC)
      CASE_FLIP(kBranchO, kBranchNO)
      CASE_FLIP(kBranchS, kBranchNS)
      CASE_FLIP(kBranchZ, kBranchNZ)
      CASE_FLIP(kBranchA, kBranchBE)
      CASE_FLIP(kBranchB, kBranchAE)
      CASE_FLIP(kBranchL, kBranchGE)
      CASE_FLIP(kBranchG, kBranchLE)
      CASE_FLIP(kBranchE, kBranchNE)
      default:
        JIT_ABORT("Not a conditional branch opcode: {}", kOpcodeNames[opcode]);
    }
  }

  // flipping the direction of comparison:
  // e.g. A >= B -> B <= A
  static Opcode flipBranchCCDirection(Opcode opcode) {
    switch (opcode) {
      CASE_FLIP(kBranchA, kBranchB)
      CASE_FLIP(kBranchAE, kBranchBE)
      CASE_FLIP(kBranchL, kBranchG)
      CASE_FLIP(kBranchLE, kBranchGE)
      default:
        JIT_ABORT(
            "Unable to flip branch condition for opcode: {}",
            kOpcodeNames[opcode]);
    }
  }

  static Opcode flipComparisonDirection(Opcode opcode) {
    switch (opcode) {
      CASE_FLIP(kGreaterThanEqualSigned, kLessThanEqualSigned)
      CASE_FLIP(kGreaterThanEqualUnsigned, kLessThanEqualUnsigned)
      CASE_FLIP(kGreaterThanSigned, kLessThanSigned)
      CASE_FLIP(kGreaterThanUnsigned, kLessThanUnsigned)
      case kEqual:
        return kEqual;
      case kNotEqual:
        return kNotEqual;
      default:
        JIT_ABORT(
            "Unable to flip comparison direction for opcode: {}",
            kOpcodeNames[opcode]);
    }
  }

#undef CASE_FLIP

  static Opcode compareToBranchCC(Opcode opcode) {
    switch (opcode) {
      case kEqual:
        return kBranchE;
      case kNotEqual:
        return kBranchNE;
      case kGreaterThanUnsigned:
        return kBranchA;
      case kLessThanUnsigned:
        return kBranchB;
      case kGreaterThanEqualUnsigned:
        return kBranchAE;
      case kLessThanEqualUnsigned:
        return kBranchBE;
      case kGreaterThanSigned:
        return kBranchG;
      case kLessThanSigned:
        return kBranchL;
      case kGreaterThanEqualSigned:
        return kBranchGE;
      case kLessThanEqualSigned:
        return kBranchLE;
      default:
        JIT_ABORT("Not a compare opcode.");
    }
  }

 private:
  int id_;
  Opcode opcode_;
  Operand output_;
  std::vector<std::unique_ptr<OperandBase>> inputs_;

  BasicBlock* basic_block_;

  const hir::Instr* origin_;

  template <typename FType, typename... AType>
  Operand* allocateOperand(FType&& set_func, AType&&... arg) {
    auto operand = std::make_unique<Operand>(this);
    auto opnd = operand.get();
    (opnd->*set_func)(std::forward<AType>(arg)...);
    inputs_.push_back(std::move(operand));
    return opnd;
  }

  // used in parser, expect unique id
  void setId(int id) {
    id_ = id;
  }

  friend class Parser;
};

// Instruction Guard specific
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
  FIELD_NO_DEFAULT(std::string, name)
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

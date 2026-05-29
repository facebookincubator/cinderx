// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Jit/lir/arch.h"
#include "cinderx/Jit/lir/type.h"

#include <cstdint>
#include <memory>
#include <variant>

namespace jit::lir {

class BasicBlock;
class Instruction;
class Operand;
class MemoryIndirect;

// Memory reference: [base_reg + index_reg * (2^index_multiplier) + offset]
class MemoryIndirect {
 public:
  explicit MemoryIndirect(Instruction* parent);
  ~MemoryIndirect();

  void setMemoryIndirect(Instruction* base, int32_t offset);
  void setMemoryIndirect(PhyLocation base, int32_t offset = 0);

  void setMemoryIndirect(
      PhyLocation base,
      PhyLocation index_reg,
      uint8_t multiplier);

  void setMemoryIndirect(
      std::variant<Instruction*, PhyLocation> base,
      std::variant<Instruction*, PhyLocation> index,
      uint8_t multiplier,
      int32_t offset);

  Operand* getBaseRegOperand() const;
  Operand* getIndexRegOperand() const;

  uint8_t getMultiplier() const;
  int32_t getOffset() const;

 private:
  void setBaseIndex(
      std::unique_ptr<Operand>& base_index_opnd,
      Instruction* base_index);
  void setBaseIndex(
      std::unique_ptr<Operand>& base_index_opnd,
      PhyLocation base_index);

  void setBaseIndex(
      std::unique_ptr<Operand>& base_index_opnd,
      std::variant<Instruction*, PhyLocation> base_index);

  Instruction* parent_{nullptr};
  std::unique_ptr<Operand> base_reg_;
  std::unique_ptr<Operand> index_reg_;
  uint8_t multiplier_{0};
  int32_t offset_{0};
};

// An operand represents either:
// - A value being defined by an instruction (output operand or immediate)
// - A use of a value defined by another instruction (linked operand)
//
// When linked (isLinked() is true), getter methods delegate to the defining
// operand. Setter methods should only be called on non-linked operands.
class Operand {
 public:
  using Type = OperandType;

  struct LinkedTag {};
  static constexpr LinkedTag kLinked{};

  Operand() = default;
  explicit Operand(Instruction* parent);

  ~Operand() = default;

  // Copies type and data_type from another operand. Value is not copied.
  Operand(Instruction* parent, Operand* operand);

  Operand(Instruction* parent, DataType data_type, Type type, uint64_t data);
  Operand(Instruction* parent, Type type, double data);

  // Construct a linked operand referencing def's output.
  Operand(Instruction* def, LinkedTag);
  Operand(Instruction* parent, Instruction* def, LinkedTag);

#define OPERAND_TYPE_DEFINES(V, ...) \
  using OperandType::k##V;           \
                                     \
  bool is##V() const {               \
    return type() == Type::k##V;     \
  }
  FOREACH_OPERAND_TYPE(OPERAND_TYPE_DEFINES)
#undef OPERAND_TYPE_DEFINES

#define OPERAND_DATA_TYPE_DEFINES(V, ...) using DataType::k##V;
  FOREACH_OPERAND_DATA_TYPE(OPERAND_DATA_TYPE_DEFINES)
#undef OPERAND_DATA_TYPE_DEFINES

  size_t sizeInBits() const;

  // Get the instruction using this operand.
  Instruction* instr();
  const Instruction* instr() const;

  // Set and unset the instruction using this operand.
  void assignToInstr(Instruction* instr);
  void releaseFromInstr();

  bool isFp() const;
  bool isVecD() const;

  bool isLastUse() const;
  void setLastUse();

  uint64_t getConstant() const;
  void setConstant(uint64_t n, DataType data_type = k64bit);

  double getFPConstant() const;
  void setFPConstant(double n);

  PhyLocation getPhyRegister() const;
  void setPhyRegister(PhyLocation reg);

  PhyLocation getStackSlot() const;
  void setStackSlot(PhyLocation slot);

  PhyLocation getPhyRegOrStackSlot() const;
  void setPhyRegOrStackSlot(PhyLocation loc);

  void* getMemoryAddress() const;
  void setMemoryAddress(void* addr);

  MemoryIndirect* getMemoryIndirect() const;

  template <typename... Args>
  void setMemoryIndirect(Args&&... args) {
    type_ = kInd;
    auto ind = std::make_unique<MemoryIndirect>(instr());
    ind->setMemoryIndirect(std::forward<Args>(args)...);
    value_ = std::move(ind);
  }

  BasicBlock* getBasicBlock() const;
  void setBasicBlock(BasicBlock* block);

  asmjit::Label getAsmLabel() const;
  void setAsmLabel(const asmjit::Label& label);
  bool hasAsmLabel() const;

  // Get the value of an integer constant, or the integral cast of a fixed
  // memory address.
  uint64_t getConstantOrAddress() const;

  // Get the canonical operand that defines this value. Returns this for
  // non-linked operands, or the linked defining operand otherwise.
  const Operand* getDefine() const;
  Operand* getDefine();

  DataType dataType() const;
  void setDataType(DataType data_type);

  Type type() const;
  void setNone();
  void setVirtualRegister();

  bool isLinked() const;

  // Linked operand accessors. Only valid when isLinked() is true.
  Operand* getLinkedOperand();
  const Operand* getLinkedOperand() const;
  Instruction* getLinkedInstr();
  const Instruction* getLinkedInstr() const;
  void setLinkedInstr(Instruction* def);

 private:
  uint64_t rawValue() const;
  Operand* linkedDef();
  const Operand* linkedDef() const;

  Instruction* parent_instr_{nullptr};
  bool last_use_{false};
  Type type_{kNone};
  DataType data_type_{kObject};

  std::variant<
      uint64_t,
      void*,
      BasicBlock*,
      asmjit::Label,
      std::unique_ptr<MemoryIndirect>,
      PhyLocation,
      Operand*>
      value_;
};

// OperandArg reqresents different operand data types, and is used as
// arguments to BasicBlock::allocateInstr* instructions. The latter
// will create the operands accordingly for the instructions after
// allocating them.
template <typename Type, bool Output>
struct OperandArg {
  explicit OperandArg(Type v, DataType dt = Operand::kObject)
      : value(v), data_type(dt) {}

  Type value;
  DataType data_type{Operand::kObject};
  static constexpr bool is_output = Output;
};

template <bool Output>
struct OperandArg<uint64_t, Output> {
  explicit OperandArg(uint64_t v, DataType dt = Operand::k64bit)
      : value(v), data_type(dt) {}

  uint64_t value;
  DataType data_type{Operand::k64bit};
  static constexpr bool is_output = Output;
};

// Operand is typed through its linked instruction.
template <>
struct OperandArg<Instruction*, false> {
  explicit OperandArg(Instruction* v) : value{v} {}

  Instruction* value{nullptr};
  static constexpr bool is_output = false;
};

template <bool Output>
struct OperandArg<MemoryIndirect, Output> {
  using Reg = std::variant<Instruction*, PhyLocation>;

  explicit OperandArg(Reg b, DataType dt = Operand::kObject)
      : base(b), data_type(dt) {}
  explicit OperandArg(Reg b, int32_t off, DataType dt = Operand::kObject)
      : base(b), offset(off), data_type(dt) {}
  OperandArg(Reg b, Reg i, DataType dt = Operand::kObject)
      : base(b), index(i), data_type(dt) {}
  OperandArg(Reg b, Reg i, int32_t off, DataType dt = Operand::kObject)
      : base(b), index(i), offset(off), data_type(dt) {}
  OperandArg(
      Reg b,
      Reg i,
      unsigned int num_bytes,
      int32_t off,
      DataType dt = Operand::kObject)
      : base(b), index(i), offset(off), data_type(dt) {
    // x86 encodes scales as size==2**X, so this does log2(num_bytes), but we
    // have a limited set of inputs.
    switch (num_bytes) {
      case 1:
        multiplier = 0;
        break;
      case 2:
        multiplier = 1;
        break;
      case 4:
        multiplier = 2;
        break;
      case 8:
        multiplier = 3;
        break;
      default:
        JIT_ABORT("Unexpected num_bytes {}", num_bytes);
    }
  }

  Reg base{PhyLocation::REG_INVALID};
  Reg index{PhyLocation::REG_INVALID};
  uint8_t multiplier{0};
  int32_t offset{0};
  DataType data_type{Operand::kObject};
  static constexpr bool is_output = Output;
};

template <>
struct OperandArg<void, true> {
  OperandArg(const DataType& dt = Operand::kObject) : data_type(dt) {}

  DataType data_type{Operand::kObject};
  static constexpr bool is_output = true;
};

// Creates a new struct type so that types like Stk and PhyReg are different
// from each other. This is needed because we need std::is_same_v<Stk, PhyReg> =
// false. If we used `using` then they would be aliases of each other.
#define DECLARE_TYPE_ARG(__T, __V, __O)      \
  struct __T : public OperandArg<__V, __O> { \
    using OperandArg::OperandArg;            \
  };

DECLARE_TYPE_ARG(PhyReg, PhyLocation, false)
DECLARE_TYPE_ARG(Imm, uint64_t, false)
DECLARE_TYPE_ARG(FPImm, double, false)
DECLARE_TYPE_ARG(MemImm, void*, false)
DECLARE_TYPE_ARG(Stk, PhyLocation, false)
DECLARE_TYPE_ARG(Lbl, BasicBlock*, false)
DECLARE_TYPE_ARG(VReg, Instruction*, false)
DECLARE_TYPE_ARG(Ind, MemoryIndirect, false)

// AsmLbl wraps an asmjit::Label directly (as opposed to Lbl which wraps a
// BasicBlock* that is later resolved to a label via block_label_map).
struct AsmLbl {
  explicit AsmLbl(asmjit::Label& l) : value(l) {}
  asmjit::Label value;
  static constexpr bool is_output = false;
};

DECLARE_TYPE_ARG(OutPhyReg, PhyLocation, true)
DECLARE_TYPE_ARG(OutImm, uint64_t, true)
DECLARE_TYPE_ARG(OutFPImm, double, true)
DECLARE_TYPE_ARG(OutMemImm, void*, true)
DECLARE_TYPE_ARG(OutStk, PhyLocation, true)
DECLARE_TYPE_ARG(OutLbl, BasicBlock*, true)
DECLARE_TYPE_ARG(OutDbl, double, true)
DECLARE_TYPE_ARG(OutInd, MemoryIndirect, true)
DECLARE_TYPE_ARG(OutVReg, void, true)

} // namespace jit::lir

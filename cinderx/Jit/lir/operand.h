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
class OperandBase;
class Operand;
class LinkedOperand;
class MemoryIndirect;

// Defines the interface for all the operand kinds.
class OperandBase {
 public:
  using Type = OperandType;

  OperandBase() = default;
  explicit OperandBase(Instruction* parent);

  virtual ~OperandBase() = default;

  OperandBase(const OperandBase& ob);

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

  virtual uint64_t getConstant() const = 0;
  virtual double getFPConstant() const = 0;
  virtual PhyLocation getPhyRegister() const = 0;
  virtual PhyLocation getStackSlot() const = 0;
  virtual PhyLocation getPhyRegOrStackSlot() const = 0;
  virtual void* getMemoryAddress() const = 0;
  virtual MemoryIndirect* getMemoryIndirect() const = 0;
  virtual BasicBlock* getBasicBlock() const = 0;

  // Get the value of an integer constant, or the integral cast of a fixed
  // memory address.
  virtual uint64_t getConstantOrAddress() const = 0;

  // Get the canonical operand that defines this operand.  For Operand, that is
  // itself.  For LinkedOperand, it's the linked operand.
  virtual Operand* getDefine() = 0;
  virtual const Operand* getDefine() const = 0;

  virtual DataType dataType() const = 0;
  virtual Type type() const = 0;

  virtual bool isLinked() const = 0;

 private:
  Instruction* parent_instr_{nullptr};
  bool last_use_{false};
};

// Memory reference: [base_reg + index_reg * (2^index_multiplier) + offset]
class MemoryIndirect {
 public:
  explicit MemoryIndirect(Instruction* parent);

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

  OperandBase* getBaseRegOperand() const;
  OperandBase* getIndexRegOperand() const;

  uint8_t getMultipiler() const;
  int32_t getOffset() const;

 private:
  void setBaseIndex(
      std::unique_ptr<OperandBase>& base_index_opnd,
      Instruction* base_index);
  void setBaseIndex(
      std::unique_ptr<OperandBase>& base_index_opnd,
      PhyLocation base_index);

  void setBaseIndex(
      std::unique_ptr<OperandBase>& base_index_opnd,
      std::variant<Instruction*, PhyLocation> base_index);

  Instruction* parent_{nullptr};
  std::unique_ptr<OperandBase> base_reg_;
  std::unique_ptr<OperandBase> index_reg_;
  uint8_t multiplier_{0};
  int32_t offset_{0};
};

// An operand that is either an immediate value, or a value being defined by an
// instruction.
class Operand : public OperandBase {
 public:
  Operand() = default;
  explicit Operand(Instruction* parent);

  ~Operand() override = default;

  // Only copies simple fields (type and data type) from operand.
  // The value_ field is not copied.
  Operand(Instruction* parent, Operand* operand);

  Operand(Instruction* parent, DataType data_type, Type type, uint64_t data);
  Operand(Instruction* parent, Type type, double data);

  uint64_t getConstant() const override;
  void setConstant(uint64_t n, DataType data_type = k64bit);

  double getFPConstant() const override;
  void setFPConstant(double n);

  PhyLocation getPhyRegister() const override;
  void setPhyRegister(PhyLocation reg);

  PhyLocation getStackSlot() const override;
  void setStackSlot(PhyLocation slot);

  PhyLocation getPhyRegOrStackSlot() const override;
  void setPhyRegOrStackSlot(PhyLocation loc);

  void* getMemoryAddress() const override;
  void setMemoryAddress(void* addr);

  MemoryIndirect* getMemoryIndirect() const override;

  template <typename... Args>
  void setMemoryIndirect(Args&&... args) {
    type_ = kInd;
    auto ind = std::make_unique<MemoryIndirect>(instr());
    ind->setMemoryIndirect(std::forward<Args>(args)...);
    value_ = std::move(ind);
  }

  BasicBlock* getBasicBlock() const override;
  void setBasicBlock(BasicBlock* block);

  uint64_t getConstantOrAddress() const override;

  const Operand* getDefine() const override;
  Operand* getDefine() override;

  DataType dataType() const override;
  void setDataType(DataType data_type);

  Type type() const override;
  void setNone();
  void setVirtualRegister();

  bool isLinked() const override;

 private:
  uint64_t rawValue() const;

  Type type_{kNone};
  DataType data_type_{kObject};

  std::variant<
      uint64_t,
      void*,
      BasicBlock*,
      std::unique_ptr<MemoryIndirect>,
      PhyLocation>
      value_;
};

// An operand that points to the output value of an instruction.  Represents a
// def-use relationship.
//
// Can only be the input of an instruction.
class LinkedOperand : public OperandBase {
 public:
  explicit LinkedOperand(Instruction* def);
  LinkedOperand(Instruction* parent, Instruction* def);

  ~LinkedOperand() override = default;

  Operand* getLinkedOperand();
  const Operand* getLinkedOperand() const;

  Instruction* getLinkedInstr();
  const Instruction* getLinkedInstr() const;

  void setLinkedInstr(Instruction* def);

  uint64_t getConstant() const override;
  double getFPConstant() const override;
  PhyLocation getPhyRegister() const override;
  PhyLocation getStackSlot() const override;
  PhyLocation getPhyRegOrStackSlot() const override;
  void* getMemoryAddress() const override;
  MemoryIndirect* getMemoryIndirect() const override;
  BasicBlock* getBasicBlock() const override;
  uint64_t getConstantOrAddress() const override;
  Operand* getDefine() override;
  const Operand* getDefine() const override;
  DataType dataType() const override;
  Type type() const override;
  bool isLinked() const override;

 private:
  friend class Operand;

  Operand* def_opnd_{nullptr};
};

// OperandArg reqresents different operand data types, and is used as
// arguments to BasicBlock::allocateInstr* instructions. The latter
// will create the operands accordingly for the instructions after
// allocating them.
template <typename Type, bool Output>
struct OperandArg {
  explicit OperandArg(Type v, DataType dt = OperandBase::kObject)
      : value(v), data_type(dt) {}

  Type value;
  DataType data_type{OperandBase::kObject};
  static constexpr bool is_output = Output;
};

template <bool Output>
struct OperandArg<uint64_t, Output> {
  explicit OperandArg(uint64_t v, DataType dt = OperandBase::k64bit)
      : value(v), data_type(dt) {}

  uint64_t value;
  DataType data_type{OperandBase::k64bit};
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

  explicit OperandArg(Reg b, DataType dt = OperandBase::kObject)
      : base(b), data_type(dt) {}
  explicit OperandArg(Reg b, int32_t off, DataType dt = OperandBase::kObject)
      : base(b), offset(off), data_type(dt) {}
  OperandArg(Reg b, Reg i, DataType dt = OperandBase::kObject)
      : base(b), index(i), data_type(dt) {}
  OperandArg(Reg b, Reg i, int32_t off, DataType dt = OperandBase::kObject)
      : base(b), index(i), offset(off), data_type(dt) {}
  OperandArg(
      Reg b,
      Reg i,
      unsigned int num_bytes,
      int32_t off,
      DataType dt = OperandBase::kObject)
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
  DataType data_type{OperandBase::kObject};
  static constexpr bool is_output = Output;
};

template <>
struct OperandArg<void, true> {
  OperandArg(const DataType& dt = OperandBase::kObject) : data_type(dt) {}

  DataType data_type{OperandBase::kObject};
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

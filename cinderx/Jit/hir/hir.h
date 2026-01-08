// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/bytecode_offsets.h"
#include "cinderx/Jit/code_patcher.h"
#include "cinderx/Jit/hir/frame_state.h"
#include "cinderx/Jit/hir/opcode.h"
#include "cinderx/Jit/hir/register.h"
#include "cinderx/Jit/hir/type.h"
#include "cinderx/Jit/intrusive_list.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace jit::hir {

/*
 * This file defines the high-level intermediate representation (HIR) used by
 * the JIT.
 *
 * The main goals for the IR are:
 * 1. Stay close to Python. The HIR is machine independent and tries to stay
 *    close to Python in order to enable optimizations that are easier to
 *    perform at a higher level of abstraction. For example, null checks for
 *    variable accesses are represented explicitly so that they may be
 *    optimized away when it can be statically determined that a variable is
 *    defined.
 * 2. Be as explicit as possible. The CPython bytecode has a large amount of
 *    implicit logic (e.g. refcounting, null checks). Making that logic
 *    explicit in the IR makes it possible to optimize away.
 * 3. Be easy to lower into a lower-level IR for code generation. It should be
 *    possible to lower the HIR into C or LLVM IR mechanically.
 *
 * Functions are converted into HIR by performing an abstract interpretation
 * over the function's bytecode.
 *
 * Functions are represented as a control flow graph of basic blocks. Each
 * basic block contains a list of instructions that ends in a
 * terminator. Instructions operate on an arbitrary set of variables and are
 * not in SSA form.
 */

class BasicBlock;

// Every control flow instruction has one or more Edges. BasicBlocks that
// contain or are targets of these instructions hold pointers to their Edges in
// sets of in- and out-edges.
class Edge {
 public:
  Edge() = default;
  Edge(const Edge& other);
  ~Edge();

  Edge& operator=(const Edge&) = delete;

  BasicBlock* from() const;
  BasicBlock* to() const;

  void set_from(BasicBlock* from);
  void set_to(BasicBlock* to);

 private:
  BasicBlock* from_{nullptr};
  BasicBlock* to_{nullptr};
};

// Used to represent that a type must be a subclass of one of the types
// specified in the constraint. This is done to prevent accepting a register
// that's typed as the union of the types in the Constraint
enum class Constraint {
  kType,
  kMatchAllAsCInt,
  kMatchAllAsPrimitive,
  kTupleExactOrCPtr,
  kListOrChkList,
  kDictOrChkDict,
  kOptObjectOrCInt,
  kOptObjectOrCIntOrCBool,
};

struct OperandType {
  /* implicit */ OperandType(Type ty) : kind{Constraint::kType}, type{ty} {}
  /* implicit */ OperandType(Constraint c) : kind{c}, type{TBottom} {}

  Constraint kind;
  Type type;
};

std::ostream& operator<<(std::ostream& os, OperandType kind);

template <typename... Args>
inline std::vector<OperandType> makeTypeVec(Args&&... args) {
  return {args...};
}

class DeoptBase;

// Base class that all concrete HIR instructions must derive from.
//
// Instructions have variable sized instances; the operands are stored
// **before** the instruction. The memory layout for an instruction looks
// like:
//
//   +--------------+
//   | Operand 0    |
//   | Operand 1    |
//   | ...          |
//   | Num operands |
//   | Vtable ptr   | <--- Where `this` points
//   | ....         |
//   +--------------+
//
// Given that instructions have variable sized instances, they must be
// allocated using the `create` methods that are defined on concrete
// subclasses. Attempting to heap allocate instructions should result
// in a compiler error, however, automatic allocation will still compile.
// Don't do that.
class Instr {
  // Instructions are part of a doubly linked list in the basic block they
  // belong to.
  IntrusiveListNode block_node_;

 public:
  using List = IntrusiveList<Instr, &Instr::block_node_>;

  static constexpr bool has_output = false;

  static void operator delete(void* ptr);

  virtual ~Instr() = default;

  // This defines a predicate per opcode that can be used to determine
  // if an instance of an instruction is a particular subclass
  // (e.g. `instr->IsBranch()`)
#define DEFINE_OP_PREDICATE(opname)       \
  bool Is##opname() const {               \
    return opcode() == Opcode::k##opname; \
  }
  FOREACH_OPCODE(DEFINE_OP_PREDICATE)
#undef DEFINE_OP_PREDICATE

  constexpr Opcode opcode() const {
    return opcode_;
  }

  // Get the name of the instruction's HIR opcode.
  std::string_view opname() const;

  // Return the number of operands that the instruction takes
  std::size_t NumOperands() const;

  // Return the i-th operand
  Register* GetOperand(std::size_t i) const;

  // Update the i-th operand
  void SetOperand(std::size_t i, Register* reg);

  // Get all operands for this instruction.
  std::span<Register* const> GetOperands() const;

  // Return the i-th operand type
  virtual OperandType GetOperandType(std::size_t /* i */) const = 0;

  // Visit all Registers used by the instruction, whether they're normal
  // operands or other data. Iteration can be stopped early by returning false
  // from the callback.
  virtual bool visitUses(const std::function<bool(Register*&)>& func);

  // Visit all Registers used by the instruction, without allowing mutation of
  // the uses.
  bool visitUses(const std::function<bool(Register*)>& func) const;

  // Return whether or not the instruction uses the supplied register as an
  // input
  bool Uses(Register* needle) const;

  // Replace uses of orig with replacement.
  void ReplaceUsesOf(Register* orig, Register* replacement);

  // If this instruction produces a value, return where it will be stored
  Register* output() const;

  // Set where the output from this instruction will be stored
  void setOutput(Register* dst);

  // Basic blocks must be terminated with control flow ops
  bool IsTerminator() const;

  // If this is a control instruction, return the number of outgoing edges
  std::size_t numEdges() const;

  // If this is a control instruction, return the i-th edge
  Edge* edge(std::size_t i);
  const Edge* edge(std::size_t i) const;

  // Get a list of all outgoing edges from this instruction.
  virtual std::span<const Edge> edges() const;

  virtual Instr* clone() const = 0;

  // Get or set the i-th successor.
  BasicBlock* successor(std::size_t i) const;
  void set_successor(std::size_t i, BasicBlock* to);

  void InsertBefore(Instr& instr);
  void InsertAfter(Instr& instr);

  // Unlink this Instr from its block.
  void unlink();

  // Get the basic block that this instruction is part of.
  BasicBlock* block() const;

  void ReplaceWith(Instr& instr);
  void ExpandInto(const std::vector<Instr*>& expansion);

  // Returns the `FrameState` that dominates this instruction, if one exists
  // and there are no non-replayable instructions between it and the
  // instruction.
  const FrameState* getDominatingFrameState() const;

  // Returns whether or not this instruction can be safely re-executed.
  bool isReplayable() const;

  // Set/get the bytecode offset that this instruction is associated with
  BCOffset bytecodeOffset() const;
  void setBytecodeOffset(BCOffset off);

  // Inherit the same bytecode offset as another instruction.
  void copyBytecodeOffset(const Instr& instr);

  // Downcast the Instr to a DeoptBase, returning nullptr if it isn't one.
  virtual DeoptBase* asDeoptBase();
  virtual const DeoptBase* asDeoptBase() const;

 protected:
  // Allocate a block of memory suitable to house an `Instr`. This function is
  // intended to be used by the various `create` functions that are defined on
  // concrete `Instr` subclasses.
  static void* allocate(std::size_t fixed_size, std::size_t num_operands);

  explicit Instr(Opcode opcode);
  Instr(const Instr& other);

  Instr& operator=(const Instr&) = delete;

  void* operator new(std::size_t count, void* ptr);

  // Get the base allocated address of this structure.  This is a smaller
  // address than `this` because of the variable length array of operands that's
  // allocated inline with the Instr object.
  void* base();

  Register** operands();
  Register* const* operands() const;

  Register*& operandAt(std::size_t i);

  friend class BasicBlock;

  // Link this Instr into its block. Meant to be called after inserting it into
  // the appropriate position in the block.
  void link(BasicBlock* block);

  // Set this Instr's block, updating any edges as appropriate.
  void set_block(BasicBlock* block);

 protected:
  Opcode opcode_;
  BCOffset bytecode_offset_{-1};
  Register* output_{nullptr};
  BasicBlock* block_{nullptr};
};

using InstrPredicate = std::function<bool(const Instr&)>;

// Subclass of Instr that is able to deopt back to the interpreter.
class DeoptBase : public Instr {
 public:
  explicit DeoptBase(Opcode op);
  DeoptBase(Opcode op, const FrameState& frame);
  DeoptBase(const DeoptBase& other);

  template <typename... Args>
  void emplaceLiveReg(Args&&... args) {
    live_regs_.emplace_back(std::forward<Args>(args)...);
  }

  const std::vector<RegState>& live_regs() const;
  std::vector<RegState>& live_regs();

  void sortLiveRegs();

  // Set/get the metadata needed to reconstruct the state of the interpreter
  // after this instruction executes.
  void setFrameState(std::unique_ptr<FrameState> state);
  void setFrameState(const FrameState& state);
  FrameState* frameState() const;
  std::unique_ptr<FrameState> takeFrameState();

  bool visitUses(const std::function<bool(Register*&)>& func) override;

  DeoptBase* asDeoptBase() override;
  const DeoptBase* asDeoptBase() const override;

  int nonce() const;
  void set_nonce(int nonce);

  // Get or set the human-readable description of why this instruction might
  // deopt.
  const std::string& descr() const;
  void setDescr(std::string r);

  // Get or set the optional value that is responsible for this deopt
  // event. Its exact meaning depends on the opcode of this instruction.
  Register* guiltyReg() const;
  void setGuiltyReg(Register* reg);

 private:
  std::vector<RegState> live_regs_;
  std::unique_ptr<FrameState> frame_state_{nullptr};
  // If set and this instruction deopts at runtime, this value is made
  // conveniently available in the deopt machinery.
  Register* guilty_reg_{nullptr};
  int nonce_{-1};
  // A human-readable description of why this instruction might deopt.
  std::string descr_;
};

// This pile of template metaprogramming provides a convenient way to define
// concrete subclasses of `Instr`. It allows users to
//
// - Specify whether or not the instruction has an output via the `HasOutput`
//   tag type.
// - Specify the number of operands via the `Operands` tag type. Variadic
//   instructions are defined using `Operands<>`.
// - Specify an optional different base class. If given, it must derive from
//   `Instr` and appear as the last template argument. It's constructor must
//   accept an `Opcode` as the first argument.
template <class T, Opcode opcode, typename... Tys>
class InstrT;

// Base classes.
template <class T, Opcode opc, class Base, typename... Tys>
class InstrT<T, opc, Base, Tys...> : public Base {
 public:
  OperandType GetOperandType(std::size_t i) const override {
    JIT_DCHECK(
        i < this->NumOperands(),
        "operand {} out of range (max is {})",
        i,
        this->NumOperands() - 1);
    return static_cast<const T*>(this)->GetOperandTypeImpl(i);
  }

  static_assert(
      std::is_base_of<Instr, Base>::value,
      "base type must derive from Instr");
  static_assert(
      sizeof...(Tys) == 0,
      "base type must appear as last template parameter");

  InstrT(const InstrT& other) : Base(other) {
    for (size_t i = 0; i < other.NumOperands(); i++) {
      this->SetOperand(i, other.GetOperand(i));
    }
  }

  Instr* clone() const override {
    auto ptr = Instr::allocate(sizeof(T), this->NumOperands());
    return new (ptr) T(*static_cast<const T*>(this));
  }

  template <typename... Args>
  InstrT(Args&&... args) : Base(opc, std::forward<Args>(args)...) {}
};

template <class T, Opcode opc>
class InstrT<T, opc> : public InstrT<T, opc, Instr> {
 public:
  using InstrT<T, opc, Instr>::InstrT;
};

// Support for specifying the number of operands expected by the instruction.
//
// Caveats:
//
// - Custom constructors must be public in order to be accessible by the
//   `create` methods defined below.
// - Constructors are provided for common arities that expect operands to be
//   provided and handle setting them on the instruction.
constexpr int kVariadic = -1;

template <int n = kVariadic>
struct Operands;

template <class T, Opcode opcode, int arity, typename... Tys>
class InstrT<T, opcode, Operands<arity>, Tys...>
    : public InstrT<T, opcode, Tys...> {
 public:
  // Define a `create` method for non-variadic `T`.
  //
  // Usage:
  //   auto instr = T::create(<args for T's constructor>);
  template <typename... Args, class T1 = T>
    requires(arity >= 0)
  static T1* create(Args&&... args) {
    auto ptr = Instr::allocate(sizeof(T1), arity);
    return new (ptr) T1(std::forward<Args>(args)...);
  }

  // Define a `create` method for variadic `T`.
  //
  // Usage:
  //   auto instr = T::create(<num_operands>, <args for T's constructor>);
  template <typename... Args, class T1 = T>
    requires(arity == kVariadic)
  static T1* create(std::size_t num_ops, Args&&... args) {
    auto ptr = Instr::allocate(sizeof(T1), num_ops);
    return new (ptr) T1(std::forward<Args>(args)...);
  }

  // Forwarding constructor for variadic `T`.
  template <typename... Args, int a = arity>
    requires(a <= 0)
  explicit InstrT(Args&&... args)
      : InstrT<T, opcode, Tys...>(std::forward<Args>(args)...) {}

  // Constructor for unary `T`.
  template <typename... Args, int a = arity>
    requires(a == 1)
  explicit InstrT(Register* reg, Args&&... args)
      : InstrT<T, opcode, Tys...>(std::forward<Args>(args)...) {
    this->operandAt(0) = reg;
  }

  template <int a = arity>
    requires(a == 1)
  Register* reg() const {
    return this->GetOperand(0);
  }

  // Constructor for binary `T`.
  template <typename... Args, int a = arity>
    requires(a == 2)
  InstrT(Register* lhs, Register* rhs, Args&&... args)
      : InstrT<T, opcode, Tys...>(std::forward<Args>(args)...) {
    this->operandAt(0) = lhs;
    this->operandAt(1) = rhs;
  }

  // Constructor for trinary `T`.
  template <typename... Args, int x = arity>
    requires(x == 3)
  InstrT(Register* a, Register* b, Register* c, Args&&... args)
      : InstrT<T, opcode, Tys...>(std::forward<Args>(args)...) {
    this->operandAt(0) = a;
    this->operandAt(1) = b;
    this->operandAt(2) = c;
  }

  // Constructor for 4 operand `T`.
  template <typename... Args, int x = arity>
    requires(x == 4)
  InstrT(Register* a, Register* b, Register* c, Register* d, Args&&... args)
      : InstrT<T, opcode, Tys...>(std::forward<Args>(args)...) {
    this->operandAt(0) = a;
    this->operandAt(1) = b;
    this->operandAt(2) = c;
    this->operandAt(3) = d;
  }
};

// Support for setting the output
struct HasOutput;

template <class T, Opcode opcode, typename... Tys>
class InstrT<T, opcode, HasOutput, Tys...> : public InstrT<T, opcode, Tys...> {
 public:
  static constexpr bool has_output = true;

  template <typename... Args>
  explicit InstrT(Register* dst, Args&&... args)
      : InstrT<T, opcode, Tys...>(std::forward<Args>(args)...) {
    this->setOutput(dst);
  }
};

// TASK(T105350013): Add a compile-time op_types size check
#define INSTR_CLASS(name, types, ...)                                          \
  name##                                                                       \
  _OperandTypes{public : OperandType GetOperandTypeImpl(std::size_t i) const { \
      static const std::vector<OperandType> op_types = makeTypeVec types;      \
  std::size_t num_ops = op_types.size();                                       \
  if (i >= num_ops) {                                                          \
    return op_types[num_ops - 1];                                              \
  } else {                                                                     \
    return op_types[i];                                                        \
  }                                                                            \
  }                                                                            \
  }                                                                            \
  ;                                                                            \
  class name final : public InstrT<name, Opcode::k##name, __VA_ARGS__>,        \
                     public name##_OperandTypes

#define DEFINE_SIMPLE_INSTR(name, types, ...)   \
  class INSTR_CLASS(name, types, __VA_ARGS__) { \
   private:                                     \
    friend InstrT;                              \
    using InstrT::InstrT;                       \
  }

#define FOREACH_BINARY_OP_KIND(V) \
  V(Add)                          \
  V(And)                          \
  V(FloorDivide)                  \
  V(LShift)                       \
  V(MatrixMultiply)               \
  V(Modulo)                       \
  V(Multiply)                     \
  V(Or)                           \
  V(Power)                        \
  V(RShift)                       \
  V(Subscript)                    \
  V(Subtract)                     \
  V(TrueDivide)                   \
  V(Xor)                          \
  V(FloorDivideUnsigned)          \
  V(ModuloUnsigned)               \
  V(RShiftUnsigned)               \
  V(PowerUnsigned)

enum class BinaryOpKind {
#define DEFINE_OP(NAME) k##NAME,
  FOREACH_BINARY_OP_KIND(DEFINE_OP)
#undef DEFINE_OP
};

#define COUNT_OP(NAME) +1
constexpr size_t kNumBinaryOpKinds = FOREACH_BINARY_OP_KIND(COUNT_OP);
#undef COUNT_OP

std::string_view GetBinaryOpName(BinaryOpKind op);
BinaryOpKind ParseBinaryOpName(std::string_view name);

// Perform a binary operation (e.g. '+', '-')
class INSTR_CLASS(
    BinaryOp,
    (TObject, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  BinaryOp(
      Register* dst,
      BinaryOpKind op,
      Register* left,
      Register* right,
      const FrameState& frame)
      : InstrT(dst, left, right, frame), op_(op) {}

  BinaryOpKind op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  BinaryOpKind op_;
};

#define FOREACH_UNARY_OP_KIND(V) \
  V(Not)                         \
  V(Negate)                      \
  V(Positive)                    \
  V(Invert)

enum class UnaryOpKind {
#define DEFINE_OP(NAME) k##NAME,
  FOREACH_UNARY_OP_KIND(DEFINE_OP)
#undef DEFINE_OP
};

#define COUNT_OP(NAME) +1
constexpr size_t kNumUnaryOpKinds = FOREACH_UNARY_OP_KIND(COUNT_OP);
#undef COUNT_OP

std::string_view GetUnaryOpName(UnaryOpKind op);
UnaryOpKind ParseUnaryOpName(std::string_view name);

// Perform a unary operator (-x, ~x, etc...)
class INSTR_CLASS(UnaryOp, (TObject), HasOutput, Operands<1>, DeoptBase) {
 public:
  UnaryOp(
      Register* dst,
      UnaryOpKind op,
      Register* operand,
      const FrameState& frame)
      : InstrT(dst, operand, frame), op_(op) {}

  UnaryOpKind op() const {
    return op_;
  }

  Register* operand() const {
    return GetOperand(0);
  }

 private:
  UnaryOpKind op_;
};

#define FOREACH_INPLACE_OP_KIND(V) \
  V(Add)                           \
  V(And)                           \
  V(FloorDivide)                   \
  V(LShift)                        \
  V(MatrixMultiply)                \
  V(Modulo)                        \
  V(Multiply)                      \
  V(Or)                            \
  V(Power)                         \
  V(RShift)                        \
  V(Subtract)                      \
  V(TrueDivide)                    \
  V(Xor)

enum class InPlaceOpKind {
#define DEFINE_OP(NAME) k##NAME,
  FOREACH_INPLACE_OP_KIND(DEFINE_OP)
#undef DEFINE_OP
};

#define COUNT_OP(NAME) +1
constexpr size_t kNumInPlaceOpKinds = FOREACH_INPLACE_OP_KIND(COUNT_OP);
#undef COUNT_OP

std::string_view GetInPlaceOpName(InPlaceOpKind op);
InPlaceOpKind ParseInPlaceOpName(std::string_view name);

// Perform a in place operator x += 2
class INSTR_CLASS(
    InPlaceOp,
    (TObject, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  InPlaceOp(
      Register* dst,
      InPlaceOpKind op,
      Register* left,
      Register* right,
      const FrameState& frame)
      : InstrT(dst, left, right, frame), op_(op) {}

  InPlaceOpKind op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  InPlaceOpKind op_;
};

// Builds a slice object, with 2 or 3 operands from the stack
class INSTR_CLASS(BuildSlice, (TObject), HasOutput, Operands<>, DeoptBase) {
 public:
  using InstrT::InstrT;

  Register* start() const {
    return GetOperand(0);
  }

  Register* stop() const {
    return GetOperand(1);
  }

  Register* step() const {
    return NumOperands() == 2 ? nullptr : GetOperand(2);
  }
};

// Builds a new Function object, with the given code object and optionally a
// qualified name.
DEFINE_SIMPLE_INSTR(
    MakeFunction,
    (TCode, TOptObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Takes a list as operand 0
// Takes an item as operand 1
DEFINE_SIMPLE_INSTR(
    ListAppend,
    (Constraint::kListOrChkList, TOptObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// extend the list with the elements in iterable
// Takes a list as operand 0
// Takes an iterable as operand 1
DEFINE_SIMPLE_INSTR(
    ListExtend,
    (Constraint::kListOrChkList, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Gets a tuple representation from a sequence.
DEFINE_SIMPLE_INSTR(GetTuple, (TObject), HasOutput, Operands<1>, DeoptBase);

// An unconditional branch
class INSTR_CLASS(Branch, (), Operands<0>) {
 public:
  explicit Branch(BasicBlock* target) : InstrT() {
    set_target(target);
  }

  BasicBlock* target() const {
    return edge_.to();
  }

  void set_target(BasicBlock* target) {
    edge_.set_to(target);
  }

  std::span<const Edge> edges() const override;

 private:
  Edge edge_;
};

enum class FunctionAttr {
  kClosure,
  kAnnotations,
  kKwDefaults,
  kDefaults,
  kAnnotate,
};

const char* functionFieldName(FunctionAttr field);

class INSTR_CLASS(SetFunctionAttr, (TObject, TFunc), Operands<2>) {
 public:
  SetFunctionAttr(Register* value, Register* base, FunctionAttr field)
      : InstrT(value, base), field_(field) {}

  Register* value() const {
    return GetOperand(0);
  }

  Register* base() const {
    return GetOperand(1);
  }

  FunctionAttr field() const {
    return field_;
  }

  uint64_t offset() const {
    switch (field_) {
      case FunctionAttr::kClosure:
        return offsetof(PyFunctionObject, func_closure);
      case FunctionAttr::kAnnotations:
        return offsetof(PyFunctionObject, func_annotations);
      case FunctionAttr::kKwDefaults:
        return offsetof(PyFunctionObject, func_kwdefaults);
      case FunctionAttr::kDefaults:
        return offsetof(PyFunctionObject, func_defaults);
      case FunctionAttr::kAnnotate:
#if PY_VERSION_HEX >= 0x030E0000
        return offsetof(PyFunctionObject, func_annotate);
#else
        JIT_ABORT("FunctionAttr::kAnnotate not supported before 3.14");
#endif
    }
    JIT_ABORT("Invalid field {}", static_cast<int>(field_));
  }

 private:
  FunctionAttr field_;
};

enum class CallFlags : uint32_t {
  None = 0,

  KwArgs = 1 << 0,
  Awaited = 1 << 1,
  Static = 1 << 2,
};

constexpr uint32_t raw(CallFlags flags) {
  return static_cast<uint32_t>(flags);
}

constexpr CallFlags operator|(const CallFlags& a, const CallFlags& b) {
  return static_cast<CallFlags>(raw(a) | raw(b));
}

constexpr CallFlags& operator|=(CallFlags& a, const CallFlags& b) {
  a = a | b;
  return a;
}

// Common case is to test for flags so this returns a bool.
constexpr bool operator&(const CallFlags& a, const CallFlags& b) {
  return static_cast<bool>(raw(a) & raw(b));
}

class INSTR_CLASS(VectorCall, (TOptObject), HasOutput, Operands<>, DeoptBase) {
 public:
  VectorCall(Register* dst, CallFlags flags) : InstrT{dst}, flags_{flags} {}

  VectorCall(Register* dst, CallFlags flags, const FrameState& frame)
      : VectorCall{dst, flags} {
    setFrameState(frame);
  }

  // The function to call
  Register* func() const {
    return GetOperand(0);
  }

  std::size_t numArgs() const {
    return NumOperands() - 1;
  }

  Register* arg(std::size_t i) const {
    return GetOperand(i + 1);
  }

  CallFlags flags() const {
    return flags_;
  }

 private:
  CallFlags flags_;
};

class INSTR_CLASS(
    CallEx,
    (TObject, TObject, TOptObject),
    HasOutput,
    Operands<3>,
    DeoptBase) {
 public:
  CallEx(
      Register* dst,
      Register* func,
      Register* pargs,
      Register* kwargs,
      CallFlags flags)
      : InstrT{dst, func, pargs, kwargs}, flags_{flags} {
    JIT_CHECK(
        !(flags_ & CallFlags::Static), "CallEx doesn't support Static Python");
  }

  CallEx(
      Register* dst,
      Register* func,
      Register* pargs,
      Register* kwargs,
      CallFlags flags,
      const FrameState& frame)
      : CallEx{dst, func, pargs, kwargs, flags} {
    setFrameState(frame);
  }

  // The function to call
  Register* func() const {
    return GetOperand(0);
  }

  Register* pargs() const {
    return GetOperand(1);
  }

  Register* kwargs() const {
    return GetOperand(2);
  }

  CallFlags flags() const {
    return flags_;
  }

 private:
  CallFlags flags_;
};

// Call to one of the C functions defined by CallCFunc_FUNCS. We have a static
// set of functions so we can (one day) safely (de)serialize HIR fully.
class INSTR_CLASS(CallCFunc, (TOptObject | TCUInt64), HasOutput, Operands<>) {
 public:
#if PY_VERSION_HEX >= 0x030C0000
#define CallCFunc_FUNCS(X)         \
  X(Cix_PyAsyncGenValueWrapperNew) \
  X(JitCoro_GetAwaitableIter)      \
  X(JitGen_yf)
#else
// List of allowed functions
#define CallCFunc_FUNCS(X)         \
  X(Cix_PyAsyncGenValueWrapperNew) \
  X(Cix_PyCoro_GetAwaitableIter)   \
  X(Cix_PyGen_yf)
#endif

  enum class Func {
#define ENUM_FUNC(name, ...) k##name,
    CallCFunc_FUNCS(ENUM_FUNC)
#undef ENUM_FUNC
  };

  CallCFunc(Register* dst, Func func, const std::vector<Register*>& args)
      : InstrT(dst), func_(func) {
    size_t i = 0;
    for (Register* arg : args) {
      SetOperand(i++, arg);
    }
  }

  std::string_view funcName() const;

  Func func() const {
    return func_;
  }

 private:
  const Func func_;
};

inline std::ostream& operator<<(std::ostream& os, CallCFunc::Func func) {
  switch (func) {
#define CALL_CFUNC_STR(X, ...) \
  case CallCFunc::Func::k##X:  \
    return os << #X;
    CallCFunc_FUNCS(CALL_CFUNC_STR)
#undef CALL_CFUNC_STR
        default : break;
  }
  return os << "<unknown CallCFunc>";
}

// Call to a C function pointer, the return value indicates an error. If the
// return type is PyObject then an error is indicated by returning NULL. If
// the return type is a primitive then edx is zero if returning an integer or
// xmm1 if returning a floating point value.
class INSTR_CLASS(CallInd, (TTop), HasOutput, Operands<>, DeoptBase) {
 public:
  CallInd(Register* dst, const char* name, Type ret_type)
      : InstrT(dst), name_(name), ret_type_(ret_type) {}

  template <typename... Args>
  CallInd(Register* dst, const char* name, Type ret_type, Args&&... args)
      : InstrT(dst), name_(name), ret_type_(ret_type) {
    std::array<Register*, sizeof...(Args)> operands{args...};
    JIT_CHECK(
        operands.size() == NumOperands(),
        "Expected {} arguments, got {}",
        NumOperands(),
        operands.size());
    size_t i = 0;
    for (Register* operand : operands) {
      SetOperand(i++, operand);
    }
  }

  const char* name() const {
    return name_;
  }

  Register* func() const {
    return GetOperand(0);
  }

  Type ret_type() const {
    return ret_type_;
  }

  int arg_count() const {
    return NumOperands() - 1;
  }

  Register* arg(int arg) const {
    return GetOperand(arg + 1);
  }

 private:
  const char* name_;
  Type ret_type_;
};

class INSTR_CLASS(
    CallIntrinsic,
    (TOptObject | TCUInt64),
    HasOutput,
    Operands<>) {
 public:
  CallIntrinsic(Register* dst, size_t index, const std::vector<Register*>& args)
      : InstrT(dst), index_(index) {
    size_t i = 0;
    for (Register* arg : args) {
      SetOperand(i++, arg);
    }
  }

  size_t index() const {
    return index_;
  }

 private:
  const size_t index_;
};

// Phi instruction
class INSTR_CLASS(Phi, (TTop), HasOutput, Operands<>) {
 public:
  explicit Phi(Register* dst) : InstrT(dst) {}

  static Phi* create(
      Register* dst,
      const std::unordered_map<BasicBlock*, Register*>& args) {
    void* ptr = Instr::allocate(sizeof(Phi), args.size());
    auto phi = new (ptr) Phi(dst);
    phi->setArgs(args);
    return phi;
  }

  // A trivial phi merges its output with only one other value.
  Register* isTrivial() const {
    Register* out = output();
    Register* val = nullptr;
    for (std::size_t i = 0; i < NumOperands(); i++) {
      Register* reg = GetOperand(i);
      if (reg != out && reg != val) {
        if (val != nullptr) {
          return nullptr;
        }
        val = reg;
      }
    }
    return val;
  }

  // Return the index of the given predecessor in basic_blocks.
  std::size_t blockIndex(const BasicBlock* block) const;

  const std::vector<BasicBlock*> basic_blocks() const {
    return basic_blocks_;
  }

  void setArgs(const std::unordered_map<BasicBlock*, Register*>& args);

 private:
  // List of incoming blocks, sorted by ascending block ID.
  std::vector<BasicBlock*> basic_blocks_;
};

// The first operand is the receiver that was used for the corresponding
// LoadMethod. The second operand is the callable to call. The remaining
// operands are arguments to the call.
class INSTR_CLASS(CallMethod, (TOptObject), HasOutput, Operands<>, DeoptBase) {
 public:
  CallMethod(Register* dst, CallFlags flags) : InstrT{dst}, flags_{flags} {
    JIT_CHECK(
        !(flags_ & CallFlags::Static),
        "CallMethod doesn't support Static Python");
  }

  CallMethod(Register* dst, CallFlags flags, const FrameState& frame)
      : CallMethod{dst, flags} {
    setFrameState(frame);
  }

  // The function to call
  Register* func() const {
    return GetOperand(0);
  }

  // The register containing the receiver used to perform the method lookup
  Register* self() const {
    return GetOperand(1);
  }

  std::size_t NumArgs() const {
    return NumOperands() - 2;
  }

  Register* arg(std::size_t i) const {
    return GetOperand(i + 2);
  }

  CallFlags flags() const {
    return flags_;
  }

 private:
  CallFlags flags_;
};

// A call to a function at a known address
class INSTR_CLASS(CallStatic, (TTop), HasOutput, Operands<>) {
 public:
  CallStatic(Register* out, void* addr, Type ret_type)
      : InstrT(out), addr_(addr), ret_type_(ret_type) {}

  template <typename... Args>
  CallStatic(Register* out, void* addr, Type ret_type, Args&&... args)
      : InstrT(out), addr_(addr), ret_type_(ret_type) {
    std::array<Register*, sizeof...(Args)> operands{args...};
    JIT_CHECK(
        operands.size() == NumOperands(),
        "Expected {} arguments, got {}",
        NumOperands(),
        operands.size());
    size_t i = 0;
    for (Register* operand : operands) {
      SetOperand(i++, operand);
    }
  }

  std::size_t NumArgs() const {
    return NumOperands();
  }

  Register* arg(std::size_t i) const {
    return GetOperand(i);
  }

  void* addr() const {
    return addr_;
  }

  Type ret_type() const {
    return ret_type_;
  }

 private:
  void* addr_;
  Type ret_type_;
};

// A call to a function at a known address
class INSTR_CLASS(CallStaticRetVoid, (TTop), Operands<>) {
 public:
  explicit CallStaticRetVoid(void* addr) : InstrT(), addr_(addr) {}

  std::size_t NumArgs() const {
    return NumOperands();
  }

  Register* arg(std::size_t i) const {
    return GetOperand(i);
  }

  void* addr() const {
    return addr_;
  }

 private:
  void* addr_;
};

// Invokes a function with a static entry point, where we can
// directly provide the arguments using the x64 calling convention.
class INSTR_CLASS(
    InvokeStaticFunction,
    (TTop),
    HasOutput,
    Operands<>,
    DeoptBase) {
 public:
  // Would be better not to have this constructor, we shouldn't use it, but
  // currently newInstr in the parser requires it, T85605140
  InvokeStaticFunction(
      Register* dst,
      PyFunctionObject* func,
      Type ret_type,
      const FrameState& frame)
      : InstrT(dst, frame), func_(func), ret_type_(ret_type) {}

  InvokeStaticFunction(Register* dst, PyFunctionObject* func, Type ret_type)
      : InstrT(dst), func_(func), ret_type_(ret_type) {}

  std::size_t NumArgs() const {
    return NumOperands();
  }

  Register* arg(std::size_t i) const {
    return GetOperand(i);
  }

  PyFunctionObject* func() const {
    return func_;
  }

  Type ret_type() const {
    return ret_type_;
  }

 private:
  PyFunctionObject* func_;
  Type ret_type_;
};

class CheckBase : public DeoptBase {
 protected:
  // Used only for tests.
  explicit CheckBase(Opcode op) : DeoptBase(op) {
    auto new_frame = std::make_unique<FrameState>();
    setFrameState(std::move(new_frame));
  }

  CheckBase(Opcode op, const FrameState& frame) : DeoptBase(op, frame) {}

 public:
  Register* reg() const {
    return GetOperand(0);
  }
};

// Check if an error has occurred (_PyErr_Occurred() is true).
// If so, transfer control to the exception handler for the block.
DEFINE_SIMPLE_INSTR(CheckErrOccurred, (), Operands<0>, CheckBase);

// Check if an exception has occurred (implied by var being NULL).
// If so, transfer control to the exception handler for the block.
DEFINE_SIMPLE_INSTR(
    CheckExc,
    (Constraint::kOptObjectOrCInt),
    HasOutput,
    Operands<1>,
    CheckBase);

// Check if an exception has occurred as indicated by a negative
// return code.
DEFINE_SIMPLE_INSTR(CheckNeg, (TCInt), HasOutput, Operands<1>, CheckBase);

class INSTR_CLASS(GetSecondOutput, (TTop), HasOutput, Operands<1>) {
 public:
  GetSecondOutput(Register* dst, Type type, Register* src)
      : InstrT(dst, src), type_(type) {}

  Type type() const {
    return type_;
  }

 private:
  Type type_;
};

class CheckBaseWithName : public CheckBase {
 protected:
  // Used only for tests.
  CheckBaseWithName(Opcode op, BorrowedRef<> name)
      : CheckBase(op), name_(name) {}

  CheckBaseWithName(Opcode op, BorrowedRef<> name, const FrameState& frame)
      : CheckBase(op, frame), name_(name) {}

 public:
  BorrowedRef<> name() const {
    return name_;
  }

 private:
  BorrowedRef<> name_;
};

// If the operand is Nullptr, raise an UnboundLocalError referencing the
// given local variable name.
DEFINE_SIMPLE_INSTR(
    CheckVar,
    (TOptObject),
    HasOutput,
    Operands<1>,
    CheckBaseWithName);

// If the operand is Nullptr, raise a NameError referencing the given free
// variable name.
DEFINE_SIMPLE_INSTR(
    CheckFreevar,
    (TOptObject),
    HasOutput,
    Operands<1>,
    CheckBaseWithName);

// If the operand is Nullptr, raise an AttributeError referencing the given
// attribute/field name.
DEFINE_SIMPLE_INSTR(
    CheckField,
    (TOptObject),
    HasOutput,
    Operands<1>,
    CheckBaseWithName);

DEFINE_SIMPLE_INSTR(
    IsNegativeAndErrOccurred,
    (TCInt),
    HasOutput,
    Operands<1>,
    DeoptBase);

class INSTR_CLASS(LoadField, (TOptObject), HasOutput, Operands<1>) {
 public:
  LoadField(
      Register* dst,
      Register* receiver,
      const std::string& name,
      std::size_t offset,
      Type type,
      bool borrowed = true)
      : InstrT(dst, receiver),
        name_(name),
        offset_(offset),
        type_(type),
        borrowed_(borrowed) {}

  // The object we're loading the attribute from
  Register* receiver() const {
    return reg();
  }

  std::string name() const {
    return name_;
  }

  // Offset where the field is stored
  std::size_t offset() const {
    return offset_;
  }

  Type type() const {
    return type_;
  }

  bool borrowed() const {
    return borrowed_;
  }

 private:
  std::string name_;
  std::size_t offset_;
  Type type_;
  bool borrowed_;
};

class INSTR_CLASS(StoreField, (TObject, TTop, TOptObject), Operands<3>) {
 public:
  StoreField(
      Register* receiver,
      const std::string& name,
      std::size_t offset,
      Register* value,
      Type type,
      Register* previous // for keeping the prevous value of the field alive
                         // (for refcount insertion) until after the store.
      )
      : InstrT(receiver, value, previous),
        name_(name),
        offset_(offset),
        type_(type) {}

  // The object we're loading the attribute from
  Register* receiver() const {
    return GetOperand(0);
  }

  void set_receiver(Register* receiver) {
    SetOperand(0, receiver);
  }

  // The value being stored
  Register* value() const {
    return GetOperand(1);
  }

  void set_value(Register* value) {
    SetOperand(1, value);
  }

  std::string name() const {
    return name_;
  }

  // Offset where the field is stored
  std::size_t offset() const {
    return offset_;
  }

  Type type() const {
    return type_;
  }

 private:
  std::string name_;
  std::size_t offset_;
  Type type_;
};

class INSTR_CLASS(Cast, (TObject), HasOutput, Operands<1>, DeoptBase) {
 public:
  Cast(
      Register* dst,
      Register* receiver,
      PyTypeObject* pytype,
      bool optional,
      bool exact,
      const FrameState& frame)
      : InstrT(dst, receiver, frame),
        pytype_(pytype),
        optional_(optional),
        exact_(exact) {}

  Register* value() const {
    return reg();
  }

  PyTypeObject* pytype() const {
    return pytype_;
  }

  bool optional() const {
    return optional_;
  }

  bool exact() const {
    return exact_;
  }

 private:
  PyTypeObject* pytype_;
  bool optional_;
  bool exact_;
};

class INSTR_CLASS(TpAlloc, (), HasOutput, Operands<0>, DeoptBase) {
 public:
  TpAlloc(Register* dst, PyTypeObject* pytype, const FrameState& frame)
      : InstrT(dst, frame), pytype_(pytype) {}

  PyTypeObject* pytype() const {
    return pytype_;
  }

 private:
  PyTypeObject* pytype_;
};

// Perform a binary operation (e.g. '+', '-') on primitive int operands
class INSTR_CLASS(
    IntBinaryOp,
    (Constraint::kMatchAllAsCInt, Constraint::kMatchAllAsCInt),
    HasOutput,
    Operands<2>) {
 public:
  IntBinaryOp(Register* dst, BinaryOpKind op, Register* left, Register* right)
      : InstrT(dst, left, right), op_(op) {}

  BinaryOpKind op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  BinaryOpKind op_;
};

// Perform a binary operation (e.g. '+', '-') on primitive double operands
class INSTR_CLASS(
    DoubleBinaryOp,
    (TCDouble, TCDouble),
    HasOutput,
    Operands<2>) {
 public:
  DoubleBinaryOp(
      Register* dst,
      BinaryOpKind op,
      Register* left,
      Register* right)
      : InstrT(dst, left, right), op_(op) {}

  BinaryOpKind op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  BinaryOpKind op_;
};

class InlineBase {
 public:
  virtual ~InlineBase() = default;
  virtual int inlineDepth() const = 0;
};

// Owns a FrameState that all inlined FrameState-owning instructions will point
// to via FrameState's `parent' pointer.
class INSTR_CLASS(BeginInlinedFunction, (), Operands<0>), public InlineBase {
 public:
  BeginInlinedFunction(
      BorrowedRef<PyFunctionObject> func,
      std::unique_ptr<FrameState> caller_state,
      const std::string& fullname,
      BorrowedRef<> reifier)
      : InstrT(), func_(func), reifier_(reifier), fullname_(fullname) {
    caller_state_ = std::move(caller_state);
  }

  // Note: The copy constructor creates a new FrameState - this means that
  // inlined FrameStates will not point to the copied FrameState as their parent
  BeginInlinedFunction(const BeginInlinedFunction& other)
      : InstrT(), func_(other.func()), fullname_(other.fullname()) {
    caller_state_ = std::make_unique<FrameState>(*other.callerFrameState());
  }

  const FrameState* callerFrameState() const {
    return caller_state_.get();
  }

  BorrowedRef<PyFunctionObject> func() const {
    return func_;
  }

  BorrowedRef<PyCodeObject> code() const {
    return func_->func_code;
  }

  std::string fullname() const {
    return fullname_;
  }

  BorrowedRef<> builtins() const {
    return func_->func_builtins;
  }

  BorrowedRef<PyObject> globals() const {
    return func_->func_globals;
  }

  BorrowedRef<> reifier() const {
    return reifier_;
  }

  int inlineDepth() const override {
    return caller_state_->inlineDepth() + 1;
  }

 private:
  // BeginInlinedFunction must own the FrameState that is used for building the
  // linked list of FrameStates as well as its parent FrameState. The parent is
  // originally owned by the Call instruction, but that gets destroyed.
  // Used for printing.
  BorrowedRef<PyFunctionObject> func_;
  BorrowedRef<> reifier_;
  std::unique_ptr<FrameState> caller_state_{nullptr};
  std::string fullname_;
};

class INSTR_CLASS(EndInlinedFunction, (), Operands<0>), public InlineBase {
 public:
  explicit EndInlinedFunction(BeginInlinedFunction * begin)
      : InstrT(), begin_(begin), inline_depth_(begin->inlineDepth()) {}

  BeginInlinedFunction* matchingBegin() const {
    return begin_;
  }

  int inlineDepth() const override {
    return inline_depth_;
  }

 private:
  BeginInlinedFunction* begin_{nullptr};
  int inline_depth_{-1};
};

#define FOREACH_PRIMITIVE_UNARY_OP_KIND(V) \
  V(NegateInt)                             \
  V(InvertInt)                             \
  V(NotInt)

enum class PrimitiveUnaryOpKind {
#define DEFINE_OP(NAME) k##NAME,
  FOREACH_PRIMITIVE_UNARY_OP_KIND(DEFINE_OP)
#undef DEFINE_OP
};

#define COUNT_OP(NAME) +1
constexpr size_t kNumPrimitiveUnaryOpKinds =
    FOREACH_PRIMITIVE_UNARY_OP_KIND(COUNT_OP);
#undef COUNT_OP

std::string_view GetPrimitiveUnaryOpName(PrimitiveUnaryOpKind op);
PrimitiveUnaryOpKind ParsePrimitiveUnaryOpName(std::string_view name);

// Perform a unary operation (e.g. '~', '-') on primitive operands
class INSTR_CLASS(PrimitiveUnaryOp, (TPrimitive), HasOutput, Operands<1>) {
 public:
  PrimitiveUnaryOp(Register* dst, PrimitiveUnaryOpKind op, Register* value)
      : InstrT(dst, value), op_(op) {}

  PrimitiveUnaryOpKind op() const {
    return op_;
  }

  Register* value() const {
    return GetOperand(0);
  }

 private:
  PrimitiveUnaryOpKind op_;
};

#define FOREACH_COMPARE_OP(V)                               \
  /* Begin rich comparison opcodes. */                      \
  V(LessThan)                                               \
  V(LessThanEqual)                                          \
  V(Equal)                                                  \
  V(NotEqual)                                               \
  V(GreaterThan)                                            \
  V(GreaterThanEqual)                                       \
  /* End rich comparison opcodes. */                        \
  V(In)                                                     \
  V(NotIn)                                                  \
  /* Note: Is and IsNot are handled by PrimitiveCompare. */ \
  V(ExcMatch)                                               \
  V(GreaterThanUnsigned)                                    \
  V(GreaterThanEqualUnsigned)                               \
  V(LessThanUnsigned)                                       \
  V(LessThanEqualUnsigned)

enum class CompareOp {
#define DEFINE_OP(NAME) k##NAME,
  FOREACH_COMPARE_OP(DEFINE_OP)
#undef DEFINE_OP
};

#define COUNT_OP(NAME) +1
constexpr size_t kNumCompareOps = FOREACH_COMPARE_OP(COUNT_OP);
#undef COUNT_OP

std::string_view GetCompareOpName(CompareOp op);
CompareOp ParseCompareOpName(std::string_view name);

// Perform the comparison indicated by op
class INSTR_CLASS(
    Compare,
    (TOptObject, TOptObject),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  Compare(
      Register* dst,
      CompareOp op,
      Register* left,
      Register* right,
      const FrameState& frame)
      : InstrT(dst, left, right, frame), op_(op) {}

  CompareOp op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  CompareOp op_;
};

// Perform the comparison indicated by op between two floats
class INSTR_CLASS(
    FloatCompare,
    (TFloatExact, TFloatExact),
    HasOutput,
    Operands<2>) {
 public:
  FloatCompare(Register* dst, CompareOp op, Register* left, Register* right)
      : InstrT(dst, left, right), op_(op) {}

  CompareOp op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  CompareOp op_;
};

// Perform the comparison indicated by op between two longs
class INSTR_CLASS(
    LongCompare,
    (TLongExact, TLongExact),
    HasOutput,
    Operands<2>) {
 public:
  LongCompare(Register* dst, CompareOp op, Register* left, Register* right)
      : InstrT(dst, left, right), op_(op) {}

  CompareOp op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  CompareOp op_;
};

// Perform the comparison indicated by op between two strings
class INSTR_CLASS(
    UnicodeCompare,
    (TUnicodeExact, TUnicodeExact),
    HasOutput,
    Operands<2>) {
 public:
  UnicodeCompare(Register* dst, CompareOp op, Register* left, Register* right)
      : InstrT(dst, left, right), op_(op) {}

  CompareOp op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  CompareOp op_;
};

// Perform BinaryOp<Add> with two strings
DEFINE_SIMPLE_INSTR(
    UnicodeConcat,
    (TUnicodeExact, TUnicodeExact),
    HasOutput,
    Operands<2>,
    DeoptBase);

DEFINE_SIMPLE_INSTR(
    CopyDictWithoutKeys,
    (TObject, TTupleExact),
    HasOutput,
    Operands<2>,
    DeoptBase);

DEFINE_SIMPLE_INSTR(
    UnicodeRepeat,
    (TUnicodeExact, TCInt64),
    HasOutput,
    Operands<2>,
    DeoptBase);

DEFINE_SIMPLE_INSTR(
    UnicodeSubscr,
    (TUnicodeExact, TCInt64),
    HasOutput,
    Operands<2>,
    DeoptBase);

// NB: This needs to be in the order that the values appear in the
// BinaryOpKind enum
const std::array<binaryfunc, kNumBinaryOpKinds> kLongBinaryOpSlotMethods = {
    /* kAdd                 */ PyLong_Type.tp_as_number->nb_add,
    /* kAnd                 */ PyLong_Type.tp_as_number->nb_and,
    /* kFloorDivide         */ PyLong_Type.tp_as_number->nb_floor_divide,
    /* kLShift              */ PyLong_Type.tp_as_number->nb_lshift,
    /* kMatrixMultiply      */ nullptr, // unsupported: matrix multiply
    /* kModulo              */ PyLong_Type.tp_as_number->nb_remainder,
    /* kMultiply            */ PyLong_Type.tp_as_number->nb_multiply,
    /* kOr                  */ PyLong_Type.tp_as_number->nb_or,
    /* kPower               */ nullptr, // power is ternary, handled specially
    /* kRShift              */ PyLong_Type.tp_as_number->nb_rshift,
    /* kSubscript           */ nullptr, // unsupported: getitem
    /* kSubtract            */ PyLong_Type.tp_as_number->nb_subtract,
    /* kTrueDivide          */ PyLong_Type.tp_as_number->nb_true_divide,
    /* kXor                 */ PyLong_Type.tp_as_number->nb_xor,
    /* kFloorDivideUnsigned */ nullptr,
    /* kModuloUnsigned      */ nullptr,
    /* kRShiftUnsigned      */ nullptr,
    /* kPowerUnsigned       */ nullptr,
};

// Perform the operation indicated by op
class INSTR_CLASS(
    LongBinaryOp,
    (TLongExact, TLongExact),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  LongBinaryOp(
      Register* dst,
      BinaryOpKind op,
      Register* left,
      Register* right,
      const FrameState& frame)
      : InstrT(dst, left, right, frame), op_(op) {}

  BinaryOpKind op() const {
    return op_;
  }

  binaryfunc slotMethod() const {
    auto op_kind = static_cast<unsigned long>(op());
    JIT_CHECK(op_kind < kLongBinaryOpSlotMethods.size(), "unsupported binop");
    binaryfunc helper = kLongBinaryOpSlotMethods[op_kind];
    JIT_DCHECK(helper != nullptr, "unsupported slot method");
    return helper;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  BinaryOpKind op_;
};

const std::array<binaryfunc, kNumInPlaceOpKinds> kLongInPlaceOpSlotMethods = {
    // These don't use "nb_inplace" versions because those don't exist and we
    // fallback to the non-inplace versions
    PyLong_Type.tp_as_number->nb_add,
    PyLong_Type.tp_as_number->nb_and,
    PyLong_Type.tp_as_number->nb_floor_divide,
    PyLong_Type.tp_as_number->nb_lshift,
    nullptr, // unsupported: matrix multiply
    PyLong_Type.tp_as_number->nb_remainder,
    PyLong_Type.tp_as_number->nb_multiply,
    PyLong_Type.tp_as_number->nb_or,
    nullptr, // power is ternary and handled specially
    PyLong_Type.tp_as_number->nb_rshift,
    PyLong_Type.tp_as_number->nb_subtract,
    PyLong_Type.tp_as_number->nb_true_divide,
    PyLong_Type.tp_as_number->nb_xor,
};

class INSTR_CLASS(
    LongInPlaceOp,
    (TLongExact, TLongExact),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  LongInPlaceOp(
      Register* dst,
      InPlaceOpKind op,
      Register* left,
      Register* right,
      const FrameState& frame)
      : InstrT(dst, left, right, frame), op_(op) {}

  InPlaceOpKind op() const {
    return op_;
  }

  binaryfunc slotMethod() const {
    auto op_kind = static_cast<unsigned long>(op());
    JIT_CHECK(op_kind < kLongInPlaceOpSlotMethods.size(), "unsupported binop");
    binaryfunc helper = kLongInPlaceOpSlotMethods[op_kind];
    JIT_DCHECK(helper != nullptr, "unsupported slot method");
    return helper;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  InPlaceOpKind op_;
};

const std::array<binaryfunc, kNumBinaryOpKinds> kFloatBinaryOpSlotMethods = {
    /* kAdd                 */ PyFloat_Type.tp_as_number->nb_add,
    /* kAnd                 */ nullptr,
    /* kFloorDivide         */ PyFloat_Type.tp_as_number->nb_floor_divide,
    /* kLShift              */ nullptr,
    /* kMatrixMultiply      */ nullptr,
    /* kModulo              */ PyFloat_Type.tp_as_number->nb_remainder,
    /* kMultiply            */ PyFloat_Type.tp_as_number->nb_multiply,
    /* kOr                  */ nullptr,
    /* kPower               */ nullptr,
    /* kRShift              */ nullptr,
    /* kSubscript           */ nullptr,
    /* kSubtract            */ PyFloat_Type.tp_as_number->nb_subtract,
    /* kTrueDivide          */ PyFloat_Type.tp_as_number->nb_true_divide,
    /* kXor                 */ nullptr,
    /* kFloorDivideUnsigned */ nullptr,
    /* kModuloUnsigned      */ nullptr,
    /* kRShiftUnsigned      */ nullptr,
    /* kPowerUnsigned       */ nullptr,
};

// Perform the operation indicated by op
class INSTR_CLASS(
    FloatBinaryOp,
    (TFloatExact, TFloatExact),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  FloatBinaryOp(
      Register* dst,
      BinaryOpKind op,
      Register* left,
      Register* right,
      const FrameState& frame)
      : InstrT(dst, left, right, frame), op_(op) {}

  BinaryOpKind op() const {
    return op_;
  }

  binaryfunc slotMethod() const {
    auto helper = FloatBinaryOp::slotMethod(op());
    JIT_DCHECK(helper != std::nullopt, "unsupported slot method");
    return helper.value();
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

  static std::optional<binaryfunc> slotMethod(BinaryOpKind op) {
    auto op_kind = static_cast<unsigned long>(op);
    JIT_CHECK(op_kind < kFloatBinaryOpSlotMethods.size(), "unsupported binop");

    binaryfunc func = kFloatBinaryOpSlotMethods[op_kind];
    return func == nullptr ? std::nullopt : std::make_optional(func);
  }

 private:
  BinaryOpKind op_;
};

// Like Compare but has an Int32 output so it can be used to replace
// a Compare + IsTruthy.
class INSTR_CLASS(
    CompareBool,
    (TObject, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  CompareBool(
      Register* dst,
      CompareOp op,
      Register* left,
      Register* right,
      const FrameState& frame)
      : InstrT(dst, left, right, frame), op_(op) {}

  CompareOp op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

 private:
  CompareOp op_;
};

class INSTR_CLASS(IntConvert, (TPrimitive), HasOutput, Operands<1>) {
 public:
  IntConvert(Register* dst, Register* src, Type type)
      : InstrT(dst, src), type_(type) {}

  Register* src() const {
    return GetOperand(0);
  }

  Type type() const {
    return type_;
  }

 private:
  Type type_;
};

#define FOREACH_PRIMITIVE_COMPARE_OP(V) \
  V(LessThan)                           \
  V(LessThanEqual)                      \
  V(Equal)                              \
  V(NotEqual)                           \
  V(GreaterThan)                        \
  V(GreaterThanEqual)                   \
  V(GreaterThanUnsigned)                \
  V(GreaterThanEqualUnsigned)           \
  V(LessThanUnsigned)                   \
  V(LessThanEqualUnsigned)

enum class PrimitiveCompareOp {
#define DEFINE_OP(NAME) k##NAME,
  FOREACH_PRIMITIVE_COMPARE_OP(DEFINE_OP)
#undef DEFINE_OP
};

#define COUNT_OP(NAME) +1
constexpr size_t kNumPrimitiveCompareOps =
    FOREACH_PRIMITIVE_COMPARE_OP(COUNT_OP);
#undef COUNT_OP

std::string_view GetPrimitiveCompareOpName(PrimitiveCompareOp op);
PrimitiveCompareOp ParsePrimitiveCompareOpName(std::string_view name);

// Convert a CompareOp into an equivalent PrimitiveCompareOp, if it exists.
std::optional<PrimitiveCompareOp> toPrimitiveCompareOp(CompareOp op);

class INSTR_CLASS(PrimitiveCompare, (), HasOutput, Operands<2>) {
 public:
  PrimitiveCompare(
      Register* dst,
      PrimitiveCompareOp op,
      Register* left,
      Register* right)
      : InstrT(dst, left, right), op_(op) {}

  PrimitiveCompareOp op() const {
    return op_;
  }

  Register* left() const {
    return GetOperand(0);
  }

  Register* right() const {
    return GetOperand(1);
  }

  OperandType GetOperandTypeImpl(std::size_t /* i */) const {
    // `is` gets treated as a PrimtiveCompare and can hold anything
    if (op_ == PrimitiveCompareOp::kEqual ||
        op_ == PrimitiveCompareOp::kNotEqual) {
      return TTop;
    } else {
      return {Constraint::kMatchAllAsPrimitive};
    }
  }

 private:
  PrimitiveCompareOp op_;
};

DEFINE_SIMPLE_INSTR(PrimitiveBoxBool, (TCBool), HasOutput, Operands<1>);

class INSTR_CLASS(
    PrimitiveBox,
    (TPrimitive),
    HasOutput,
    Operands<1>,
    DeoptBase) {
 public:
  PrimitiveBox(
      Register* dst,
      Register* value,
      Type type,
      const FrameState& frame)
      : InstrT(dst, value, frame), type_(type) {
    JIT_CHECK(
        !(type <= TCBool),
        "PrimitiveBox does not support TCBool; use PrimitiveBoxBool instead.");
  }

  Register* value() const {
    return GetOperand(0);
  }

  Type type() const {
    return type_;
  }

  OperandType GetOperandTypeImpl(std::size_t /* i */) const {
    return type_;
  }

 private:
  Type type_;
};

class INSTR_CLASS(PrimitiveUnbox, (), HasOutput, Operands<1>) {
 public:
  PrimitiveUnbox(Register* dst, Register* value, Type type)
      : InstrT(dst, value), type_(type) {}

  Register* value() const {
    return GetOperand(0);
  }

  Type type() const {
    return type_;
  }

  OperandType GetOperandTypeImpl(std::size_t /* i */) const {
    return type_.asBoxed();
  }

 private:
  Type type_;
};

// Similar to PrimitiveUnbox, but uses PyNumber_AsSsize_t() instead of
// PyLong_AsSize_t() for a different exception and message on overflow.
class INSTR_CLASS(IndexUnbox, (TLong), HasOutput, Operands<1>) {
 public:
  IndexUnbox(Register* dst, Register* value, PyObject* exc = PyExc_IndexError)
      : InstrT(dst, value), exc_(exc) {}

  PyObject* exception() const {
    return exc_;
  }

 private:
  PyObject* exc_;
};

class CondBranchBase : public Instr {
 public:
  CondBranchBase(Opcode opcode, BasicBlock* true_bb, BasicBlock* false_bb)
      : Instr(opcode) {
    set_true_bb(true_bb);
    set_false_bb(false_bb);
  }

  BasicBlock* true_bb() const {
    return true_edge_.to();
  }

  void set_true_bb(BasicBlock* block) {
    true_edge_.set_to(block);
  }

  BasicBlock* false_bb() const {
    return false_edge_.to();
  }

  void set_false_bb(BasicBlock* block) {
    false_edge_.set_to(block);
  }

  std::span<const Edge> edges() const override;

 private:
  Edge true_edge_;
  Edge false_edge_;
};

// Transfer control to `true_bb` if `reg` is nonzero, otherwise `false_bb`.
DEFINE_SIMPLE_INSTR(
    CondBranch,
    (Constraint::kOptObjectOrCIntOrCBool),
    Operands<1>,
    CondBranchBase);

// Branch to `true_bb` if the operand is not the sentinel value that indicates
// an iterator is exhausted, or `false_bb` otherwise.
DEFINE_SIMPLE_INSTR(
    CondBranchIterNotDone,
    (TObject),
    Operands<1>,
    CondBranchBase);

// Branch to `true_bb` if the operand matches the supplied type specification,
// or `false_bb` otherwise.
class INSTR_CLASS(CondBranchCheckType, (TObject), Operands<1>, CondBranchBase) {
 public:
  CondBranchCheckType(
      Register* target,
      const Type& type,
      BasicBlock* true_bb,
      BasicBlock* false_bb)
      : InstrT(target, true_bb, false_bb), type_(type) {}

  const Type& type() const {
    return type_;
  }

 private:
  const Type type_;
};

// Decrement the reference count of `reg`
DEFINE_SIMPLE_INSTR(Decref, (TObject), Operands<1>);

// Decrement the reference count of `reg`, if `reg` is not NULL
DEFINE_SIMPLE_INSTR(XDecref, (TOptObject), Operands<1>);

// Increment the reference count of `reg`
DEFINE_SIMPLE_INSTR(Incref, (TObject), Operands<1>);

// Increment the refrence count of `reg`, if `reg` is not NULL
DEFINE_SIMPLE_INSTR(XIncref, (TOptObject), Operands<1>);

// batch decrement references
DEFINE_SIMPLE_INSTR(BatchDecref, (TObject), Operands<>);

class DeoptBaseWithNameIdx : public DeoptBase {
 public:
  DeoptBaseWithNameIdx(Opcode op, int name_idx)
      : DeoptBase(op), name_idx_(name_idx) {}

  DeoptBaseWithNameIdx(Opcode op, int name_idx, const FrameState& frame)
      : DeoptBase(op, frame), name_idx_(name_idx) {}

  // Index of the attribute name in the code object's co_names tuple.
  int name_idx() const {
    return name_idx_;
  }

  // The name object, retrieved from the code object's co_names tuple.
  BorrowedRef<PyUnicodeObject> name() const {
    return PyTuple_GET_ITEM(frameState()->code->co_names, name_idx());
  }

 private:
  int name_idx_;
};

// Load an attribute from an object. The already_optimized option is for use
// when this instruction is used as part of the slow-path in optimization for an
// initial LoadAttr.
class INSTR_CLASS(
    LoadAttr,
    (TObject),
    HasOutput,
    Operands<1>,
    DeoptBaseWithNameIdx) {
 public:
  LoadAttr(
      Register* dst,
      Register* receiver,
      int name_idx,
      const FrameState& frame,
      bool already_optimized = false)
      : InstrT(dst, receiver, name_idx, frame),
        already_optimized_(already_optimized) {}

  bool alreadyOptimized() const {
    return already_optimized_;
  }

 private:
  bool already_optimized_;
};

// Variant of LoadAttr that uses an inline cache.
DEFINE_SIMPLE_INSTR(
    LoadAttrCached,
    (TObject),
    HasOutput,
    Operands<1>,
    DeoptBaseWithNameIdx);

// Set the attribute of an object.
DEFINE_SIMPLE_INSTR(
    StoreAttr,
    (TObject, TObject),
    Operands<2>,
    DeoptBaseWithNameIdx);

// Variant of StoreAttr that uses an inline cache.
DEFINE_SIMPLE_INSTR(
    StoreAttrCached,
    (TObject, TObject),
    Operands<2>,
    DeoptBaseWithNameIdx);

// Delete an attribute from an object
DEFINE_SIMPLE_INSTR(DeleteAttr, (TObject), Operands<1>, DeoptBaseWithNameIdx);

// Load an attribute from an object, skipping the instance dictionary but still
// calling descriptors as appropriate (to create bound methods, for example).
// Note the lifetime of failure_fmt_str needs to outlive the JIT function.
class INSTR_CLASS(
    LoadAttrSpecial,
    (TObject),
    HasOutput,
    Operands<1>,
    DeoptBase) {
#if PY_VERSION_HEX >= 0x030C0000
  using IDType = PyObject;
#else
  using IDType = _Py_Identifier;
#endif
 public:
  LoadAttrSpecial(
      Register* dst,
      Register* receiver,
      IDType* id,
      const char* failure_fmt_str,
      const FrameState& frame)
      : InstrT(dst, receiver, frame),
        id_(id),
        failure_fmt_str_(failure_fmt_str) {}

  IDType* id() const {
    return id_;
  }

  const char* failureFmtStr() const {
    return failure_fmt_str_;
  }

 private:
  IDType* id_;
  const char* failure_fmt_str_;
};

// Format and raise an error after failing to get an iterator for 'async with'.
class INSTR_CLASS(RaiseAwaitableError, (TType), Operands<1>, DeoptBase) {
 public:
  RaiseAwaitableError(Register* type, bool is_aenter, const FrameState& frame)
      : InstrT{type, frame}, is_aenter_{is_aenter} {}

  bool isAEnter() const {
    return is_aenter_;
  }

 private:
  bool is_aenter_;
};

// Load a type object guard from a cache specialized for loading attributes from
// type receivers.
class INSTR_CLASS(LoadTypeAttrCacheEntryType, (), HasOutput, Operands<0>) {
 public:
  LoadTypeAttrCacheEntryType(Register* dst, int cache_id)
      : InstrT{dst}, cache_id_{cache_id} {}

  int cache_id() const {
    return cache_id_;
  }

 private:
  int cache_id_;
};

// Load a value from a cache specialized for loading attributes from type
// receivers.
class INSTR_CLASS(LoadTypeAttrCacheEntryValue, (), HasOutput, Operands<0>) {
 public:
  LoadTypeAttrCacheEntryValue(Register* dst, int cache_id)
      : InstrT{dst}, cache_id_{cache_id} {}

  int cache_id() const {
    return cache_id_;
  }

 private:
  int cache_id_;
};

// Perform a full attribute lookup. Fill the cache if the receiver is a type
// object.
class INSTR_CLASS(
    FillTypeAttrCache,
    (TType),
    HasOutput,
    Operands<1>,
    DeoptBaseWithNameIdx) {
 public:
  FillTypeAttrCache(
      Register* dst,
      Register* receiver,
      int name_idx,
      int cache_id,
      const FrameState& frame)
      : InstrT(dst, receiver, name_idx, frame), cache_id_(cache_id) {}
  FillTypeAttrCache(
      Register* dst,
      Register* receiver,
      int name_idx,
      int cache_id,
      std::unique_ptr<FrameState> frame)
      : InstrT(dst, receiver, name_idx), cache_id_(cache_id) {
    setFrameState(std::move(frame));
  }

  // The object we're loading the attribute from
  Register* receiver() const {
    return reg();
  }

  int cache_id() const {
    return cache_id_;
  }

 private:
  int cache_id_;
};

class LoadMethodBase : public DeoptBaseWithNameIdx {
 protected:
  LoadMethodBase(Opcode op, int name_idx, const FrameState& frame)
      : DeoptBaseWithNameIdx(op, name_idx, frame) {}

 public:
  // The object we're loading the attribute from
  Register* receiver() const {
    return GetOperand(0);
  }
};

// Like LoadAttr, but when we know that we're loading an attribute that will be
// used for a method call.
DEFINE_SIMPLE_INSTR(
    LoadMethod,
    (TObject),
    HasOutput,
    Operands<1>,
    LoadMethodBase);

// Variant of LoadMethod that uses an inline cache.
DEFINE_SIMPLE_INSTR(
    LoadMethodCached,
    (TObject),
    HasOutput,
    Operands<1>,
    LoadMethodBase);

// Like LoadMethod, but specialized for loading an attribute from a module
DEFINE_SIMPLE_INSTR(
    LoadModuleAttrCached,
    (TObject),
    HasOutput,
    Operands<1>,
    DeoptBaseWithNameIdx);

// Like LoadMethod, but specialized for loading a method from a module
DEFINE_SIMPLE_INSTR(
    LoadModuleMethodCached,
    (TObject),
    HasOutput,
    Operands<1>,
    LoadMethodBase);

// Return true if the instruction is an instance of LoadMethodBase.
bool isLoadMethodBase(const Instr& instr);

// Return true if the given instruction represents a subclass of LoadMethodBase
// or a Phi composed of a FillTypeMethodCache and LoadTypeMethodCacheEntryValue.
bool isAnyLoadMethod(const Instr& instr);

class LoadSuperBase : public DeoptBaseWithNameIdx {
 protected:
  LoadSuperBase(Opcode op, int name_idx, bool no_args_in_super_call)
      : DeoptBaseWithNameIdx(op, name_idx),
        no_args_in_super_call_(no_args_in_super_call) {}

  LoadSuperBase(
      Opcode op,
      int name_idx,
      bool no_args_in_super_call,
      const FrameState& frame)
      : DeoptBaseWithNameIdx(op, name_idx, frame),
        no_args_in_super_call_(no_args_in_super_call) {}

 public:
  // Global 'super' value
  Register* global_super() const {
    return GetOperand(0);
  }

  // See comment for 'receiver'
  Register* type() const {
    return GetOperand(1);
  }

  // The object that determines mro to be searched.
  // Search will be started from the class right after the 'type'
  Register* receiver() const {
    return GetOperand(2);
  }

  bool no_args_in_super_call() const {
    return no_args_in_super_call_;
  }

 private:
  bool no_args_in_super_call_;
};

DEFINE_SIMPLE_INSTR(
    LoadMethodSuper,
    (TObject, TType, TObject),
    HasOutput,
    Operands<3>,
    LoadSuperBase);
DEFINE_SIMPLE_INSTR(
    LoadAttrSuper,
    (TObject, TType, TObject),
    HasOutput,
    Operands<3>,
    LoadSuperBase);

// Perform a full method lookup. Fill the cache if the receiver does not match
// the type cached
class INSTR_CLASS(
    FillTypeMethodCache,
    (TType),
    HasOutput,
    Operands<1>,
    DeoptBaseWithNameIdx) {
 public:
  FillTypeMethodCache(
      Register* dst,
      Register* receiver,
      int name_idx,
      int cache_id,
      const FrameState& frame)
      : InstrT(dst, receiver, name_idx, frame), cache_id_(cache_id) {}
  FillTypeMethodCache(
      Register* dst,
      Register* receiver,
      int name_idx,
      int cache_id,
      std::unique_ptr<FrameState> frame)
      : InstrT(dst, receiver, name_idx), cache_id_(cache_id) {
    setFrameState(std::move(frame));
  }

  // The object we're loading the method from
  Register* receiver() const {
    return reg();
  }

  int cache_id() const {
    return cache_id_;
  }

 private:
  int cache_id_;
};

// Load the type from a cache specialized for loading methods from type
// receivers
class INSTR_CLASS(LoadTypeMethodCacheEntryType, (), HasOutput, Operands<0>) {
 public:
  LoadTypeMethodCacheEntryType(Register* dst, int cache_id)
      : InstrT(dst), cache_id_(cache_id) {}

  int cache_id() const {
    return cache_id_;
  }

 private:
  int cache_id_;
};

// Load the value from a cache specialized for loading methods from type
// receivers
class INSTR_CLASS(
    LoadTypeMethodCacheEntryValue,
    (TType),
    HasOutput,
    Operands<1>) {
 public:
  LoadTypeMethodCacheEntryValue(Register* dst, int cache_id, Register* receiver)
      : InstrT(dst, receiver), cache_id_(cache_id) {}

  int cache_id() const {
    return cache_id_;
  }

  // The type object we're loading the method from
  Register* receiver() const {
    return reg();
  }

 private:
  int cache_id_;
};
// Load the current PyFunctionObject* into a Register. Must not appear after
// any non-LoadArg instructions.
DEFINE_SIMPLE_INSTR(LoadCurrentFunc, (), HasOutput, Operands<0>);

// Load the value from the cell in operand
DEFINE_SIMPLE_INSTR(LoadCellItem, (TOptObject), HasOutput, Operands<1>);

// Load the value from the cell in src, stealing the reference to it. This is
// used only as the precursor to SetCellItem, so that we can decref the old item
// in the cell that the cell is about to lose its reference to.
DEFINE_SIMPLE_INSTR(StealCellItem, (TObject), HasOutput, Operands<1>);

// Store a value to the cell in dst. The `old` arg is unused but exists in order
// to ensure that the previous cell contents are not decref-ed until after the
// new cell contents are in place.
// Takes a cell as operand 0
// Takes a src as operand 1
// Takes in anything as operand 2
DEFINE_SIMPLE_INSTR(
    SetCellItem,
    (TObject, TOptObject, TOptObject),
    Operands<3>);

class INSTR_CLASS(InitFrameCellVars, (TObject), Operands<1>) {
 public:
  using InstrT::InstrT;
  InitFrameCellVars(Register* func, int cells) : InstrT(func), cells_(cells) {}

  Register* func() const {
    return GetOperand(0);
  }

  int num_cell_vars() const {
    return cells_;
  }

 private:
  int cells_;
};

// Load a constant value (given as a Type) into a register.
class INSTR_CLASS(LoadConst, (), HasOutput, Operands<0>) {
 public:
  LoadConst(Register* dst, Type type) : InstrT(dst), type_(type) {
    JIT_DCHECK(
        type.isSingleValue(), "Given Type must represent a single value");
  }

  Type type() const {
    return type_;
  }

 private:
  Type type_;
};

class INSTR_CLASS(LoadFunctionIndirect, (), HasOutput, Operands<0>, DeoptBase) {
 public:
  LoadFunctionIndirect(
      PyObject** funcptr,
      PyObject* descr,
      Register* dst,
      const FrameState& frame)
      : InstrT(dst, frame), funcptr_(funcptr), descr_(descr) {}

  PyObject** funcptr() const {
    return funcptr_;
  }
  PyObject* descr() const {
    return descr_;
  }

 private:
  PyObject** funcptr_;
  PyObject* descr_;
};

// Load a global.
//
// The name is specified by the name_idx in the co_names tuple of the code
// object.
class INSTR_CLASS(LoadGlobalCached, (), HasOutput, Operands<0>) {
 public:
  LoadGlobalCached(
      Register* dst,
      BorrowedRef<PyCodeObject> code,
      BorrowedRef<PyDictObject> builtins,
      BorrowedRef<PyDictObject> globals,
      int name_idx)
      : InstrT(dst),
        code_(code),
        builtins_(builtins),
        globals_(globals),
        name_idx_(name_idx) {}

  BorrowedRef<PyCodeObject> code() const {
    return code_;
  }

  BorrowedRef<PyDictObject> builtins() const {
    return builtins_;
  }

  BorrowedRef<PyDictObject> globals() const {
    return globals_;
  }

  int name_idx() const {
    return name_idx_;
  }

 private:
  BorrowedRef<PyCodeObject> code_;
  BorrowedRef<PyDictObject> builtins_;
  BorrowedRef<PyDictObject> globals_;
  int name_idx_;
};

DEFINE_SIMPLE_INSTR(
    LoadGlobal,
    (),
    HasOutput,
    Operands<0>,
    DeoptBaseWithNameIdx);

// Return a copy of the input with a refined Type. The output Type is the
// intersection of the given Type and the input's Type.
class INSTR_CLASS(RefineType, (TTop), HasOutput, Operands<1>) {
 public:
  RefineType(Register* dst, Type type, Register* src)
      : InstrT(dst, src), type_(type) {}

  Type type() const {
    return type_;
  }

 private:
  Type type_;
};

//  Return from the function
class INSTR_CLASS(Return, (), Operands<1>) {
 public:
  explicit Return(Register* val) : InstrT(val), type_(TObject) {}
  Return(Register* val, Type type) : InstrT(val), type_(type) {}

  Type type() const {
    return type_;
  }

  OperandType GetOperandTypeImpl(std::size_t /* i */) const {
    return type_;
  }

 private:
  Type type_;
};

// Should be generated whenever an optimization removes the usage of a register
// but still relies on that register being of a certain type
// (see simplifyIsTruthy)
//
// Ensures that we don't accidentally remove a type check (such as in GuardType)
// despite a register not having any explicit users
class INSTR_CLASS(UseType, (), Operands<1>) {
 public:
  UseType(Register* val, Type type) : InstrT(val), type_(type) {}

  Type type() const {
    return type_;
  }

  OperandType GetOperandTypeImpl(std::size_t /* i */) const {
    return type_;
  }

 private:
  Type type_;
};

// Assign one register to another
DEFINE_SIMPLE_INSTR(Assign, (TTop), HasOutput, Operands<1>);

// Assign one register to another with a new type (unchecked!)
class INSTR_CLASS(BitCast, (TTop), HasOutput, Operands<1>) {
 public:
  BitCast(Register* dst, Register* src, Type type)
      : InstrT(dst, src), type_(type) {}

  Type type() const {
    return type_;
  }

 private:
  Type type_;
};

// Load the value of an argument to the current function. Reads from implicit
// state set up by the function prologue and must not appear after any
// non-LoadArg instruction.
class INSTR_CLASS(LoadArg, (), HasOutput, Operands<0>) {
 public:
  LoadArg(Register* dst, uint32_t arg_idx)
      : InstrT(dst), arg_idx_(arg_idx), type_(TObject) {}

  LoadArg(Register* dst, uint32_t arg_idx, Type type)
      : InstrT(dst), arg_idx_(arg_idx), type_(type) {}

  uint32_t arg_idx() const {
    return arg_idx_;
  }

  Type type() const {
    return type_;
  }

 private:
  uint32_t arg_idx_;
  Type type_;
};

// Allocate and fill a list object with the given operands
class INSTR_CLASS(MakeList, (TObject), HasOutput, Operands<>, DeoptBase) {
 public:
  MakeList(Register* dst, const FrameState& frame) : InstrT(dst, frame) {}

  MakeList(
      Register* dst,
      const std::vector<Register*>& args,
      const FrameState& frame)
      : InstrT(dst, frame) {
    JIT_CHECK(
        NumOperands() == args.size(),
        "Cannot add {} args to instr with {} operands",
        args.size(),
        NumOperands());
    size_t i = 0;
    for (Register* arg : args) {
      SetOperand(i++, arg);
    }
  }

  size_t nvalues() const {
    return NumOperands();
  }
};

// Allocate and fill a tuple object with the given operands
class INSTR_CLASS(MakeTuple, (TObject), HasOutput, Operands<>, DeoptBase) {
 public:
  MakeTuple(Register* dst, const FrameState& frame) : InstrT(dst, frame) {}

  MakeTuple(
      Register* dst,
      const std::vector<Register*>& args,
      const FrameState& frame)
      : InstrT(dst, frame) {
    JIT_CHECK(
        NumOperands() == args.size(),
        "Cannot add {} args to instr with {} operands",
        args.size(),
        NumOperands());
    size_t i = 0;
    for (Register* arg : args) {
      SetOperand(i++, arg);
    }
  }

  size_t nvalues() const {
    return NumOperands();
  }
};

// Initialize a tuple from a list
DEFINE_SIMPLE_INSTR(
    MakeTupleFromList,
    (TList),
    HasOutput,
    Operands<1>,
    DeoptBase);

// Load an element from a tuple at a known index, with no bounds checking.
class INSTR_CLASS(LoadTupleItem, (TTuple), HasOutput, Operands<1>) {
 public:
  LoadTupleItem(Register* dst, Register* tuple, size_t idx)
      : InstrT(dst, tuple), idx_(idx) {}

  Register* tuple() const {
    return GetOperand(0);
  }

  size_t idx() const {
    return idx_;
  }

 private:
  size_t idx_;
};

// Load an element from an array at a known index and offset, with no bounds
// checking. Equivalent to ((type*)(((char*)ob_item)+offset))[idx]
class INSTR_CLASS(
    LoadArrayItem,
    (Constraint::kTupleExactOrCPtr, TCInt, TOptObject),
    HasOutput,
    Operands<3>) {
 public:
  LoadArrayItem(
      Register* dst,
      Register* ob_item,
      Register* idx,
      // This operand is never actually used, but it's an input for this because
      // we need to keep a reference to the container alive. The refcount
      // insertion pass handles this for us if the container is an input for
      // this instruction.
      Register* array_unused,
      ssize_t offset,
      Type type)
      : InstrT(dst, ob_item, idx, array_unused), offset_(offset), type_(type) {}

  Register* ob_item() const {
    return GetOperand(0);
  }

  Register* idx() const {
    return GetOperand(1);
  }

  Register* seq() const {
    return GetOperand(2);
  }

  ssize_t offset() const {
    return offset_;
  }

  Type type() const {
    return type_;
  }

 private:
  ssize_t offset_;
  Type type_;
};

// Load an item from dict->ma_values[item_idx]. Users must ensure that the
// given dict has a split table and that item_idx is the result of
// _PyDictKeys_GetSplitIndex(dict->ma_keys).
class INSTR_CLASS(LoadSplitDictItem, (TDict), HasOutput, Operands<1>) {
 public:
  LoadSplitDictItem(Register* dst, Register* dict, Py_ssize_t item_idx)
      : InstrT{dst, dict}, item_idx_{item_idx} {}

  Py_ssize_t itemIdx() const {
    return item_idx_;
  }

 private:
  Py_ssize_t item_idx_{0};
};

class INSTR_CLASS(
    LoadFieldAddress,
    (TOptObject, TCInt64),
    HasOutput,
    Operands<2>) {
 public:
  LoadFieldAddress(Register* dst, Register* object, Register* offset)
      : InstrT(dst, object, offset) {}

  Register* object() const {
    return GetOperand(0);
  }

  Register* offset() const {
    return GetOperand(1);
  }
};

// Store an element to an array at a known index, with no bounds checking.
class INSTR_CLASS(StoreArrayItem, (TCPtr, TCInt, TTop, TObject), Operands<4>) {
 public:
  StoreArrayItem(
      Register* ob_item,
      Register* idx,
      Register* value,
      // This operand is never actually used, but it's an input for this because
      // we need to keep a reference to the container alive. The refcount
      // insertion pass handles this for us if the container is an input for
      // this instruction.
      Register* container_unused,
      Type type)
      : InstrT(ob_item, idx, value, container_unused), type_(type) {}

  Register* ob_item() const {
    return GetOperand(0);
  }

  Register* idx() const {
    return GetOperand(1);
  }

  Register* value() const {
    return GetOperand(2);
  }

  Type type() const {
    return type_;
  }

 private:
  Type type_;
};

// Check whether the given index lies within the array boundary.
// Returns the actual index between [0, len(array)) into the array (in case it's
// negative). Returns -1 if the given index is not within bounds.
// Takes an array as operand 0
// Takes an idx as operand 1
DEFINE_SIMPLE_INSTR(
    CheckSequenceBounds,
    (TObject, TCInt),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Create a cell holding given value and place the cell in dst.
// Calls PyCell_New, so it implicitly increfs the value placed in the cell.
DEFINE_SIMPLE_INSTR(MakeCell, (TOptObject), HasOutput, Operands<1>, DeoptBase);

// Allocate an empty dict with the given capacity, or the default capacity if 0
// is given.
class INSTR_CLASS(MakeDict, (), HasOutput, Operands<0>, DeoptBase) {
 public:
  MakeDict(Register* dst, size_t capacity, const FrameState& frame)
      : InstrT(dst, frame), capacity_(capacity) {}

  size_t GetCapacity() const {
    return capacity_;
  }

 private:
  size_t capacity_;
};

// Allocate an empty checked dict with the given capacity, or the default
// capacity if 0 is given.
class INSTR_CLASS(MakeCheckedDict, (), HasOutput, Operands<0>, DeoptBase) {
 public:
  MakeCheckedDict(
      Register* dst,
      size_t capacity,
      Type dict_type,
      const FrameState& frame)
      : InstrT(dst, frame), capacity_(capacity), type_(dict_type) {}

  size_t GetCapacity() const {
    return capacity_;
  }

  Type type() const {
    return type_;
  }

 private:
  size_t capacity_;
  Type type_;
};

// Allocate and fill a CheckedList object with the given operands
class INSTR_CLASS(
    MakeCheckedList,
    (TObject),
    HasOutput,
    Operands<>,
    DeoptBase) {
 public:
  MakeCheckedList(Register* dst, Type list_type, const FrameState& frame)
      : InstrT(dst, frame), type_(list_type) {}

  size_t nvalues() const {
    return NumOperands();
  }

  Type type() const {
    return type_;
  }

 private:
  Type type_;
};

// Merge two maps by (ultimately) calling PyDict_Update
DEFINE_SIMPLE_INSTR(
    DictUpdate,
    (TDict, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Merge two maps by (ultimately) calling _PyDict_MergeEx
DEFINE_SIMPLE_INSTR(
    DictMerge,
    (TDict, TObject, TObject),
    HasOutput,
    Operands<3>,
    DeoptBase);

// Allocate an empty set
DEFINE_SIMPLE_INSTR(MakeSet, (), HasOutput, Operands<0>, DeoptBase);

// merge two sets by calling _PySet_Update
DEFINE_SIMPLE_INSTR(
    MergeSetUnpack,
    (TSet, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// the main step in MATCH_CLASS opcode, where match_class() is called
// takes subject as operand 0
// takes type as operand 1
// takes nargs as operand 2
// takes kwargs as operand 3
DEFINE_SIMPLE_INSTR(
    MatchClass,
    (TObject, TObject, TCUInt64, TObject),
    HasOutput,
    Operands<4>);

// Takes a dict as operand 0
// Takes a key as operand 1
// Takes a value as operand 2
DEFINE_SIMPLE_INSTR(
    SetDictItem,
    (Constraint::kDictOrChkDict, TObject, TOptObject),
    HasOutput,
    Operands<3>,
    DeoptBase);

// Takes a set as operand 0
// Takes a key as operand 1
DEFINE_SIMPLE_INSTR(
    SetSetItem,
    (TSet, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Takes a set as operand 0
// Takes an iterable as operand 1
DEFINE_SIMPLE_INSTR(
    SetUpdate,
    (TSet, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Load the size of a PyVarObject as a CInt64.
DEFINE_SIMPLE_INSTR(LoadVarObjectSize, (TOptObject), HasOutput, Operands<1>);

// Stores into an index
//
// Places NULL in dst if an error occurred or a non-NULL value otherwise
DEFINE_SIMPLE_INSTR(
    StoreSubscr,
    (TObject, TObject, TOptObject),
    Operands<3>,
    DeoptBase);

class INSTR_CLASS(
    DictSubscr,
    (TDictExact, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  DictSubscr(
      Register* dst,
      Register* left,
      Register* right,
      const FrameState& frame)
      : InstrT(dst, left, right, frame) {}
};

// Return a new iterator for the object, or return it if it's an iterator
class INSTR_CLASS(GetIter, (TObject), HasOutput, Operands<1>, DeoptBase) {
 public:
  GetIter(Register* dst, Register* iterable, const FrameState& frame)
      : InstrT(dst, iterable, frame) {}

  Register* iterable() const {
    return GetOperand(0);
  }
};

DEFINE_SIMPLE_INSTR(GetAIter, (TObject), HasOutput, Operands<1>, DeoptBase);
DEFINE_SIMPLE_INSTR(GetANext, (TObject), HasOutput, Operands<1>, DeoptBase);

// Get the length of an object by calling __len__.
DEFINE_SIMPLE_INSTR(GetLength, (TObject), HasOutput, Operands<1>, DeoptBase);

// Invoke next() on the iterator.
//
// The output is one of three values:
//
//   1. A sentinel value that indicates the iterator is exhausted.
//   2. NULL to indicate an error has occurred.
//   3. Any other value is the output of the iterator.
class INSTR_CLASS(
    InvokeIterNext,
    (TObject),
    HasOutput,
    Operands<1>,
    DeoptBase) {
 public:
  InvokeIterNext(Register* dst, Register* iter, const FrameState& frame)
      : InstrT(dst, iter, frame) {}

  Register* iterator() const {
    return GetOperand(0);
  }
};

// Returns a non-zero value if we need to release the GIL or run pending calls
// (e.g. signal handlers).  Returns 0 otherwise. This is intended to be
// followed immediately by a CondBranch.
DEFINE_SIMPLE_INSTR(LoadEvalBreaker, (), HasOutput, Operands<0>);

// Let other threads run, run signal handlers, etc.
DEFINE_SIMPLE_INSTR(RunPeriodicTasks, (), HasOutput, Operands<0>, DeoptBase);

class INSTR_CLASS(Snapshot, (), Operands<0>) {
 public:
  explicit Snapshot(const FrameState& frame_state) : InstrT() {
    setFrameState(frame_state);
  }
  Snapshot() : InstrT() {}

  // Make sure we call InstrT's copy constructor and not InstrT's Args
  // constructor
  Snapshot(const Snapshot& other) : InstrT(static_cast<const InstrT&>(other)) {
    if (FrameState* copy_fs = other.frameState()) {
      setFrameState(std::make_unique<FrameState>(*copy_fs));
    }
  }

  // Set/get the metadata needed to reconstruct the state of the interpreter
  // after this instruction executes.
  void setFrameState(std::unique_ptr<FrameState> state) {
    frame_state_ = std::move(state);
  }

  void setFrameState(const FrameState& state) {
    frame_state_ = std::make_unique<FrameState>(state);
  }

  FrameState* frameState() const {
    return frame_state_.get();
  }

  bool visitUses(const std::function<bool(Register*&)>& func) override {
    if (auto fs = frameState()) {
      return fs->visitUses(func);
    }
    return true;
  }

 private:
  std::unique_ptr<FrameState> frame_state_{nullptr};
};

// Used to indicate a control flow path that is statically known to be
// unreachable. Executing an Unreachable at runtime can only happen due
// to bugs in the compiler.
DEFINE_SIMPLE_INSTR(Unreachable, (), Operands<0>);

// Always deopt.
DEFINE_SIMPLE_INSTR(Deopt, (), Operands<0>, DeoptBase);

// A DeoptPatchpoint reserves space in the instruction stream that may be
// overwritten at runtime with a Deopt instruction.
//
// These are typically used by optimizations that want to invalidate compiled
// code at runtime when an invariant that the code depends on is violated.
//
// See the comment in Jit/deopt_patcher.h for a description of how to use
// these.
class INSTR_CLASS(DeoptPatchpoint, (), Operands<0>, DeoptBase) {
 public:
  explicit DeoptPatchpoint(JumpPatcher* patcher)
      : InstrT(), patcher_(patcher) {}

  JumpPatcher* patcher() const {
    return patcher_;
  }

 private:
  JumpPatcher* patcher_;
};

// A guard verifies that the operand is nonzero. When it's not, control is
// transferred to the interpreter at the point specified by the attached
// FrameState.
DEFINE_SIMPLE_INSTR(
    Guard,
    (Constraint::kOptObjectOrCIntOrCBool),
    Operands<1>,
    DeoptBase);

// A guard that verifies that its src is the same object as the target, or
// deopts if not.
class INSTR_CLASS(GuardIs, (TOptObject), HasOutput, Operands<1>, DeoptBase) {
 public:
  GuardIs(Register* dst, PyObject* target, Register* src)
      : InstrT(dst, src), target_(target) {}

  PyObject* target() const {
    return target_;
  }

 private:
  PyObject* target_;
};

// Return a copy of the input with a refined Type. The output Type is the
// intersection of the source's type with the target Type.
class INSTR_CLASS(GuardType, (TObject), HasOutput, Operands<1>, DeoptBase) {
 public:
  GuardType(Register* dst, Type target, Register* src)
      : InstrT(dst, src), target_(target) {}

  GuardType(Register* dst, Type target, Register* src, const FrameState& fs)
      : InstrT(dst, src, fs), target_(target) {}

  Type target() const {
    return target_;
  }

 private:
  Type target_;
};

using ProfiledTypes = std::vector<std::vector<Type>>;

// Stores all profiled types for a set of operands at a bytecode location.
//
// The top-level vector represents the different profiles seen (sorted by
// frequency), and each inner vector represents the type of each operand for
// that profile.
// Used informatively - has no output and does not compile down to LIR.
class INSTR_CLASS(HintType, (TObject), Operands<>) {
 public:
  HintType(ProfiledTypes op_types, const std::vector<Register*>& args)
      : InstrT(), types_(op_types) {
    size_t i = 0;
    for (Register* arg : args) {
      SetOperand(i++, arg);
    }
  }

  ProfiledTypes seenTypes() const {
    return types_;
  }

 private:
  ProfiledTypes types_;
};

// Output 1, 0, if `value` is truthy or not truthy.
DEFINE_SIMPLE_INSTR(IsTruthy, (TObject), HasOutput, Operands<1>, DeoptBase);

DEFINE_SIMPLE_INSTR(
    IsInstance,
    (TObject, TType),
    HasOutput,
    Operands<2>,
    DeoptBase);

class INSTR_CLASS(
    ImportFrom,
    (TObject),
    HasOutput,
    Operands<1>,
    DeoptBaseWithNameIdx) {
 public:
  ImportFrom(
      Register* dst,
      Register* module,
      int name_idx,
      const FrameState& frame)
      : InstrT(dst, module, name_idx, frame) {}

  Register* module() const {
    return GetOperand(0);
  }
};

class INSTR_CLASS(
    EagerImportName,
    (TObject, TLong),
    HasOutput,
    Operands<2>,
    DeoptBaseWithNameIdx) {
 public:
  EagerImportName(
      Register* dst,
      int name_idx,
      Register* fromlist,
      Register* level,
      const FrameState& frame)
      : InstrT(dst, fromlist, level, name_idx, frame) {}

  Register* GetFromList() const {
    return GetOperand(0);
  }

  Register* GetLevel() const {
    return GetOperand(1);
  }
};

class INSTR_CLASS(
    ImportName,
    (TObject, TLong),
    HasOutput,
    Operands<2>,
    DeoptBaseWithNameIdx) {
 public:
  ImportName(
      Register* dst,
      int name_idx,
      Register* fromlist,
      Register* level,
      const FrameState& frame)
      : InstrT(dst, fromlist, level, name_idx, frame) {}

  Register* GetFromList() const {
    return GetOperand(0);
  }

  Register* GetLevel() const {
    return GetOperand(1);
  }
};

DEFINE_SIMPLE_INSTR(Raise, (), Operands<0>, DeoptBase);

// Set an error by calling PyErr_Format() and then raising. This is typically
// used when a runtime assertion implemented as part of a Python opcode is hit.
class INSTR_CLASS(RaiseStatic, (TObject), Operands<>, DeoptBase) {
 public:
  RaiseStatic(PyObject* exc_type, const char* fmt, const FrameState& frame)
      : InstrT(frame), fmt_(fmt), exc_type_(exc_type) {
    JIT_CHECK(PyExceptionClass_Check(exc_type), "Expecting exception type");
  }

  const char* fmt() const {
    return fmt_;
  }

  PyObject* excType() const {
    return exc_type_;
  }

 private:
  const char* fmt_;
  PyObject* exc_type_;
};

DEFINE_SIMPLE_INSTR(SetCurrentAwaiter, (TOptObject), Operands<1>);

DEFINE_SIMPLE_INSTR(YieldValue, (TObject), HasOutput, Operands<1>, DeoptBase);

// InitialYield causes a generator function to suspend and return a new
// 'PyGenObject' object holding its state. This should only appear in generator
// functions and in them should be exactly one instance, which in 3.10 is
// before execution begins, and in 3.12 is generated by RETURN_GENERATOR.
DEFINE_SIMPLE_INSTR(InitialYield, (), HasOutput, Operands<0>, DeoptBase);

// Send the value in operand 0 to the subiterator in operand 1, forwarding
// yielded values from the subiterator back to our caller until it is
// exhausted.
DEFINE_SIMPLE_INSTR(
    YieldFrom,
    (TObject, TOptObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// A more compact (in terms of emitted code) equivalent to YieldValue followed
// by YieldFrom.
DEFINE_SIMPLE_INSTR(
    YieldAndYieldFrom,
    (TOptObject, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Like YieldFrom but instead of propagating StopAsyncIteration it instead
// yields the sentinel value indicating that iteration has completed. Used to
// implement `async for` loops.
DEFINE_SIMPLE_INSTR(
    YieldFromHandleStopAsyncIteration,
    (TObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Implements BUILD_STRING opcode.
DEFINE_SIMPLE_INSTR(BuildString, (TUnicode), HasOutput, Operands<>, DeoptBase);

// Implements FORMAT_VALUE opcode, which handles f-string value formatting.
class INSTR_CLASS(
    FormatValue,
    (TOptUnicode, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase) {
 public:
  FormatValue(
      Register* dst,
      Register* fmt_spec,
      Register* value,
      int conversion,
      const FrameState& frame)
      : InstrT(dst, fmt_spec, value, frame), conversion_(conversion) {}
  int conversion() const {
    return conversion_;
  }

 private:
  int conversion_;
};

// Implements FORMAT_WITH_SPEC opcode, which handles f-string value formatting
// with spec.
DEFINE_SIMPLE_INSTR(
    FormatWithSpec,
    (TObject, TOptObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

// Implements `del container[sub]`
// Takes a container as operand 0
// Takes a sub as operand 1
DEFINE_SIMPLE_INSTR(DeleteSubscr, (TObject, TObject), Operands<2>, DeoptBase);

// Unpack a sequence as UNPACK_EX opcode and save the results
// to a tuple
class INSTR_CLASS(
    UnpackExToTuple,
    (TObject),
    HasOutput,
    Operands<1>,
    DeoptBase) {
 public:
  UnpackExToTuple(
      Register* dst,
      Register* seq,
      int before,
      int after,
      const FrameState& frame)
      : InstrT(dst, seq, frame), before_(before), after_(after) {}

  Register* seq() const {
    return GetOperand(0);
  }

  int before() const {
    return before_;
  }
  int after() const {
    return after_;
  }

 private:
  int before_;
  int after_;
};

DEFINE_SIMPLE_INSTR(
    WaitHandleLoadCoroOrResult,
    (TObject),
    HasOutput,
    Operands<1>);
DEFINE_SIMPLE_INSTR(WaitHandleLoadWaiter, (TObject), HasOutput, Operands<1>);
DEFINE_SIMPLE_INSTR(WaitHandleRelease, (TObject), Operands<1>);

// MatchKeys calls CPython's match_keys interpreter function. It takes two
// arguments, subject and keys. Returns null on error, None if no match, and a
// tuple of values on match.
DEFINE_SIMPLE_INSTR(
    MatchKeys,
    (TObject, TObject),
    HasOutput,
    Operands<2>,
    DeoptBase);

class INSTR_CLASS(UpdatePrevInstr, (), Operands<0>) {
 public:
  explicit UpdatePrevInstr(int line_no, BeginInlinedFunction* parent)
      : line_no_(line_no), parent_(parent) {}

  int lineNo() const {
    return line_no_;
  }

  // The inlined function which this update belongs to or nullptr if not in an
  // inlined function.
  BeginInlinedFunction* parent() const {
    return parent_;
  }

 private:
  int line_no_;
  BeginInlinedFunction* parent_;
};

DEFINE_SIMPLE_INSTR(
    Send,
    (TObject, TObject),
    Operands<2>,
    HasOutput,
    DeoptBase);

class INSTR_CLASS(
    BuildInterpolation,
    (TObject, TObject, TObject),
    HasOutput,
    Operands<3>,
    DeoptBase) {
 public:
  BuildInterpolation(
      Register* dst,
      Register* value,
      Register* str,
      Register* format,
      int conversion,
      const FrameState& frame)
      : InstrT(dst, value, str, format, frame), conversion_(conversion) {}

  int conversion() const {
    return conversion_;
  }

 private:
  int conversion_;
};

DEFINE_SIMPLE_INSTR(
    BuildTemplate,
    (TObject, TObject),
    Operands<2>,
    HasOutput,
    DeoptBase);

class INSTR_CLASS(ConvertValue, (TObject), HasOutput, Operands<1>, DeoptBase) {
 public:
  ConvertValue(
      Register* dst,
      Register* value,
      int converter_idx,
      const FrameState& frame)
      : InstrT(dst, value, frame), converter_idx_(converter_idx) {}

  int converterIdx() const {
    return converter_idx_;
  }

 private:
  int converter_idx_;
};

class INSTR_CLASS(LoadSpecial, (TObject), HasOutput, Operands<1>, DeoptBase) {
 public:
  LoadSpecial(
      Register* method_and_self_o,
      Register* self,
      int special_idx,
      const FrameState& frame)
      : InstrT(method_and_self_o, self, frame), special_idx_(special_idx) {}

  int specialIdx() const {
    return special_idx_;
  }

 private:
  int special_idx_;
};

DEFINE_SIMPLE_INSTR(CIntToCBool, (TCInt64), HasOutput, Operands<1>);

// Return true if the given instruction returns an exact copy of its input "at
// runtime" (most passthrough instructions will be copy-propagated away in
// LIR). The output differs only in some HIR-level property that is erased in
// the generated code, usually its Type.
//
// This is used by modelReg() and optimizations that want to treat all
// HIR-level copies of a value as one combined entity (see the 'Value copies'
// section of Jit/hir/refcount_insertion.md for a concrete example).
bool isPassthrough(const Instr& instr);

// Trace through any passthrough instructions in the definition chain of the
// given value, returning the original source of the value.
Register* modelReg(Register* reg);

class BasicBlock {
 public:
  BasicBlock() : BasicBlock(0) {}
  explicit BasicBlock(int id_) : id(id_) {}
  ~BasicBlock();

  // Replace any references to old_pred in this block's Phis with new_pred.
  void fixupPhis(BasicBlock* old_pred, BasicBlock* new_pred);
  // Adds a new predecessor to the phi that follows from the old predecessor
  void addPhiPredecessor(BasicBlock* old_pred, BasicBlock* new_pred);
  // Removes any references to old_pred in this block's Phis
  void removePhiPredecessor(BasicBlock* old_pred);

  // Read-only access to the incoming and outgoing edges.
  const std::unordered_set<const Edge*>& in_edges() const {
    return in_edges_;
  }
  const std::unordered_set<const Edge*>& out_edges() const {
    return out_edges_;
  }

  // Append or prepend an instruction to the instructions in the basic block.
  //
  // NB: The block takes ownership of the insruction and frees it when the block
  //     is deleted.
  Instr* Append(Instr* instr);
  void push_front(Instr* instr);
  Instr* pop_front();

  // Insert the given Instr before `it'.
  void insert(Instr* instr, Instr::List::iterator it);

  template <typename T, typename... Args>
  T* append(Args&&... args) {
    T* instr = T::create(std::forward<Args>(args)...);
    Append(instr);
    return instr;
  }

  template <typename T, typename... Args>
  T* appendWithOff(BCOffset bc_off, Args&&... args) {
    auto instr = append<T>(std::forward<Args>(args)...);
    instr->setBytecodeOffset(bc_off);
    return instr;
  }

  template <typename T, typename... Args>
  T* push_front(Args&&... args) {
    T* instr = T::create(std::forward<Args>(args)...);
    push_front(instr);
    return instr;
  }

  void retargetPreds(BasicBlock* target);

  BasicBlock* successor(std::size_t i) const {
    return GetTerminator()->successor(i);
  }

  void set_successor(std::size_t i, BasicBlock* succ) {
    GetTerminator()->set_successor(i, succ);
  }

  // Remove and delete all contained instructions, leaving the block empty.
  void clear();

  // BasicBlock holds a list of instructions, delegating most operations
  // directly to its IntrusiveList.
  auto empty() const {
    return instrs_.IsEmpty();
  }
  auto& front() {
    return instrs_.Front();
  }
  auto& front() const {
    return instrs_.Front();
  }
  auto& back() {
    return instrs_.Back();
  }
  auto& back() const {
    return instrs_.Back();
  }
  auto iterator_to(Instr& instr) {
    return instrs_.iterator_to(instr);
  }
  auto const_iterator_to(const Instr& instr) const {
    return instrs_.const_iterator_to(instr);
  }
  auto begin() {
    return instrs_.begin();
  }
  auto begin() const {
    return instrs_.begin();
  }
  auto end() {
    return instrs_.end();
  }
  auto end() const {
    return instrs_.end();
  }
  auto reverse_iterator_to(Instr& instr) {
    return instrs_.reverse_iterator_to(instr);
  }
  auto const_reverse_iterator_to(const Instr& instr) const {
    return instrs_.const_reverse_iterator_to(instr);
  }
  auto rbegin() {
    return instrs_.rbegin();
  }
  auto rbegin() const {
    return instrs_.rbegin();
  }
  auto rend() {
    return instrs_.rend();
  }
  auto rend() const {
    return instrs_.rend();
  }
  auto crend() const {
    return instrs_.crend();
  }

  // Return the snapshot on entry to this block
  Snapshot* entrySnapshot();

  // Return the last instruction in the block
  Instr* GetTerminator();
  const Instr* GetTerminator() const {
    return const_cast<BasicBlock*>(this)->GetTerminator();
  }

  // A trampoline block consists of a single direct jump to another block
  bool IsTrampoline();

  // Call f with each Phi instruction at the beginning of this block.
  template <typename F>
  void forEachPhi(F f) {
    for (auto& instr : *this) {
      if (!instr.IsPhi()) {
        break;
      }
      f(static_cast<Phi&>(instr));
    }
  }

  int id;

  // Basic blocks belong to a list of all blocks in their CFG
  IntrusiveListNode cfg_node;

 private:
  DISALLOW_COPY_AND_ASSIGN(BasicBlock);

  friend class Edge;

  // Instructions for this basic block.
  //
  // The last instruction is guaranteed to be a terminator, which must be one
  // of:
  //
  // - Branch
  // - CondBranch
  // - Return
  //
  Instr::List instrs_;

  // Outgoing edges.
  std::unordered_set<const Edge*> out_edges_;

  // Incoming edges.
  std::unordered_set<const Edge*> in_edges_;
};

class Environment {
 public:
  using RegisterMap = std::unordered_map<int, std::unique_ptr<Register>>;
  using ReferenceSet = std::unordered_set<ThreadedRef<>>;

  Environment() = default;
  ~Environment();

  Register* AllocateRegister();

  const RegisterMap& GetRegisters() const;

  // Only intended to be used in tests and parsing code.
  Register* addRegister(std::unique_ptr<Register> reg);

  // Only intended to be used in tests and parsing code. Ensure that this
  // Environment owns a reference to the given borrowed object, keeping it
  // alive for use by the compiled code. Make Environment a new owner of the
  // object.
  BorrowedRef<> addReference(BorrowedRef<> obj);
  BorrowedRef<> addReference(Ref<> obj);

  const ReferenceSet& references() const;

  // Returns nullptr if a register with the given `id` isn't found
  Register* getRegister(int id);

  int nextRegisterId() const {
    return next_register_id_;
  }

  void setNextRegisterId(int id) {
    next_register_id_ = id;
  }

  int allocateLoadTypeAttrCache() {
    return next_load_type_attr_cache_++;
  }

  int numLoadTypeAttrCaches() const {
    return next_load_type_attr_cache_;
  }

  int allocateLoadTypeMethodCache() {
    return next_load_type_method_cache_++;
  }

  int numLoadTypeMethodCaches() const {
    return next_load_type_method_cache_;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(Environment);

  RegisterMap registers_;
  ReferenceSet references_;
  int next_register_id_{0};
  int next_load_type_attr_cache_{0};
  int next_load_type_method_cache_{0};
};

constexpr unsigned long kThreadSafeFlagsMask = Py_TPFLAGS_BASETYPE;

struct TypedArgument {
  TypedArgument(
      long locals_idx,
      BorrowedRef<PyTypeObject> pytype,
      int optional,
      int exact,
      Type jit_type);
  ~TypedArgument();

  TypedArgument(const TypedArgument& other);
  TypedArgument& operator=(const TypedArgument& other);

  // Returns type flags which should not change between concurrent compilation
  // threads.
  unsigned long threadSafeTpFlags() const;

  long locals_idx;
  Ref<PyTypeObject> pytype;
  int optional;
  int exact;
  Type jit_type;
  unsigned long thread_safe_flags;
};

// Does the given code object need access to its containing PyFunctionObject at
// runtime?
bool usesRuntimeFunc(BorrowedRef<PyCodeObject> code);

#define FOREACH_FAILURE_TYPE(V)                                            \
  V(HasDefaults, "it has defaults")                                        \
  V(HasKwdefaults, "it has kwdefaults")                                    \
  V(HasKwOnlyArgs, "it has keyword-only args")                             \
  V(HasVarargs, "it has varargs")                                          \
  V(HasVarkwargs, "it has varkwargs")                                      \
  V(CalledWithMismatchedArgs, "it is called with mismatched arguments")    \
  V(IsGenerator, "it is a generator")                                      \
  V(HasCellvars, "it has cellvars")                                        \
  V(HasFreevars, "it has freevars")                                        \
  V(NeedsRuntimeAccess, "it needs runtime access to its PyFunctionObject") \
  V(NeedsPreload, "the function is not preloaded")                         \
  V(IsVectorCallWithPrimitives,                                            \
    "it is a vectorcalled static function with pimitive args")             \
  V(GlobalsNotDict, "globals is not a dict")                               \
  V(BuiltinsNotDict, "builtins is not a dict")                             \
  V(HasEagerImportName, "has an eager import name instruction")

enum class InlineFailureType {
#define DECLARE_FAILURE_TYPE(failure, msg) k##failure,
  FOREACH_FAILURE_TYPE(DECLARE_FAILURE_TYPE)
#undef DECLARE_FAILURE_TYPE
};

const char* getInlineFailureMessage(InlineFailureType failure_type);
const char* getInlineFailureName(InlineFailureType failure_type);

FrameState* get_frame_state(Instr& instr);
const FrameState* get_frame_state(const Instr& instr);

} // namespace jit::hir

template <>
struct fmt::formatter<jit::hir::OperandType> : fmt::ostream_formatter {};

template <>
struct fmt::formatter<jit::hir::CallCFunc::Func> : fmt::ostream_formatter {};

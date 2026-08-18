// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/lir/block.h"

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <bit>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace cinderx::jit::lir {

// Convert an HIR type into an LIR type.
DataType hirTypeToDataType(hir::Type tp);

class BasicBlockBuilder {
 public:
  BasicBlockBuilder(jit::codegen::Environ* env, Function* func);

  void setCurrentInstr(const hir::Instr* inst);
  const hir::Instr* currentInstr() const {
    return cur_hir_instr_;
  }

  // Return the id of a DeoptMetadata for the current instruction, returning
  // the same id if called multiple times for the same instruction.
  std::size_t makeDeoptMetadata();

  // Allocate a new block, not yet attached anywhere in the current CFG.
  BasicBlock* allocateBlock();

  // Append a block to the CFG and switch to it.
  void appendBlock(BasicBlock* block);

  // Terminate the current block and switch over to a new one.
  //
  // Any predecessor/successor links are expected to be set up already.
  void switchBlock(BasicBlock* block);

  // Set an annotation that will be applied to the next instruction appended.
  void annotateNext(std::string text) {
    cur_bb_->pending_annotation_ = std::move(text);
  }

  // Allocate and append a new instruction to the instruction stream.
  template <class... Args>
  Instruction* appendInstr(Opcode opcode, Args&&... args) {
    auto instr = cur_bb_->allocateInstr(opcode, cur_hir_instr_);
    (genericCreateInstrInput(instr, args), ...);
    return instr;
  }

  // Allocate and append a new instruction to the instruction stream.
  //
  // The instruction is expecting to produce a VReg and match it to an HIR
  // register.
  template <class... Args>
  Instruction* appendInstr(hir::Register* dest, Opcode opcode, Args&&... args) {
    auto dest_lir = OutVReg{hirTypeToDataType(dest->type())};
    auto instr = appendInstr(opcode, dest_lir, std::forward<Args>(args)...);
    auto [it, inserted] = env_->output_map.emplace(dest, instr);
    JIT_CHECK(inserted, "HIR value '{}' defined twice in LIR", *dest);
    return instr;
  }

  // Allocate and append a new instruction to the instruction stream.
  template <class... Args>
  Instruction* appendInstr(OutInd dest, Opcode opcode, Args&&... args) {
    auto instr = appendInstr(opcode, std::forward<Args>(args)...);
    instr->output()->setMemoryIndirect(
        dest.base, dest.index, dest.multiplier, dest.offset);
    instr->output()->setDataType(dest.data_type);
    return instr;
  }

  // Allocate and append a new instruction to the instruction stream.
  //
  // The instruction is expecting to produce a VReg and match it to an HIR
  // register.
  template <class... Args>
  Instruction* appendInstr(OutMemImm dest, Opcode opcode, Args&&... args) {
    auto instr = appendInstr(opcode, std::forward<Args>(args)...);
    instr->output()->setMemoryAddress(dest.value);
    return instr;
  }

  // Allocate and append a new instruction to the instruction stream.
  //
  // The instruction is expecting to produce a VReg and match it to an HIR
  // register.
  template <class... Args>
  Instruction* appendInstr(OutVReg dest, Opcode opcode, Args&&... args) {
    auto instr = appendInstr(opcode, std::forward<Args>(args)...);
    instr->output()->setVirtualRegister();
    instr->output()->setDataType(dest.data_type);
    return instr;
  }

  // Allocate and append a new instruction to the instruction stream.
  //
  // The instruction is expecting to produce a VReg and match it to an HIR
  // register.
  template <class... Args>
  Instruction* appendInstr(OutPhyReg dest, Opcode opcode, Args&&... args) {
    auto instr = appendInstr(opcode, std::forward<Args>(args)...);
    instr->output()->setPhyRegister(dest.value);
    instr->output()->setDataType(dest.data_type);
    return instr;
  }

  // Allocate and append a new branching instruction to the instruction stream.
  template <class Arg>
  Instruction* appendBranch(
      Opcode opcode,
      Arg&& arg,
      BasicBlock* true_bb,
      BasicBlock* false_bb) {
    auto instr = appendInstr(opcode, std::forward<Arg>(arg));
    cur_bb_->addSuccessor(true_bb);
    cur_bb_->addSuccessor(false_bb);
    return instr;
  }

  // Allocate and append a new branching instruction which is checking a flag
  template <class... Args>
  Instruction*
  appendBranch(Opcode opcode, BasicBlock* true_bb, Args&&... args) {
    auto instr = appendInstr(opcode, std::forward<Args>(args)...);
    cur_bb_->addSuccessor(true_bb);
    return instr;
  }

  template <
      typename FuncReturnType,
      typename... FuncArgs,
      typename... AppendArgs>
  Instruction* appendCallInstruction(
      hir::Register* dst,
      FuncReturnType (*func)(FuncArgs...),
      AppendArgs&&... args) {
    static_assert(
        !std::is_void_v<FuncReturnType>,
        "appendCallInstruction cannot be used with functions that return "
        "void.");
    auto instr =
        appendCallInstructionInternal(func, std::forward<AppendArgs>(args)...);
    createInstrOutput(instr, dst);
    return instr;
  }

  template <
      typename FuncReturnType,
      typename... FuncArgs,
      typename... AppendArgs>
  Instruction* appendCallInstruction(
      OutVReg dst,
      FuncReturnType (*func)(FuncArgs...),
      AppendArgs&&... args) {
    static_assert(
        !std::is_void_v<FuncReturnType>,
        "appendCallInstruction cannot be used with functions that return "
        "void.");
    auto instr =
        appendCallInstructionInternal(func, std::forward<AppendArgs>(args)...);
    instr->addOperands(dst);
    return instr;
  }

  template <
      typename FuncReturnType,
      typename... FuncArgs,
      typename... AppendArgs>
  Instruction* appendInvokeInstruction(
      FuncReturnType (*func)(FuncArgs...),
      AppendArgs&&... args) {
    static_assert(
        std::is_void_v<FuncReturnType>,
        "appendInvokeInstruction can only be used with functions that return "
        "void.");
    return appendCallInstructionInternal(
        func, std::forward<AppendArgs>(args)...);
  }

  // Create a new LIR instruction for the current HIR instruction.
  Instruction* createInstr(Opcode opcode);

  Instruction* getDefInstr(const hir::Register* reg);

  void createInstrInput(Instruction* instr, hir::Register* reg);
  void createInstrOutput(Instruction* instr, hir::Register* dst);

  std::vector<BasicBlock*> generate();

  BasicBlock* curBlock() const {
    return cur_bb_;
  }

 private:
  const hir::Instr* cur_hir_instr_{nullptr};
  std::optional<std::size_t> cur_deopt_metadata_;
  BasicBlock* cur_bb_{nullptr};
  std::vector<BasicBlock*> bbs_;
  jit::codegen::Environ* env_;
  Function* func_;

  template <
      typename FuncReturnType,
      typename... FuncArgs,
      typename... AppendArgs>
  Instruction* appendCallInstructionInternal(
      FuncReturnType (*func)(FuncArgs...),
      AppendArgs&&... args) {
    static_assert(
        sizeof...(FuncArgs) == sizeof...(AppendArgs),
        "The number of parameters the function accepts and the number of "
        "arguments passed is different.");

    auto instr = createInstr(Opcode::kCall);
    genericCreateInstrInput(instr, func);

    // Avoid expanding mismatched packs when the arity check fails. Otherwise,
    // the compiler produces additional errors that obscure the static_assert
    // above.
    if constexpr (sizeof...(FuncArgs) == sizeof...(AppendArgs)) {
      (appendCallInstructionArgument<FuncArgs>(instr, args), ...);
    }

    return instr;
  }

  template <typename ExpectedArgType, typename Arg>
  void appendCallInstructionArgument(Instruction* instr, const Arg& arg) {
    using ActualArgType = std::remove_cv_t<std::remove_reference_t<Arg>>;

    if constexpr (std::is_same_v<ExpectedArgType, PyThreadState*>) {
      JIT_CHECK(
          arg == env_->asm_tstate,
          "The thread state was passed as a different value than "
          "env_->asm_tstate");
    } else if constexpr (
        std::is_same_v<ActualArgType, hir::Register*> ||
        std::is_same_v<ActualArgType, std::string>) {
      // Could add a runtime check here to ensure the type of the register is
      // correct, at least for non-temp-register args, but not doing that
      // currently.
    } else if constexpr (std::is_same_v<ActualArgType, Instruction*>) {
    } else if constexpr (std::is_pointer_v<ExpectedArgType>) {
      if constexpr (std::is_function_v<ActualArgType>) {
        // A bare function is passed by reference. ActualArgType has the
        // reference removed, so compare it with the type pointed to by
        // ExpectedArgType.
        static_assert(
            std::is_same_v<
                ActualArgType,
                std::remove_pointer_t<ExpectedArgType>>,
            "Mismatched function pointer parameter types!");
      } else if constexpr (!std::is_same_v<ActualArgType, std::nullptr_t>) {
        static_assert(
            std::is_same_v<ActualArgType, ExpectedArgType>,
            "Mismatched function parameter types!");
      }
    } else {
      static_assert(
          std::is_same_v<ActualArgType, ExpectedArgType>,
          "Mismatched function parameter types!");
    }

    genericCreateInstrInput(instr, arg);
  }

  bool usesImmediateInput(hir::Type const& tp);

  void createRegisterInput(Instruction* instr, hir::Register* val);

  template <typename T>
  void genericCreateInstrInput(Instruction* instr, const T& val) {
    using CurArgType = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<CurArgType, hir::Register*>) {
      if (val == nullptr) {
        instr->allocateImmediateInput(
            static_cast<uint64_t>(0), DataType::k64bit);
      } else {
        createRegisterInput(instr, val);
      }
    } else if constexpr (std::is_same_v<CurArgType, Instruction*>) {
      instr->allocateLinkedInput(val);
    } else if constexpr (
        std::is_pointer_v<CurArgType> || std::is_function_v<CurArgType>) {
      instr->allocateImmediateInput(
          reinterpret_cast<uint64_t>(val), DataType::kObject);
    } else if constexpr (std::is_same_v<CurArgType, std::nullptr_t>) {
      instr->allocateImmediateInput(
          static_cast<uint64_t>(0), DataType::kObject);
    } else if constexpr (std::is_same_v<CurArgType, bool>) {
      instr->allocateImmediateInput(val ? 1 : 0, DataType::k8bit);
    } else if constexpr (std::is_floating_point_v<CurArgType>) {
      instr->allocateImmediateInput(
          std::bit_cast<uint64_t>(val), DataType::kDouble);
    } else if constexpr (std::is_integral_v<CurArgType>) {
      if constexpr (sizeof(CurArgType) == 1) {
        instr->allocateImmediateInput(
            static_cast<uint64_t>(val), DataType::k8bit);
      } else if constexpr (sizeof(CurArgType) == 2) {
        instr->allocateImmediateInput(
            static_cast<uint64_t>(val), DataType::k16bit);
      } else if constexpr (sizeof(CurArgType) == 4) {
        instr->allocateImmediateInput(
            static_cast<uint64_t>(val), DataType::k32bit);
      } else if constexpr (sizeof(CurArgType) == 8) {
        instr->allocateImmediateInput(
            static_cast<uint64_t>(val), DataType::k64bit);
      } else {
        static_assert(!std::is_same_v<T, T>, "Unknown integral size!");
      }
    } else {
      instr->addOperands(val);
    }
  }
};

} // namespace cinderx::jit::lir

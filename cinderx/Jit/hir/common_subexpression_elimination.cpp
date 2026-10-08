// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/common_subexpression_elimination.h"

#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/dominance.h"
#include "cinderx/Jit/hir/instr_effects.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace cinderx::jit::hir {

namespace {

// Identity of a pure value computation: the opcode plus the values it reads
// plus the immediates that specialize it. Types and small enums cover every
// supported opcode; a new field is needed if an opcode requires anything else.
// Memory loads are deliberately unsupported: they would also need store
// invalidation driven by AliasClass.
struct ValueKey {
  Opcode opcode;
  std::vector<Register*> operands;
  std::vector<Type> types;
  std::vector<uint64_t> ints;

  bool operator==(const ValueKey& other) const = default;
};

struct ValueKeyHash {
  std::size_t operator()(const ValueKey& key) const {
    std::size_t h = std::hash<int>{}(static_cast<int>(key.opcode));
    for (Register* reg : key.operands) {
      h = cinderx::combineHash(h, std::hash<Register*>{}(reg));
    }
    for (Type type : key.types) {
      h = cinderx::combineHash(h, std::hash<Type>{}(type));
    }
    for (uint64_t i : key.ints) {
      h = cinderx::combineHash(h, std::hash<uint64_t>{}(i));
    }
    return h;
  }
};

// The key names values rather than registers: chasing copies lets an
// instruction match a value that was itself just deduplicated, so chains of
// redundancies collapse in a single pass.
std::vector<Register*> keyOperands(std::initializer_list<Register*> regs) {
  std::vector<Register*> result;
  for (Register* reg : regs) {
    result.push_back(chaseAssignOperand(reg));
  }
  return result;
}

// Order the operands of a commutative operation by register id so that `a op b`
// and `b op a` produce the same key.
std::vector<Register*> commutativeKeyOperands(Register* left, Register* right) {
  std::vector<Register*> result = keyOperands({left, right});
  if (result[1]->id() < result[0]->id()) {
    std::swap(result[0], result[1]);
  }
  return result;
}

bool isCommutative(BinaryOpKind op) {
  switch (op) {
    case BinaryOpKind::kAdd:
    case BinaryOpKind::kAnd:
    case BinaryOpKind::kMultiply:
    case BinaryOpKind::kOr:
    case BinaryOpKind::kXor:
      return true;
    default:
      return false;
  }
}

std::vector<Register*>
binaryKeyOperands(BinaryOpKind op, Register* left, Register* right) {
  return isCommutative(op) ? commutativeKeyOperands(left, right)
                           : keyOperands({left, right});
}

// Get the op that gives the same result when the operands are swapped, e.g.
// `a < b` is `b > a`.
PrimitiveCompareOp swappedCompareOp(PrimitiveCompareOp op) {
  switch (op) {
    case PrimitiveCompareOp::kEqual:
    case PrimitiveCompareOp::kNotEqual:
      return op;
    case PrimitiveCompareOp::kLessThan:
      return PrimitiveCompareOp::kGreaterThan;
    case PrimitiveCompareOp::kLessThanEqual:
      return PrimitiveCompareOp::kGreaterThanEqual;
    case PrimitiveCompareOp::kGreaterThan:
      return PrimitiveCompareOp::kLessThan;
    case PrimitiveCompareOp::kGreaterThanEqual:
      return PrimitiveCompareOp::kLessThanEqual;
    case PrimitiveCompareOp::kLessThanUnsigned:
      return PrimitiveCompareOp::kGreaterThanUnsigned;
    case PrimitiveCompareOp::kLessThanEqualUnsigned:
      return PrimitiveCompareOp::kGreaterThanEqualUnsigned;
    case PrimitiveCompareOp::kGreaterThanUnsigned:
      return PrimitiveCompareOp::kLessThanUnsigned;
    case PrimitiveCompareOp::kGreaterThanEqualUnsigned:
      return PrimitiveCompareOp::kLessThanEqualUnsigned;
  }
  JIT_ABORT("Unknown PrimitiveCompareOp {}", static_cast<int>(op));
}

// Order comparison operands by register id, flipping the op when the operands
// are swapped so that `a > b` and `b < a` produce the same key.
std::pair<std::vector<Register*>, PrimitiveCompareOp>
compareKeyOperands(PrimitiveCompareOp op, Register* left, Register* right) {
  std::vector<Register*> result = keyOperands({left, right});
  if (result[1]->id() < result[0]->id()) {
    std::swap(result[0], result[1]);
    op = swappedCompareOp(op);
  }
  return {result, op};
}

uint64_t keyInt(auto value) {
  return static_cast<uint64_t>(value);
}

// Return the value identity of instr, or nullopt if its opcode is not
// supported. Only opcodes with well-defined effects may be added here:
// memoryEffects() aborts on control-flow opcodes like Phi.
std::optional<ValueKey> valueKeyImpl(const Instr& instr) {
  switch (instr.opcode()) {
    case Opcode::kBitCast: {
      const auto& bitcast = static_cast<const BitCast&>(instr);
      return ValueKey{
          instr.opcode(),
          keyOperands({bitcast.getOperand(0)}),
          {bitcast.type()},
          {}};
    }
    case Opcode::kCIntToCBool: {
      return ValueKey{
          instr.opcode(), keyOperands({instr.getOperand(0)}), {}, {}};
    }
    case Opcode::kDoubleBinaryOp: {
      const auto& binop = static_cast<const DoubleBinaryOp&>(instr);
      return ValueKey{
          instr.opcode(),
          binaryKeyOperands(binop.op(), binop.left(), binop.right()),
          {},
          {keyInt(binop.op())}};
    }
    case Opcode::kIntBinaryOp: {
      const auto& binop = static_cast<const IntBinaryOp&>(instr);
      return ValueKey{
          instr.opcode(),
          binaryKeyOperands(binop.op(), binop.left(), binop.right()),
          {},
          {keyInt(binop.op())}};
    }
    case Opcode::kIsCompactLong: {
      return ValueKey{
          instr.opcode(), keyOperands({instr.getOperand(0)}), {}, {}};
    }
    case Opcode::kLoadConst: {
      const auto& load = static_cast<const LoadConst&>(instr);
      return ValueKey{instr.opcode(), {}, {load.type()}, {}};
    }
    case Opcode::kPrimitiveBoxBool: {
      return ValueKey{
          instr.opcode(), keyOperands({instr.getOperand(0)}), {}, {}};
    }
    case Opcode::kPrimitiveCompare: {
      const auto& compare = static_cast<const PrimitiveCompare&>(instr);
      auto [operands, canonicalOp] =
          compareKeyOperands(compare.op(), compare.left(), compare.right());
      return ValueKey{
          instr.opcode(), std::move(operands), {}, {keyInt(canonicalOp)}};
    }
    case Opcode::kPrimitiveConvert: {
      const auto& convert = static_cast<const PrimitiveConvert&>(instr);
      return ValueKey{
          instr.opcode(), keyOperands({convert.src()}), {convert.type()}, {}};
    }
    case Opcode::kPrimitiveUnaryOp: {
      const auto& unary = static_cast<const PrimitiveUnaryOp&>(instr);
      return ValueKey{
          instr.opcode(),
          keyOperands({unary.value()}),
          {},
          {keyInt(unary.op())}};
    }
    case Opcode::kPrimitiveUnbox: {
      const auto& unbox = static_cast<const PrimitiveUnbox&>(instr);
      return ValueKey{
          instr.opcode(), keyOperands({unbox.value()}), {unbox.type()}, {}};
    }
    case Opcode::kRefineType: {
      const auto& refine = static_cast<const RefineType&>(instr);
      return ValueKey{
          instr.opcode(),
          keyOperands({refine.getOperand(0)}),
          {refine.type()},
          {}};
    }
    case Opcode::kUnicodeEqual: {
      const auto& equal = static_cast<const UnicodeEqual&>(instr);
      return ValueKey{
          instr.opcode(),
          commutativeKeyOperands(equal.left(), equal.right()),
          {},
          {}};
    }
    default:
      return std::nullopt;
  }
}

// Return the value identity of instr, or nullopt if it is not a supported
// pure computation. The effects checks are a safety net for the switch:
// silently skipping a misclassified opcode turns it into a missed
// optimization rather than a miscompile.
std::optional<ValueKey> valueKey(const Instr& instr) {
  if (instr.output() == nullptr) {
    return std::nullopt;
  }
  std::optional<ValueKey> key = valueKeyImpl(instr);
  if (!key.has_value()) {
    return std::nullopt;
  }
  JIT_DCHECK(
      instr.asDeoptBase() == nullptr && !hasArbitraryExecution(instr) &&
          memoryEffects(instr).may_store == AEmpty &&
          memoryEffects(instr).borrow_support == AEmpty,
      "bad instruction allowed in CSE");
  return key;
}

} // namespace

bool CommonSubexpressionElimination::run(Function& irfunc) {
  const DominatorTree& dom = irfunc.domTree();
  std::unordered_map<ValueKey, Register*, ValueKeyHash> table;
  std::vector<std::unique_ptr<Instr>> removed;

  // Rewrite the redundant computations of one block, recording the keys this
  // block contributes to the table.
  auto process = [&](BasicBlock* block, std::vector<ValueKey>& pushed) {
    for (auto it = block->begin(); it != block->end();) {
      Instr& instr = *it;
      ++it;
      std::optional<ValueKey> key = valueKey(instr);
      if (!key.has_value()) {
        continue;
      }
      auto found = table.find(*key);
      if (found != table.end()) {
        auto assign = Assign::create(instr.output(), found->second);
        assign->copyBytecodeOffset(instr);
        instr.replaceWith(*assign);
        removed.emplace_back(&instr);
      } else {
        pushed.push_back(*key);
        table.emplace(std::move(*key), instr.output());
      }
    }
  };

  // Walk the dominator tree with the value table scoped to the current
  // dominance chain, so every reuse is dominated by the computation it
  // reuses. Blocks unreachable from the entry are skipped; they hold dead
  // code that later passes remove.
  struct Frame {
    BasicBlock* block;
    size_t next_child{0};
    std::vector<ValueKey> pushed{};
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{irfunc.cfg.entry_block});
  process(stack.back().block, stack.back().pushed);
  while (!stack.empty()) {
    size_t idx = stack.size() - 1;
    const std::vector<BasicBlock*>& children = dom.children(stack[idx].block);
    if (stack[idx].next_child < children.size()) {
      BasicBlock* child = children[stack[idx].next_child++];
      stack.push_back(Frame{child});
      process(stack.back().block, stack.back().pushed);
    } else {
      for (const ValueKey& key : stack[idx].pushed) {
        table.erase(key);
      }
      stack.pop_back();
    }
  }

  if (removed.empty()) {
    return false;
  }
  CopyPropagation{}.run(irfunc);
  irfunc.invalidateDomTree();
  return true;
}

} // namespace cinderx::jit::hir

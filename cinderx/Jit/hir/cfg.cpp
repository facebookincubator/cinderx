// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/cfg.h"

namespace jit::hir {

namespace {

void postorder_traverse(
    BasicBlock* block,
    std::vector<BasicBlock*>* traversal,
    std::unordered_set<BasicBlock*>* visited) {
  JIT_CHECK(block != nullptr, "visiting null block!");
  visited->emplace(block);

  // Add successors to be visited
  Instr* instr = block->GetTerminator();
  switch (instr->opcode()) {
    case Opcode::kCondBranch:
    case Opcode::kCondBranchIterNotDone:
    case Opcode::kCondBranchCheckType: {
      auto cbr = static_cast<CondBranch*>(instr);
      if (!visited->contains(cbr->false_bb())) {
        postorder_traverse(cbr->false_bb(), traversal, visited);
      }
      if (!visited->contains(cbr->true_bb())) {
        postorder_traverse(cbr->true_bb(), traversal, visited);
      }
      break;
    }
    case Opcode::kBranch: {
      auto br = static_cast<Branch*>(instr);
      if (!visited->contains(br->target())) {
        postorder_traverse(br->target(), traversal, visited);
      }
      break;
    }
    case Opcode::kDeopt:
    case Opcode::kRaise:
    case Opcode::kRaiseAwaitableError:
    case Opcode::kRaiseStatic:
    case Opcode::kUnreachable:
    case Opcode::kReturn: {
      // No successor blocks
      break;
    }
    default: {
      /* NOTREACHED */
      JIT_ABORT(
          "Block {} has invalid terminator {}", block->id, instr->opname());
    }
  }

  traversal->emplace_back(block);
}

} // namespace

CFG::~CFG() {
  while (!blocks.IsEmpty()) {
    BasicBlock* block = &(blocks.ExtractFront());
    // This is the one situation where it's not a bug to delete a reachable
    // block, since we're deleting everything. Clear block's incoming edges so
    // its destructor doesn't complain.
    for (auto it = block->in_edges().begin(); it != block->in_edges().end();) {
      auto edge = *it;
      ++it;
      const_cast<Edge*>(edge)->set_to(nullptr);
    }
    delete block;
  }
}

BasicBlock* CFG::AllocateBlock() {
  auto block = AllocateUnlinkedBlock();
  blocks.PushBack(*block);
  return block;
}

BasicBlock* CFG::AllocateUnlinkedBlock() {
  int id = next_block_id_;
  auto block = new BasicBlock(id);
  next_block_id_++;
  return block;
}

void CFG::InsertBlock(BasicBlock* block) {
  blocks.PushBack(*block);
}

void CFG::RemoveBlock(BasicBlock* block) {
  block->cfg_node.Unlink();
}

BasicBlock* CFG::splitAfter(Instr& target) {
  auto block = target.block();
  auto tail = AllocateBlock();
  for (auto it = std::next(block->iterator_to(target)); it != block->end();) {
    auto& instr = *it;
    ++it;
    instr.unlink();
    tail->Append(&instr);
  }

  for (auto edge : tail->out_edges()) {
    edge->to()->fixupPhis(block, tail);
  }
  return tail;
}

void CFG::splitCriticalEdges() {
  std::vector<Edge*> critical_edges;

  // Separately enumerate and process the critical edges to avoid mutating the
  // CFG while iterating it.
  for (auto& block : blocks) {
    auto term = block.GetTerminator();
    JIT_DCHECK(term != nullptr, "Invalid block");
    auto num_edges = term->numEdges();
    if (num_edges < 2) {
      continue;
    }
    for (std::size_t i = 0; i < num_edges; ++i) {
      auto edge = term->edge(i);
      if (edge->to()->in_edges().size() > 1) {
        critical_edges.emplace_back(edge);
      }
    }
  }

  for (auto edge : critical_edges) {
    auto from = edge->from();
    auto to = edge->to();
    auto split_bb = AllocateBlock();
    auto term = edge->from()->GetTerminator();
    split_bb->appendWithOff<Branch>(term->bytecodeOffset(), to);
    edge->set_to(split_bb);
    to->fixupPhis(from, split_bb);
  }
}

std::vector<BasicBlock*> CFG::GetRPOTraversal() const {
  return GetRPOTraversal(entry_block);
}

std::vector<BasicBlock*> CFG::GetRPOTraversal(BasicBlock* start) {
  auto traversal = GetPostOrderTraversal(start);
  std::reverse(traversal.begin(), traversal.end());
  return traversal;
}

std::vector<BasicBlock*> CFG::GetPostOrderTraversal() const {
  return GetPostOrderTraversal(entry_block);
}

std::vector<BasicBlock*> CFG::GetPostOrderTraversal(BasicBlock* start) {
  std::vector<BasicBlock*> traversal;
  if (start == nullptr) {
    return traversal;
  }
  std::unordered_set<BasicBlock*> visited;
  postorder_traverse(start, &traversal, &visited);
  return traversal;
}

const BasicBlock* CFG::getBlockById(int id) const {
  for (auto& block : blocks) {
    if (block.id == id) {
      return &block;
    }
  }
  return nullptr;
}

} // namespace jit::hir

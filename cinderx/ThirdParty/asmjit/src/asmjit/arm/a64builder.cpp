// This file is part of AsmJit project <https://asmjit.com>
//
// See asmjit.h or LICENSE.md for license and copyright information
// SPDX-License-Identifier: Zlib

#include "../core/api-build_p.h"
#if !defined(ASMJIT_NO_AARCH64) && !defined(ASMJIT_NO_BUILDER)

#include "../arm/a64assembler.h"
#include "../arm/a64builder.h"
#include "../arm/a64emithelper_p.h"
#include "../arm/a64globals.h"
#include "../core/archcommons.h"

ASMJIT_BEGIN_SUB_NAMESPACE(a64)

// a64::Builder - Construction & Destruction
// =========================================

Builder::Builder(CodeHolder* code) noexcept : BaseBuilder() {
  _archMask = uint64_t(1) << uint32_t(Arch::kAArch64);
  if (code)
    code->attach(this);
}
Builder::~Builder() noexcept {}

// a64::Builder - Events
// =====================

Error Builder::onAttach(CodeHolder* code) noexcept {
  ASMJIT_PROPAGATE(Base::onAttach(code));

  _instructionAlignment = uint8_t(4);
  assignEmitterFuncs(this);

  return kErrorOk;
}

Error Builder::onDetach(CodeHolder* code) noexcept {
  return Base::onDetach(code);
}


// a64::Builder - Finalize
// =======================

Error Builder::finalize() {
  ASMJIT_PROPAGATE(runPasses());
  ASMJIT_PROPAGATE(relaxBranches());
  Assembler a(_code);
  a.addEncodingOptions(encodingOptions());
  a.addDiagnosticOptions(diagnosticOptions());
  return serializeTo(&a);
}

// a64::Builder - Branch Relaxation
// ================================

enum class BranchKind : uint8_t {
  kNone = 0,
  kCondBranch,
  kCompBranch,
  kTestBranch,
};

struct BranchInfo {
  BranchKind kind;
  uint32_t immBitCount;
};

static BranchInfo classifyBranch(InstId instId) noexcept {
  InstId realId = BaseInst::extractRealId(instId);
  arm::CondCode cc = BaseInst::extractARMCondCode(instId);

  if (realId == Inst::kIdB && cc != arm::CondCode::kAL)
    return {BranchKind::kCondBranch, 19};

  if (realId == Inst::kIdCbz || realId == Inst::kIdCbnz)
    return {BranchKind::kCompBranch, 19};

  if (realId == Inst::kIdTbz || realId == Inst::kIdTbnz)
    return {BranchKind::kTestBranch, 14};

  return {BranchKind::kNone, 0};
}

static uint32_t estimateNodeSize(BaseNode* node, uint32_t currentOffset) noexcept {
  if (node->isInst()) {
    InstNode* inst = node->as<InstNode>();
    InstId realId = BaseInst::extractRealId(inst->id());

    for (uint32_t i = 0; i < inst->opCount(); i++) {
      const Operand_& op = inst->op(i);

      if (realId == Inst::kIdAdr) {
        if (op.isLabel())
          return 8;
        if (op.isImm() &&
            op.as<Imm>().predicate() == Predicate::kAbsoluteAddress &&
            (op.as<Imm>().valueAs<uint64_t>() & 0xFFFu) != 0)
          return 8;
      }
      if (realId == Inst::kIdLdr && op.isMem() && op.as<Mem>().hasBaseLabel()) {
        // `kShortForm` means relaxBranches() proved the literal is within
        // imm19 reach, so the assembler emits the plain 4-byte encoding
        // instead of reserving room to relax to `adrp+ldr`.
        return Support::test(inst->options(), InstOptions::kShortForm) ? 4 : 8;
      }
      if (realId == Inst::kIdMov && op.isImm() && op.as<Imm>().valueAs<uint64_t>() > 0xFFFF)
        return 16;
    }

    return 4;
  }

  if (node->isAlign()) {
    uint32_t alignment = node->as<AlignNode>()->alignment();
    if (alignment <= 1)
      return 0;
    return (alignment - (currentOffset % alignment)) % alignment;
  }

  if (node->isEmbedData()) {
    auto* embed = node->as<EmbedDataNode>();
    return uint32_t(embed->dataSize() * embed->repeatCount());
  }

  if (node->isConstPool())
    return uint32_t(node->as<ConstPoolNode>()->size());

  return 0;
}

static void invertBranchCondition(InstNode* inst, BranchKind kind) noexcept {
  switch (kind) {
    case BranchKind::kCondBranch: {
      arm::CondCode cc = BaseInst::extractARMCondCode(inst->id());
      inst->setId(BaseInst::composeARMInstId(inst->realId(), arm::negateCond(cc)));
      break;
    }
    case BranchKind::kCompBranch:
      inst->setId(inst->realId() == Inst::kIdCbz ? Inst::kIdCbnz : Inst::kIdCbz);
      break;
    case BranchKind::kTestBranch:
      inst->setId(inst->realId() == Inst::kIdTbz ? Inst::kIdTbnz : Inst::kIdTbz);
      break;
    default:
      break;
  }
}

Error Builder::relaxBranches() {
  uint32_t labelCount = _labelNodes.size();
  if (labelCount == 0)
    return kErrorOk;

  ZoneVector<uint32_t> labelSections;
  ASMJIT_PROPAGATE(labelSections.resize(&_allocator, labelCount));

  // Whether the label is bound somewhere in the node list. `_labelNodes` holds
  // a node for every label that has merely been referenced, and an unbound
  // one's position is meaningless, so it can't be used on its own to decide
  // that a displacement is known.
  ZoneVector<uint8_t> labelBound;
  ASMJIT_PROPAGATE(labelBound.resize(&_allocator, labelCount));

  auto updateNodePositions = [&]() noexcept {
    uint32_t sectionId = 0;
    uint32_t offset = 0;

    memset(labelSections.data(), 0, labelCount * sizeof(uint32_t));
    memset(labelBound.data(), 0, labelCount * sizeof(uint8_t));

    for (BaseNode* node = firstNode(); node; node = node->next()) {
      if (node->isSection()) {
        sectionId = node->as<SectionNode>()->id();
        offset = 0;
      }

      uint32_t size = estimateNodeSize(node, offset);
      node->setPosition(offset);

      if (node->isLabel()) {
        uint32_t labelId = node->as<LabelNode>()->labelId();
        if (labelId < labelCount) {
          labelSections[labelId] = sectionId;
          labelBound[labelId] = 1;
        }
      }

      offset += size;
    }
  };

  for (;;) {
    updateNodePositions();

    bool changed = false;
    uint32_t sectionId = 0;

    for (BaseNode* node = firstNode(); node; node = node->next()) {
      if (node->isSection()) {
        sectionId = node->as<SectionNode>()->id();
        continue;
      }

      if (!node->isInst())
        continue;

      InstNode* inst = node->as<InstNode>();
      BranchInfo bi = classifyBranch(inst->id());
      if (bi.kind == BranchKind::kNone)
        continue;

      uint32_t labelIdx = inst->indexOfLabelOp();
      if (labelIdx == Globals::kNotFound)
        continue;

      uint32_t targetLabelId = inst->op(labelIdx).as<Label>().id();
      if (targetLabelId >= labelCount)
        continue;

      bool needsExpansion = (labelSections[targetLabelId] != sectionId);

      if (!needsExpansion) {
        LabelNode* targetNode = _labelNodes[targetLabelId];
        if (!targetNode)
          continue;

        int64_t displacement = int64_t(targetNode->position()) - int64_t(inst->position());
        int64_t dispImm = displacement >> 2;

        needsExpansion = (displacement & 3) != 0 ||
                         !Support::isEncodableOffset64(dispImm, bi.immBitCount);
      }

      if (!needsExpansion)
        continue;

      LabelNode* skipLabel;
      ASMJIT_PROPAGATE(newLabelNode(&skipLabel));

      InstNode* uncondBranch;
      ASMJIT_PROPAGATE(newInstNode(&uncondBranch, Inst::kIdB, InstOptions::kNone, 1));
      uncondBranch->_resetOps();
      uncondBranch->setOp(0, inst->op(labelIdx));

      inst->setOp(labelIdx, skipLabel->label());
      invertBranchCondition(inst, bi.kind);

      addAfter(uncondBranch, inst);
      addAfter(skipLabel, uncondBranch);

      changed = true;
    }

    if (!changed)
      break;
  }

  // Shrink `ldr Xd, [label]` to its plain 4-byte encoding wherever the literal
  // is provably within the +/-1MB reach of the imm19 offset. This runs after
  // the previous pass because it only shrinks instructions and the previous
  // one can expand them.
  for (;;) {
    updateNodePositions();

    bool changed = false;
    uint32_t sectionId = 0;

    for (BaseNode* node = firstNode(); node; node = node->next()) {
      if (node->isSection()) {
        sectionId = node->as<SectionNode>()->id();
        continue;
      }

      if (!node->isInst())
        continue;

      InstNode* inst = node->as<InstNode>();
      if (BaseInst::extractRealId(inst->id()) != Inst::kIdLdr)
        continue;

      if (Support::test(inst->options(), InstOptions::kShortForm))
        continue;

      uint32_t targetLabelId = Globals::kInvalidId;
      for (uint32_t i = 0; i < inst->opCount(); i++) {
        const Operand_& op = inst->op(i);
        if (op.isMem() && op.as<Mem>().hasBaseLabel()) {
          targetLabelId = op.as<Mem>().baseId();
          break;
        }
      }

      if (targetLabelId >= labelCount || !labelBound[targetLabelId])
        continue;

      // Across sections the displacement isn't settled until the sections are
      // laid out, which happens after this runs.
      if (labelSections[targetLabelId] != sectionId)
        continue;

      LabelNode* targetNode = _labelNodes[targetLabelId];
      if (!targetNode)
        continue;

      int64_t displacement = int64_t(targetNode->position()) - int64_t(inst->position());
      if ((displacement & 3) != 0 || !Support::isEncodableOffset64(displacement >> 2, 19))
        continue;

      inst->addOptions(InstOptions::kShortForm);
      changed = true;
    }

    if (!changed)
      break;
  }

  return kErrorOk;
}

ASMJIT_END_SUB_NAMESPACE

#endif // !ASMJIT_NO_AARCH64 && !ASMJIT_NO_BUILDER

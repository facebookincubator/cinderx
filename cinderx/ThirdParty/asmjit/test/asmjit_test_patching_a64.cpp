// Tests for ARM64 branch patching in vendored asmjit.
//
// These tests verify that asmjit correctly handles ARM64 instructions that
// need patching when their target is beyond the instruction's offset range.

#include <asmjit/core.h>
#include <asmjit/a64.h>
#include <stdio.h>
#include <string.h>

#include "broken.h"

using namespace asmjit;

// Helper: Create a CodeHolder + Assembler targeting AArch64.
static void setupCode(CodeHolder& code, a64::Assembler& as) {
  Environment env(Arch::kAArch64);
  code.init(env, 0);
  code.attach(&as);
}

// Helper: Create a CodeHolder + Assembler with no base address (kNoBaseAddress),
// so relocations are deferred to relocateToBase().
static void setupCodeNoBase(CodeHolder& code, a64::Assembler& as) {
  Environment env(Arch::kAArch64);
  code.init(env);
  code.attach(&as);
}

// Helper: Get a hex string of the emitted code buffer.
static void getHex(const CodeHolder& code, String& out) {
  const Section* text = code.textSection();
  out.appendHex(text->data(), text->bufferSize());
}

// Helper: Emit `count` NOP instructions to push the code forward.
static void emitNops(a64::Assembler& as, size_t count) {
  for (size_t i = 0; i < count; i++) {
    as.nop();
  }
}

static uint32_t a64BranchTargetOffset(const uint8_t* buf, uint32_t sourceOffset) {
  uint32_t instr = Support::readU32uLE(buf + sourceOffset);
  int32_t imm26 = int32_t(instr << 6) >> 6;
  return sourceOffset + uint32_t(imm26 * 4);
}

static uint32_t findA64BranchStubForTarget(
    const uint8_t* buf,
    size_t size,
    uint64_t targetAddress) {
  for (uint32_t offset = 0; offset + 16 <= size; offset += 4) {
    if (Support::readU32uLE(buf + offset) == 0x58000050u &&
        Support::readU32uLE(buf + offset + 4) == 0xD61F0200u &&
        Support::readU64uLE(buf + offset + 8) == targetAddress) {
      return offset;
    }
  }

  return Globals::kNotFound;
}

// ============================================================================
// [BL Patching Tests]
// ============================================================================

// Test that bl to a label within ±128MB encodes as a direct bl instruction.
UNIT(a64_bl_patching_in_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  as.bl(target);
  emitNops(as, 10);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // bl to target: offset = 11 instructions * 4 bytes = 44 = 0x2C
  // imm26 = 44 / 4 = 11 = 0x0B
  // bl opcode: 0x94000000 | 0x0B = 0x9400000B
  // Little-endian: 0B000094
  // First 4 bytes should be the bl instruction
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "0B000094", 8) == 0)
    .message("Expected bl encoding, got: %s", hex.data());
}

// Test that bl to an absolute address that is in ±128MB range resolves to a
// single direct bl, even when the base address is not known at emit time.
UNIT(a64_bl_patching_absolute_addr_in_range_no_nop) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  as.bl(Imm(uint64_t(0x40)));

  EXPECT_EQ(code.textSection()->bufferSize(), 4u)
    .message("Expected a single branch placeholder");

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  const uint8_t* buf = code.textSection()->data();
  uint32_t instr = Support::readU32uLE(buf);
  EXPECT_EQ(instr, 0x94000010u)
    .message("Expected direct bl +0x40, got: 0x%08X", instr);
}

// Test that bl to an absolute address that is out of ±128MB range
// gets relocated to a branch to an out-of-line stub.
UNIT(a64_bl_patching_absolute_addr) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  uint64_t farAddr = 0x100000000ULL; // 4GB away
  as.bl(Imm(farAddr));

  EXPECT_EQ(code.textSection()->bufferSize(), 4u)
    .message("Expected a single branch placeholder");
  EXPECT_EQ(Support::readU32uLE(code.textSection()->data()), 0x94000000u)
    .message("Expected bl placeholder");

  // Verify a RelocEntry was created
  EXPECT_GT(code.relocEntries().size(), 0u)
    .message("Expected at least one reloc entry for far bl");

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  Section* stubs = code.sectionByName(".a64stubs");
  EXPECT(stubs != nullptr)
    .message("Expected an AArch64 branch stub section");
  if (!stubs)
    return;

  uint32_t branch = Support::readU32uLE(code.textSection()->data());
  uint32_t expectedImm26 = uint32_t(stubs->offset() >> 2) & 0x03FFFFFFu;
  EXPECT_EQ(branch, 0x94000000u | expectedImm26)
    .message("Expected bl to stub, got: 0x%08X", branch);

  const uint8_t* stub = stubs->data();
  EXPECT_EQ(Support::readU32uLE(stub), 0x58000050u)
    .message("Expected ldr x16, literal in stub");
  EXPECT_EQ(Support::readU32uLE(stub + 4), 0xD61F0200u)
    .message("Expected br x16 in stub");
  EXPECT_EQ(Support::readU64uLE(stub + 8), farAddr)
    .message("Expected target literal in stub");
}

UNIT(a64_bl_patching_absolute_addr_relocate_multiple_bases_keeps_stub_slots) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  uint64_t farAddr1 = 0x100000000ULL;
  uint64_t farAddr2 = 0x200000000ULL;
  as.bl(Imm(farAddr1));
  as.bl(Imm(farAddr2));

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(farAddr2), kErrorOk);
  EXPECT_EQ(code.relocateToBase(farAddr1), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  Section* stubs = code.sectionByName(".a64stubs");
  EXPECT(stubs != nullptr)
    .message("Expected an AArch64 branch stub section");
  if (!stubs)
    return;

  const uint8_t* text = code.textSection()->data();
  uint32_t firstStubOffset = a64BranchTargetOffset(text, 0) - uint32_t(stubs->offset());
  uint32_t secondStubOffset = a64BranchTargetOffset(text, 4) - uint32_t(stubs->offset());

  EXPECT_NE(firstStubOffset, secondStubOffset)
    .message("Expected each far target to keep a distinct stub slot");
  EXPECT(firstStubOffset + 16 <= stubs->bufferSize())
    .message("Expected first branch target to land in the stub section");
  EXPECT(secondStubOffset + 16 <= stubs->bufferSize())
    .message("Expected second branch target to land in the stub section");
  if (firstStubOffset + 16 > stubs->bufferSize() || secondStubOffset + 16 > stubs->bufferSize())
    return;

  EXPECT_EQ(Support::readU64uLE(stubs->data() + firstStubOffset + 8), farAddr1)
    .message("Expected first branch stub to retain the first target");
  EXPECT_EQ(Support::readU64uLE(stubs->data() + secondStubOffset + 8), farAddr2)
    .message("Expected second branch stub to retain the second target");
}

// Test that a local guarded island is appended only when the final layout puts
// the global stub section out of the AArch64 branch range.
UNIT(a64_bl_patching_absolute_addr_uses_local_island_when_global_stub_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  uint64_t farAddr = 0x100000000ULL; // 4GB away
  as.bl(Imm(farAddr));

  Section* padding = nullptr;
  EXPECT_EQ(code.newSection(&padding, ".padding", SIZE_MAX, SectionFlags::kNone, 1, 1), kErrorOk);
  if (!padding)
    return;

  padding->setVirtualSize(0x08000000u);

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  Section* stubs = code.sectionByName(".a64stubs");
  EXPECT(stubs != nullptr)
    .message("Expected an AArch64 branch stub section");
  if (!stubs)
    return;

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();

  EXPECT_EQ(text->bufferSize(), 24u)
    .message("Expected bl + guard + one local stub");
  if (text->bufferSize() < 24)
    return;

  uint32_t branch = Support::readU32uLE(buf);
  EXPECT_EQ(branch, 0x94000002u)
    .message("Expected bl to the local island stub, got: 0x%08X", branch);
  EXPECT_EQ(a64BranchTargetOffset(buf, 0), 8u)
    .message("Expected bl to target the local stub");

  uint32_t guard = Support::readU32uLE(buf + 4);
  EXPECT_EQ(guard, 0x14000005u)
    .message("Expected guard branch over the local island, got: 0x%08X", guard);
  EXPECT_EQ(a64BranchTargetOffset(buf, 4), 24u)
    .message("Expected guard branch to skip the island");

  EXPECT_EQ(Support::readU32uLE(buf + 8), 0x58000050u)
    .message("Expected ldr x16, literal in local stub");
  EXPECT_EQ(Support::readU32uLE(buf + 12), 0xD61F0200u)
    .message("Expected br x16 in local stub");
  EXPECT_EQ(Support::readU64uLE(buf + 16), farAddr)
    .message("Expected target literal in local stub");
}

UNIT(a64_bl_patching_absolute_addr_uses_in_section_island_for_huge_section) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  uint64_t farAddr = 0x100000000ULL; // 4GB away
  as.bl(Imm(farAddr));
  code.textSection()->setVirtualSize(0x08000000u);

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();

  EXPECT_EQ(text->bufferSize(), 24u)
    .message("Expected bl + guard + one local stub");
  EXPECT_EQ(text->virtualSize(), 0x08000000u)
    .message("Expected the section to remain virtually huge");
  if (text->bufferSize() < 24)
    return;

  EXPECT_EQ(Support::readU32uLE(buf), 0x94000002u)
    .message("Expected bl to the local island stub");
  EXPECT_EQ(Support::readU32uLE(buf + 4), 0x14000005u)
    .message("Expected guard branch over the local island");
  EXPECT_EQ(Support::readU64uLE(buf + 16), farAddr)
    .message("Expected target literal in local stub");
}

UNIT(a64_b_patching_absolute_addr_uses_local_island_when_global_stub_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  uint64_t farAddr = 0x100000000ULL; // 4GB away
  as.b(Imm(farAddr));

  Section* padding = nullptr;
  EXPECT_EQ(code.newSection(&padding, ".padding", SIZE_MAX, SectionFlags::kNone, 1, 1), kErrorOk);
  if (!padding)
    return;

  padding->setVirtualSize(0x08000000u);

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();

  EXPECT_EQ(text->bufferSize(), 24u)
    .message("Expected b + guard + one local stub");
  if (text->bufferSize() < 24)
    return;

  uint32_t branch = Support::readU32uLE(buf);
  EXPECT_EQ(branch, 0x14000002u)
    .message("Expected b to the local island stub, got: 0x%08X", branch);
  EXPECT_EQ(a64BranchTargetOffset(buf, 0), 8u)
    .message("Expected b to target the local stub");

  uint32_t guard = Support::readU32uLE(buf + 4);
  EXPECT_EQ(guard, 0x14000005u)
    .message("Expected guard branch over the local island, got: 0x%08X", guard);
  EXPECT_EQ(a64BranchTargetOffset(buf, 4), 24u)
    .message("Expected guard branch to skip the island");

  EXPECT_EQ(Support::readU32uLE(buf + 8), 0x58000050u)
    .message("Expected ldr x16, literal in local stub");
  EXPECT_EQ(Support::readU32uLE(buf + 12), 0xD61F0200u)
    .message("Expected br x16 in local stub");
  EXPECT_EQ(Support::readU64uLE(buf + 16), farAddr)
    .message("Expected target literal in local stub");
}

// Test that Builder leaves far absolute branches to relocation-time patching
// so in-range targets can still become direct branches after the base is known.
UNIT(a64_bl_patching_absolute_addr_builder_uses_reloc_stub) {
  CodeHolder code;
  Environment env(Arch::kAArch64);
  code.init(env);

  a64::Builder as(&code);

  uint64_t farAddr = 0x100000000ULL; // 4GB away
  as.bl(Imm(farAddr));

  EXPECT_EQ(as.finalize(), kErrorOk);
  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  Section* stubs = code.sectionByName(".a64stubs");
  EXPECT(stubs != nullptr)
    .message("Expected Builder to use relocation-time stubs");
  if (!stubs)
    return;

  const uint8_t* buf = code.textSection()->data();
  EXPECT_EQ(code.textSection()->bufferSize(), 4u)
    .message("Expected a single bl site");

  uint32_t branch = Support::readU32uLE(buf);
  uint32_t expectedImm26 = uint32_t(stubs->offset() >> 2) & 0x03FFFFFFu;
  EXPECT_EQ(branch, 0x94000000u | expectedImm26)
    .message("Expected bl to .a64stubs, got: 0x%08X", branch);

  EXPECT_EQ(Support::readU32uLE(stubs->data()), 0x58000050u)
    .message("Expected ldr x16, literal in stub");
  EXPECT_EQ(Support::readU32uLE(stubs->data() + 4), 0xD61F0200u)
    .message("Expected br x16 in stub");
  EXPECT_EQ(Support::readU64uLE(stubs->data() + 8), farAddr)
    .message("Expected target literal in stub");
}

// Test that Builder uses the same relocation-time stub path for an absolute
// branch that is outside direct `bl` range after the base is known.
UNIT(a64_bl_patching_absolute_addr_builder_midrange_reloc_stub) {
  CodeHolder code;
  Environment env(Arch::kAArch64);
  code.init(env, 0);

  a64::Builder as(&code);

  uint64_t midrangeAddr = 0x10000000ULL; // 256MB away
  as.bl(Imm(midrangeAddr));

  EXPECT_EQ(as.finalize(), kErrorOk);
  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  Section* stubs = code.sectionByName(".a64stubs");
  EXPECT(stubs != nullptr)
    .message("Expected Builder to use relocation-time stubs");
  if (!stubs)
    return;

  const uint8_t* buf = code.textSection()->data();
  EXPECT_EQ(code.textSection()->bufferSize(), 4u)
    .message("Expected a single bl site");

  uint32_t branch = Support::readU32uLE(buf);
  uint32_t expectedImm26 = uint32_t(stubs->offset() >> 2) & 0x03FFFFFFu;
  EXPECT_EQ(branch, 0x94000000u | expectedImm26)
    .message("Expected bl to .a64stubs, got: 0x%08X", branch);

  EXPECT_EQ(Support::readU32uLE(stubs->data()), 0x58000050u)
    .message("Expected ldr x16, literal in stub");
  EXPECT_EQ(Support::readU32uLE(stubs->data() + 4), 0xD61F0200u)
    .message("Expected br x16 in stub");
  EXPECT_EQ(Support::readU64uLE(stubs->data() + 8), midrangeAddr)
    .message("Expected target literal in stub");
}

// Test that Builder emits only the direct `bl` when a known-base CodeHolder can
// prove the absolute target is already in range.
UNIT(a64_bl_patching_absolute_addr_builder_in_range_no_stub_pool) {
  CodeHolder code;
  Environment env(Arch::kAArch64);
  code.init(env, 0);

  a64::Builder as(&code);

  as.bl(Imm(uint64_t(0x40)));

  EXPECT_EQ(as.finalize(), kErrorOk);
  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  EXPECT(code.sectionByName(".a64stubs") == nullptr)
    .message("Expected no out-of-line stub section");
  EXPECT_EQ(code.textSection()->bufferSize(), 4u)
    .message("Expected a single direct bl");

  const uint8_t* buf = code.textSection()->data();
  uint32_t instr = Support::readU32uLE(buf);
  EXPECT_EQ(instr, 0x94000010u)
    .message("Expected direct bl +0x40, got: 0x%08X", instr);
}

// Test that Builder binds each absolute branch to the relocation-time stub with
// the matching target literal even when prior nodes have variable serialized
// sizes.
UNIT(a64_bl_patching_absolute_addr_builder_reloc_stub_multiple_targets) {
  CodeHolder code;
  Environment env(Arch::kAArch64);
  code.init(env);

  a64::Builder as(&code);

  uint64_t farAddr1 = 0x100000000ULL;
  uint64_t farAddr2 = 0x200000000ULL;

  as.mov(a64::x0, 0x123456789ABCULL);
  as.bl(Imm(farAddr1));
  as.mov(a64::x1, 0xFEDCBA987654ULL);
  as.bl(Imm(farAddr2));

  EXPECT_EQ(as.finalize(), kErrorOk);
  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  Section* stubs = code.sectionByName(".a64stubs");
  EXPECT(stubs != nullptr)
    .message("Expected Builder to use relocation-time stubs");
  if (!stubs)
    return;

  const uint8_t* buf = code.textSection()->data();
  size_t size = code.textSection()->bufferSize();

  uint32_t stub1 = findA64BranchStubForTarget(stubs->data(), stubs->bufferSize(), farAddr1);
  uint32_t stub2 = findA64BranchStubForTarget(stubs->data(), stubs->bufferSize(), farAddr2);
  EXPECT_NE(stub1, Globals::kNotFound)
    .message("Expected a stub for the first target");
  EXPECT_NE(stub2, Globals::kNotFound)
    .message("Expected a stub for the second target");
  if (stub1 == Globals::kNotFound || stub2 == Globals::kNotFound)
    return;

  uint32_t firstBranchTarget = Globals::kNotFound;
  uint32_t secondBranchTarget = Globals::kNotFound;

  for (uint32_t offset = 0; offset + 4 <= size; offset += 4) {
    uint32_t instr = Support::readU32uLE(buf + offset);
    if ((instr & 0xFC000000u) != 0x94000000u)
      continue;

    if (firstBranchTarget == Globals::kNotFound)
      firstBranchTarget = a64BranchTargetOffset(buf, offset);
    else {
      secondBranchTarget = a64BranchTargetOffset(buf, offset);
      break;
    }
  }

  EXPECT_EQ(firstBranchTarget, stubs->offset() + stub1)
    .message("Expected the first bl to target the first stub");
  EXPECT_EQ(secondBranchTarget, stubs->offset() + stub2)
    .message("Expected the second bl to target the second stub");
}

// Test that Builder doesn't rewrite conditional absolute branches to
// unconditional stub calls.
UNIT(a64_b_cond_patching_absolute_addr_builder_preserves_condition) {
  CodeHolder code;
  Environment env(Arch::kAArch64);
  code.init(env, 0);

  a64::Builder as(&code);

  as.b_eq(Imm(uint64_t(0x40)));

  EXPECT_EQ(as.finalize(), kErrorOk);
  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  EXPECT(code.sectionByName(".a64stubs") == nullptr)
    .message("Expected conditional branches not to use an unconditional stub");
  EXPECT_EQ(code.textSection()->bufferSize(), 4u)
    .message("Expected a single conditional branch");

  const uint8_t* buf = code.textSection()->data();
  uint32_t instr = Support::readU32uLE(buf);
  EXPECT_EQ(instr, 0x54000200u)
    .message("Expected conditional b.eq +0x40, got: 0x%08X", instr);
}

// ============================================================================
// [TBZ/TBNZ Patching Tests]
// ============================================================================

// Test that tbz to a nearby label encodes as a direct tbz instruction.
UNIT(a64_tbz_patching_in_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // tbz x0, #5, target
  as.tbz(a64::x0, 5, target);
  emitNops(as, 4);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // Forward ref emits 4-byte instruction (no NOP placeholder).
  // Layout: tbz(0) + 4 NOPs(4-16) + target(20)
  // displacement = 20, imm14 = 5
  // tbz x0, #5, +20: b5=0, op=0, b40=00101, imm14=5, Rt=0
  // = 0x362800A0
  // LE: A0002836
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "A0002836", 8) == 0)
    .message("Expected tbz x0, #5 encoding, got: %s", hex.data());
}

// Test that tbz to a far label (beyond ±32KB) produces an error.
// Relaxation is now handled by the Builder's relaxBranches() pass.
UNIT(a64_tbz_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  as.tbz(a64::x0, 5, target);
  emitNops(as, 8200);
  Error err = as.bind(target);

  EXPECT_EQ(err, kErrorInvalidDisplacement)
    .message("Expected kErrorInvalidDisplacement for out-of-range tbz");
}

// Test that tbnz to a far label also produces an error.
UNIT(a64_tbnz_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  as.tbnz(a64::x0, 5, target);
  emitNops(as, 8200);
  Error err = as.bind(target);

  EXPECT_EQ(err, kErrorInvalidDisplacement)
    .message("Expected kErrorInvalidDisplacement for out-of-range tbnz");
}

// ============================================================================
// [CBZ/CBNZ Patching Tests]
// ============================================================================

// Test that cbz to a nearby label encodes as a direct cbz instruction.
UNIT(a64_cbz_patching_in_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // cbz w1, target
  as.cbz(a64::w1, target);
  emitNops(as, 4);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // Forward ref emits 4-byte instruction (no NOP placeholder).
  // Layout: cbz(0) + 4 NOPs(4-16) + target(20)
  // displacement = 20, imm19 = 5
  // cbz w1, +20: sf=0, opcode=0x34, imm19=5, Rt=1
  // = 0x340000A1
  // LE: A1000034
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "A1000034", 8) == 0)
    .message("Expected cbz w1 encoding, got: %s", hex.data());
}

// Test that cbz to a far label (beyond ±1MB) produces an error.
// Relaxation is now handled by the Builder's relaxBranches() pass.
UNIT(a64_cbz_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  as.cbz(a64::w1, target);
  emitNops(as, 262200);
  Error err = as.bind(target);

  EXPECT_EQ(err, kErrorInvalidDisplacement)
    .message("Expected kErrorInvalidDisplacement for out-of-range cbz");
}

// Test that cbnz to a far label also produces an error.
UNIT(a64_cbnz_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  as.cbnz(a64::w1, target);
  emitNops(as, 262200);
  Error err = as.bind(target);

  EXPECT_EQ(err, kErrorInvalidDisplacement)
    .message("Expected kErrorInvalidDisplacement for out-of-range cbnz");
}

// ============================================================================
// [B.cond Patching Tests]
// ============================================================================

// Test that b.eq to a nearby label encodes as a direct b.eq instruction.
UNIT(a64_bcond_patching_in_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // b.eq target
  as.b_eq(target);
  emitNops(as, 4);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // Forward ref emits 4-byte instruction (no NOP placeholder).
  // Layout: b.eq(0) + 4 NOPs(4-16) + target(20)
  // displacement = 20, imm19 = 5
  // b.eq +20: opcode=0x54, imm19=5, cond=0
  // = 0x540000A0
  // LE: A0000054
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "A0000054", 8) == 0)
    .message("Expected b.eq encoding, got: %s", hex.data());
}

// Test that b.eq to a far label (beyond ±1MB) produces an error.
// Relaxation is now handled by the Builder's relaxBranches() pass.
UNIT(a64_bcond_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  as.b_eq(target);
  emitNops(as, 262200);
  Error err = as.bind(target);

  EXPECT_EQ(err, kErrorInvalidDisplacement)
    .message("Expected kErrorInvalidDisplacement for out-of-range b.eq");
}

// Test that b.ne to a far label also produces an error.
UNIT(a64_bcond_ne_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  as.b_ne(target);
  emitNops(as, 262200);
  Error err = as.bind(target);

  EXPECT_EQ(err, kErrorInvalidDisplacement)
    .message("Expected kErrorInvalidDisplacement for out-of-range b.ne");
}

// ============================================================================
// [ADR Patching Tests]
// ============================================================================

// Test that adr to a forward label emits 8-byte region (adr + NOP placeholder)
// and creates a RelocEntry for later resolution.
UNIT(a64_adr_patching_forward_ref) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // adr x1, target
  as.adr(a64::x1, target);
  emitNops(as, 4);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // Forward ref emits 8-byte region: adr x1, #0 (unpatched) + NOP placeholder.
  // adr x1, #0: 0x10000001 -> LE: 01000010
  // The displacement is resolved during relocateToBase(), not at emit time.
  EXPECT(hex.size() >= 16 && memcmp(hex.data(), "010000101F2003D5", 16) == 0)
    .message("Expected adr x1, #0 + NOP placeholder, got: %s", hex.data());

  // Verify a RelocEntry was created for the adr
  EXPECT_GT(code.relocEntries().size(), 0u)
    .message("Expected at least one reloc entry for adr forward ref");
}

// ============================================================================
// [LDR Literal Patching Tests]
// ============================================================================

// Test that ldr (PC-relative literal) to a nearby label resolves correctly
// and stays as ldr literal + NOP after relocation.
UNIT(a64_ldr_literal_in_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label pool = as.newLabel();
  as.ldr(a64::x0, a64::ptr(pool));
  emitNops(as, 4);
  as.bind(pool);
  uint64_t value = 0xDEADBEEFCAFEBABEULL;
  as.embed(&value, sizeof(value));

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.resolveUnresolvedLinks(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0x10000), kErrorOk);

  // ldr x0, +24: displacement = 24, imm19 = 6
  // 0x58000000 | (6 << 5) = 0x580000C0
  uint32_t firstWord = Support::readU32uLE(code.textSection()->data());
  EXPECT_EQ(firstWord, 0x580000C0u)
    .message("Expected ldr x0, +24, got: 0x%08X", firstWord);

  uint32_t secondWord = Support::readU32uLE(code.textSection()->data() + 4);
  EXPECT_EQ(secondWord, 0xD503201Fu)
    .message("Expected NOP after ldr, got: 0x%08X", secondWord);
}

// Test that ldr (PC-relative literal) to a far label (beyond ±1MB) gets
// relaxed to adrp+ldr after relocation.
UNIT(a64_ldr_literal_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label pool = as.newLabel();
  as.ldr(a64::x0, a64::ptr(pool));
  // Emit enough NOPs to exceed 19-bit signed offset range (±1MB = 262144 instructions)
  emitNops(as, 262200);
  as.bind(pool);
  uint64_t value = 0xDEADBEEFCAFEBABEULL;
  as.embed(&value, sizeof(value));

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.resolveUnresolvedLinks(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0x10000), kErrorOk);

  // Verify relaxed to adrp x0, page + ldr x0, [x0, #off].
  uint32_t firstWord = Support::readU32uLE(code.textSection()->data());
  EXPECT_EQ(firstWord & 0x9F000000u, 0x90000000u)
    .message("Expected adrp instruction, got: 0x%08X", firstWord);
  EXPECT_EQ(firstWord & 0x1Fu, 0u)
    .message("Expected Rd=x0 in adrp, got Rd=%u", firstWord & 0x1Fu);

  uint32_t secondWord = Support::readU32uLE(code.textSection()->data() + 4);
  EXPECT_EQ(secondWord & 0xFFC00000u, 0xF9400000u)
    .message("Expected ldr x0, [x0, #imm], got: 0x%08X", secondWord);
  EXPECT_EQ(secondWord & 0x1Fu, 0u)
    .message("Expected Rd=x0, got Rd=%u", secondWord & 0x1Fu);
  EXPECT_EQ((secondWord >> 5) & 0x1Fu, 0u)
    .message("Expected Rn=x0, got Rn=%u", (secondWord >> 5) & 0x1Fu);
}

// ============================================================================
// [B (unconditional) Patching Tests]
// ============================================================================

// Test that b to an absolute address that is out of ±128MB range
// gets relocated to a branch to an out-of-line stub.
UNIT(a64_b_patching_absolute_addr) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  uint64_t farAddr = 0x100000000ULL; // 4GB away
  as.b(Imm(farAddr));

  EXPECT_EQ(code.textSection()->bufferSize(), 4u)
    .message("Expected a single branch placeholder");
  EXPECT_EQ(Support::readU32uLE(code.textSection()->data()), 0x14000000u)
    .message("Expected b placeholder");

  // Verify a RelocEntry was created
  EXPECT_GT(code.relocEntries().size(), 0u)
    .message("Expected at least one reloc entry for far b");

  EXPECT_EQ(code.flatten(), kErrorOk);
  EXPECT_EQ(code.relocateToBase(0), kErrorOk);

  Section* stubs = code.sectionByName(".a64stubs");
  EXPECT(stubs != nullptr)
    .message("Expected an AArch64 branch stub section");
  if (!stubs)
    return;

  uint32_t branch = Support::readU32uLE(code.textSection()->data());
  uint32_t expectedImm26 = uint32_t(stubs->offset() >> 2) & 0x03FFFFFFu;
  EXPECT_EQ(branch, 0x14000000u | expectedImm26)
    .message("Expected b to stub, got: 0x%08X", branch);

  const uint8_t* stub = stubs->data();
  EXPECT_EQ(Support::readU32uLE(stub), 0x58000050u)
    .message("Expected ldr x16, literal in stub");
  EXPECT_EQ(Support::readU32uLE(stub + 4), 0xD61F0200u)
    .message("Expected br x16 in stub");
  EXPECT_EQ(Support::readU64uLE(stub + 8), farAddr)
    .message("Expected target literal in stub");
}

// ============================================================================
// [load_addr Patching Tests]
// ============================================================================

// Helper: Read a little-endian uint32 from a buffer.
static uint32_t readU32LE(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Test that load_addr emits a placeholder (adr Rd, #0 + NOP) and a RelocEntry.
UNIT(a64_load_addr_placeholder) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  as.load_addr(a64::x1, uint64_t(0xDEADBEEF));

  String hex;
  getHex(code, hex);

  // adr x1, #0 = 0x10000001 -> LE: 01000010
  // NOP = 0xD503201F -> LE: 1F2003D5
  EXPECT_EQ(hex, "010000101F2003D5")
    .message("Expected adr x1, #0 + NOP placeholder, got: %s", hex.data());

  EXPECT_GT(code.relocEntries().size(), 0u)
    .message("Expected a reloc entry for load_addr");
}

// Test that load_addr relaxes to `adr Rd, target; nop` when displacement fits
// in ±1MB (21-bit signed offset).
UNIT(a64_load_addr_reloc_adr) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  // Target address 0x64 (100 bytes from base 0).
  as.load_addr(a64::x2, uint64_t(0x64));

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(0), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);
  uint32_t instr1 = readU32LE(buf + 4);

  // displacement = 0x64, immLo = 0x64 & 3 = 0, immHi = (0x64 >> 2) = 0x19
  // adr x2: 0x10000000 | (0 << 29) | (0x19 << 5) | 2 = 0x10000322
  EXPECT_EQ(instr0, 0x10000322u)
    .message("Expected adr x2, #0x64, got: 0x%08X", instr0);
  EXPECT_EQ(instr1, 0xD503201Fu)
    .message("Expected NOP, got: 0x%08X", instr1);
}

// Test that load_addr relaxes to `adrp Rd, page; add Rd, Rd, #off` when
// displacement exceeds ±1MB but fits in ±4GB page range.
UNIT(a64_load_addr_reloc_adrp_add) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  // Target 2MB away: beyond ±1MB adr range, within ±4GB adrp range.
  // Use 0x200ABC to also test non-zero page offset.
  uint64_t target = 0x200ABC;
  as.load_addr(a64::x3, target);

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(0), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);
  uint32_t instr1 = readU32LE(buf + 4);

  // pageDelta = (0x200ABC >> 12) - (0 >> 12) = 0x200
  // pageOffset = 0x200ABC & 0xFFF = 0xABC
  // immLo = 0x200 & 3 = 0, immHi = (0x200 >> 2) = 0x80
  // adrp x3: 0x90000000 | (0 << 29) | (0x80 << 5) | 3 = 0x90001003
  // add x3, x3, #0xABC: 0x91000000 | (0xABC << 10) | (3 << 5) | 3 = 0x912AF063
  EXPECT_EQ(instr0, 0x90001003u)
    .message("Expected adrp x3, got: 0x%08X", instr0);
  EXPECT_EQ(instr1, 0x912AF063u)
    .message("Expected add x3, x3, #0xABC, got: 0x%08X", instr1);
}

// Test that load_addr falls back to `ldr Rd, [pc+off]` from the address table
// when target is beyond ±4GB.
UNIT(a64_load_addr_reloc_ldr) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  // Target 256GB away: beyond ±4GB adrp range.
  uint64_t target = 0x4000000000ULL;
  as.load_addr(a64::x4, target);

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(0), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);
  uint32_t instr1 = readU32LE(buf + 4);

  // Should be ldr x4, [pc+off]: opcode 0x58000000 | (imm19 << 5) | 4
  // The address table is right after the text section. Text = 8 bytes,
  // so address table is at offset 8. ldr displacement = 8 - 0 = 8.
  // imm19 = 8 / 4 = 2
  // ldr x4, [pc+8]: 0x58000000 | (2 << 5) | 4 = 0x58000044
  EXPECT_EQ(instr0, 0x58000044u)
    .message("Expected ldr x4, [pc+off], got: 0x%08X", instr0);
  EXPECT_EQ(instr1, 0xD503201Fu)
    .message("Expected NOP, got: 0x%08X", instr1);
}

// Test load_addr with negative displacement (target address < PC).
UNIT(a64_load_addr_reloc_adr_negative) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  // Target = 0x1000, base = 0x1080. PC = base + 0 = 0x1080.
  // displacement = 0x1000 - 0x1080 = -0x80 = -128.
  as.load_addr(a64::x5, uint64_t(0x1000));

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(0x1080), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);
  uint32_t instr1 = readU32LE(buf + 4);

  // displacement = -128 = 0xFFFFFF80 (as uint32)
  // immLo = 0xFFFFFF80 & 3 = 0, immHi = (0xFFFFFF80 >> 2) & 0x7FFFF = 0x7FFE0
  // adr x5: 0x10000000 | (0 << 29) | (0x7FFE0 << 5) | 5 = 0x10FFFC05
  EXPECT_EQ(instr0, 0x10FFFC05u)
    .message("Expected adr x5, #-128, got: 0x%08X", instr0);
  EXPECT_EQ(instr1, 0xD503201Fu)
    .message("Expected NOP, got: 0x%08X", instr1);
}

// Test load_addr with a non-zero base address that still fits in adr range.
UNIT(a64_load_addr_reloc_nonzero_base) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  // Place code at a realistic JIT address, target nearby.
  uint64_t base = 0x7F0000000000ULL;
  uint64_t target = base + 0x100;
  as.load_addr(a64::x0, target);

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(base), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);
  uint32_t instr1 = readU32LE(buf + 4);

  // displacement = 0x100, immLo = 0, immHi = 0x40
  // adr x0: 0x10000000 | (0 << 29) | (0x40 << 5) | 0 = 0x10000800
  EXPECT_EQ(instr0, 0x10000800u)
    .message("Expected adr x0, #0x100, got: 0x%08X", instr0);
  EXPECT_EQ(instr1, 0xD503201Fu)
    .message("Expected NOP, got: 0x%08X", instr1);
}

// Test that adr to a forward label resolves correctly through relocateToBase.
UNIT(a64_adr_label_reloc) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  Label target = as.newLabel();
  as.adr(a64::x1, target);
  emitNops(as, 4);
  as.bind(target);
  as.nop();

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(0x10000), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);
  uint32_t instr1 = readU32LE(buf + 4);

  // target is at offset 24 (8 bytes for adr+nop + 4*4 NOPs).
  // displacement = 24, immLo = 24 & 3 = 0, immHi = (24 >> 2) = 6
  // adr x1: 0x10000000 | (0 << 29) | (6 << 5) | 1 = 0x100000C1
  EXPECT_EQ(instr0, 0x100000C1u)
    .message("Expected adr x1, #24, got: 0x%08X", instr0);
  EXPECT_EQ(instr1, 0xD503201Fu)
    .message("Expected NOP, got: 0x%08X", instr1);
}

// Test load_addr at adr boundary: displacement of exactly +1MB-4 (max positive
// for 21-bit signed) should still use adr.
UNIT(a64_load_addr_reloc_adr_max_positive) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  // Max positive 21-bit signed value: 2^20 - 1 = 0xFFFFF (1048575).
  // But must be within the signed range: max positive = (1 << 20) - 1 = 0xFFFFF.
  uint64_t target = 0xFFFFF;
  as.load_addr(a64::x6, target);

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(0), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);

  // displacement = 0xFFFFF, immLo = 0xFFFFF & 3 = 3, immHi = (0xFFFFF >> 2) & 0x7FFFF = 0x3FFFF
  // adr x6: 0x10000000 | (3 << 29) | (0x3FFFF << 5) | 6 = 0x707FFFE6
  EXPECT_EQ(instr0, 0x707FFFE6u)
    .message("Expected adr x6, #0xFFFFF, got: 0x%08X", instr0);
}

// Test load_addr at adr boundary: displacement of exactly -1MB should still
// use adr (min negative for 21-bit signed = -2^20 = -1048576 = -0x100000).
UNIT(a64_load_addr_reloc_adr_max_negative) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  // Place code at 0x200000, target at 0x100000 => displacement = -0x100000.
  uint64_t target = 0x100000;
  as.load_addr(a64::x7, target);

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(0x200000), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);

  // displacement = -0x100000, immLo = 0, immHi = 0x40000
  // adr x7: 0x10000000 | (0x40000 << 5) | 7 = 0x10800007
  EXPECT_EQ(instr0, 0x10800007u)
    .message("Expected adr x7 with displacement -0x100000, got: 0x%08X", instr0);
}

// Test load_addr just past adr range should use adrp+add.
UNIT(a64_load_addr_reloc_adr_to_adrp_boundary) {
  CodeHolder code;
  a64::Assembler as;
  setupCodeNoBase(code, as);

  // displacement = 0x100000 (1MB exactly) exceeds signed 21-bit max (0xFFFFF).
  uint64_t target = 0x100000;
  as.load_addr(a64::x8, target);

  EXPECT_EQ(code.flatten(), kErrorOk)
    .message("flatten failed");
  EXPECT_EQ(code.relocateToBase(0), kErrorOk)
    .message("relocateToBase failed");

  const Section* text = code.textSection();
  const uint8_t* buf = text->data();
  uint32_t instr0 = readU32LE(buf);

  uint32_t instr1 = readU32LE(buf + 4);

  // Should be adrp+add, not adr, since 0x100000 = 2^20 exceeds 21-bit signed max.
  // pageDelta = (0x100000 >> 12) - 0 = 0x100, pageOffset = 0
  // adrp x8: 0x90000000 | (0x40 << 5) | 8 = 0x90000808
  // add x8, x8, #0: 0x91000000 | (0 << 10) | (8 << 5) | 8 = 0x91000108
  EXPECT_EQ(instr0, 0x90000808u)
    .message("Expected adrp x8 (just past adr range), got: 0x%08X", instr0);
  EXPECT_EQ(instr1, 0x91000108u)
    .message("Expected add x8, x8, #0, got: 0x%08X", instr1);
}

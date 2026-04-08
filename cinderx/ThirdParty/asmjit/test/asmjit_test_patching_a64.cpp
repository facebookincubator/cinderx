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

// Test that bl to an absolute address that is out of ±128MB range
// gets relaxed to ldr+blr via RelocEntry.
UNIT(a64_bl_patching_absolute_addr) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  // bl to an absolute address (far away) should emit two NOPs as placeholder
  // and create a RelocEntry. The actual relaxation happens in relocateToBase().
  uint64_t farAddr = 0x100000000ULL; // 4GB away
  as.bl(Imm(farAddr));

  String hex;
  getHex(code, hex);

  // Should emit two NOPs as placeholders (will be rewritten during relocation):
  // NOP = 0xD503201F -> little-endian = 1F2003D5
  EXPECT_EQ(hex, "1F2003D51F2003D5")
    .message("Expected two NOPs as placeholder, got: %s", hex.data());

  // Verify a RelocEntry was created
  EXPECT_GT(code.relocEntries().size(), 0u)
    .message("Expected at least one reloc entry for far bl");
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

  // Forward ref always emits 8-byte region (tbz + NOP placeholder).
  // Layout: tbz(0) + NOP(4) + 4 NOPs(8-20) + target(24)
  // displacement = 24, imm14 = 6
  // tbz x0, #5, +24: b5=0, op=0, b40=00101, imm14=6, Rt=0
  // = 0x362800C0
  // LE: C0002836
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "C0002836", 8) == 0)
    .message("Expected tbz x0, #5 encoding, got: %s", hex.data());
}

// Test that tbz to a far label (beyond ±32KB) gets relaxed:
// inverted to tbnz +8, then unconditional b to target.
UNIT(a64_tbz_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // tbz x0, #5, target
  as.tbz(a64::x0, 5, target);
  // Emit enough NOPs to exceed 14-bit signed offset range (±32KB = 8192 instructions)
  emitNops(as, 8200);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // The first 4 bytes should be the inverted condition: tbnz x0, #5, +8
  // tbnz has op=1, so bit 24 is set compared to tbz
  // tbnz x0, #5, +8: imm14 = 2 (2 words = 8 bytes)
  // b5=0, op=1(tbnz), b40=00101, imm14=2, Rt=0
  // = 0x37280040
  // LE: 40002837
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "40002837", 8) == 0)
    .message("Expected tbnz x0, #5, +8 (inverted), got first 8 chars: %.8s", hex.data());

  // The second 4 bytes should be an unconditional b to target.
  // b is at offset 4. Target = 8 + 8200*4 = 32808. Displacement = 32808 - 4 = 32804.
  // imm26 = 32804/4 = 8201 = 0x2009
  // b opcode: 0x14000000 | 0x2009 = 0x14002009
  // LE: 09200014
  EXPECT(hex.size() >= 16 && memcmp(hex.data() + 8, "09200014", 8) == 0)
    .message("Expected b to target, got next 8 chars: %.8s", hex.data() + 8);
}

// Test that tbnz (inverse) also relaxes correctly.
UNIT(a64_tbnz_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // tbnz x0, #5, target
  as.tbnz(a64::x0, 5, target);
  emitNops(as, 8200);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // The first 4 bytes should be inverted to tbz x0, #5, +8
  // tbz has op=0, so bit 24 is clear compared to tbnz
  // tbz x0, #5, +8: imm14 = 2
  // = 0x36280040
  // LE: 40002836
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "40002836", 8) == 0)
    .message("Expected tbz x0, #5, +8 (inverted tbnz), got first 8 chars: %.8s", hex.data());

  // Second word: b to target (same displacement as tbz case)
  EXPECT(hex.size() >= 16 && memcmp(hex.data() + 8, "09200014", 8) == 0)
    .message("Expected b to target, got next 8 chars: %.8s", hex.data() + 8);
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

  // Forward ref emits 8-byte region (cbz + NOP placeholder).
  // Layout: cbz(0) + NOP(4) + 4 NOPs(8-20) + target(24)
  // displacement = 24, imm19 = 6
  // cbz w1, +24: sf=0, opcode=0x34, imm19=6, Rt=1
  // = 0x340000C1
  // LE: C1000034
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "C1000034", 8) == 0)
    .message("Expected cbz w1 encoding, got: %s", hex.data());
}

// Test that cbz to a far label (beyond ±1MB) gets relaxed:
// inverted to cbnz +8, then unconditional b to target.
UNIT(a64_cbz_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // cbz w1, target
  as.cbz(a64::w1, target);
  // Emit enough NOPs to exceed 19-bit signed offset range (±1MB = 262144 instructions)
  emitNops(as, 262200);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // The first 4 bytes should be the inverted condition: cbnz w1, +8
  // cbnz has bit 24 set compared to cbz
  // cbnz w1, +8: imm19 = 2 (2 words = 8 bytes)
  // = 0x35000041
  // LE: 41000035
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "41000035", 8) == 0)
    .message("Expected cbnz w1, +8 (inverted), got first 8 chars: %.8s", hex.data());

  // The second 4 bytes should be an unconditional b to target.
  // b is at offset 4. Target = 8 + 262200*4 = 1048808. Displacement = 1048808 - 4 = 1048804.
  // imm26 = 1048804/4 = 262201 = 0x40039
  // b opcode: 0x14000000 | 0x40039 = 0x14040039
  // LE: 39000414
  EXPECT(hex.size() >= 16 && memcmp(hex.data() + 8, "39000414", 8) == 0)
    .message("Expected b to target, got next 8 chars: %.8s", hex.data() + 8);
}

// Test that cbnz (inverse) also relaxes correctly.
UNIT(a64_cbnz_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // cbnz w1, target
  as.cbnz(a64::w1, target);
  emitNops(as, 262200);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // The first 4 bytes should be inverted to cbz w1, +8
  // cbz has bit 24 clear compared to cbnz
  // cbz w1, +8: imm19 = 2
  // = 0x34000041
  // LE: 41000034
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "41000034", 8) == 0)
    .message("Expected cbz w1, +8 (inverted cbnz), got first 8 chars: %.8s", hex.data());

  // Second word: b to target (same displacement as cbz case)
  EXPECT(hex.size() >= 16 && memcmp(hex.data() + 8, "39000414", 8) == 0)
    .message("Expected b to target, got next 8 chars: %.8s", hex.data() + 8);
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

  // Forward ref emits 8-byte region (b.eq + NOP placeholder).
  // Layout: b.eq(0) + NOP(4) + 4 NOPs(8-20) + target(24)
  // displacement = 24, imm19 = 6
  // b.eq +24: opcode=0x54, imm19=6, cond=0
  // = 0x540000C0
  // LE: C0000054
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "C0000054", 8) == 0)
    .message("Expected b.eq encoding, got: %s", hex.data());
}

// Test that b.eq to a far label (beyond ±1MB) gets relaxed:
// inverted to b.ne +8, then unconditional b to target.
UNIT(a64_bcond_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // b.eq target
  as.b_eq(target);
  // Emit enough NOPs to exceed 19-bit signed offset range (±1MB)
  emitNops(as, 262200);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // The first 4 bytes should be the inverted condition: b.ne +8
  // b.ne flips bit 0 of cond: eq(0) -> ne(1)
  // b.ne +8: imm19 = 2 (2 words = 8 bytes), cond = 1
  // = 0x54000041
  // LE: 41000054
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "41000054", 8) == 0)
    .message("Expected b.ne +8 (inverted), got first 8 chars: %.8s", hex.data());

  // The second 4 bytes should be an unconditional b to target.
  // Same displacement as cbz case: imm26 = 262201 = 0x40039
  // LE: 39000414
  EXPECT(hex.size() >= 16 && memcmp(hex.data() + 8, "39000414", 8) == 0)
    .message("Expected b to target, got next 8 chars: %.8s", hex.data() + 8);
}

// Test that b.ne (already inverted) also relaxes correctly.
UNIT(a64_bcond_ne_patching_out_of_range) {
  CodeHolder code;
  a64::Assembler as;
  setupCode(code, as);

  Label target = as.newLabel();
  // b.ne target
  as.b_ne(target);
  emitNops(as, 262200);
  as.bind(target);
  as.nop();

  String hex;
  getHex(code, hex);

  // The first 4 bytes should be inverted to b.eq +8
  // ne(1) -> eq(0)
  // b.eq +8: imm19 = 2, cond = 0
  // = 0x54000040
  // LE: 40000054
  EXPECT(hex.size() >= 8 && memcmp(hex.data(), "40000054", 8) == 0)
    .message("Expected b.eq +8 (inverted b.ne), got first 8 chars: %.8s", hex.data());

  // Second word: b to target (same displacement)
  EXPECT(hex.size() >= 16 && memcmp(hex.data() + 8, "39000414", 8) == 0)
    .message("Expected b to target, got next 8 chars: %.8s", hex.data() + 8);
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

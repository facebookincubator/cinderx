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

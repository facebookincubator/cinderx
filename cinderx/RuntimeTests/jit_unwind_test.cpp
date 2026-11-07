// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/config.h"
#include "cinderx/Jit/jit_unwind.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace cinderx::jit {
namespace {

uint32_t readU32(const uint8_t* p) {
  uint32_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

uint64_t readU64(const uint8_t* p) {
  uint64_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

uint32_t readUleb(const uint8_t*& p) {
  uint32_t result = 0;
  int shift = 0;
  while (true) {
    uint8_t byte = *p++;
    result |= static_cast<uint32_t>(byte & 0x7f) << shift;
    if ((byte & 0x80) == 0) {
      break;
    }
    shift += 7;
  }
  return result;
}

int32_t readSleb(const uint8_t*& p) {
  int32_t result = 0;
  int shift = 0;
  uint8_t byte;
  do {
    byte = *p++;
    result |= static_cast<int32_t>(byte & 0x7f) << shift;
    shift += 7;
  } while (byte & 0x80);
  if ((shift < 32) && (byte & 0x40) != 0) {
    result |= -1 << shift;
  }
  return result;
}

struct ParsedEhFrame {
  uint32_t cie_length{0};
  uint8_t cie_version{0};
  uint32_t code_alignment{0};
  int32_t data_alignment{0};
  uint8_t ra_reg{0};
  uint8_t fde_encoding{0};
  std::vector<uint8_t> cie_program;
  uint32_t fde_length{0};
  uint32_t cie_pointer{0};
  uintptr_t initial_location{0};
  uintptr_t address_range{0};
  std::vector<uint8_t> fde_program;
  size_t fde_offset{0};
};

// Minimal .eh_frame reader written from the DWARF spec, independent of the
// builder under test.
ParsedEhFrame parseEhFrame(const uint8_t* buffer, size_t size) {
  ParsedEhFrame parsed;
  const uint8_t* p = buffer;
  parsed.cie_length = readU32(p);
  p += 4;
  EXPECT_EQ(readU32(p), 0u); // CIE id
  p += 4;
  parsed.cie_version = *p++;
  // Augmentation string "zR\0".
  EXPECT_STREQ(reinterpret_cast<const char*>(p), "zR");
  p += 3;
  parsed.code_alignment = readUleb(p);
  parsed.data_alignment = readSleb(p);
  parsed.ra_reg = *p++;
  EXPECT_EQ(readUleb(p), 1u); // Augmentation data length.
  parsed.fde_encoding = *p++;
  // The initial program runs to the end of the CIE section, including any
  // trailing DW_CFA_nop (0) alignment padding.
  const uint8_t* cie_end = buffer + 4 + parsed.cie_length;
  parsed.cie_program.assign(p, cie_end);
  p = cie_end;
  parsed.fde_offset = static_cast<size_t>(p - buffer);
  parsed.fde_length = readU32(p);
  p += 4;
  parsed.cie_pointer = readU32(p);
  p += 4;
  parsed.initial_location = static_cast<uintptr_t>(readU64(p));
  p += 8;
  parsed.address_range = static_cast<uintptr_t>(readU64(p));
  p += 8;
  EXPECT_EQ(*p++, 0); // Augmentation data length.
  const uint8_t* fde_end = buffer + parsed.fde_offset + 4 + parsed.fde_length;
  EXPECT_LE(static_cast<size_t>(fde_end - buffer), size);
  parsed.fde_program.assign(p, fde_end);
  return parsed;
}

// A call frame program matches when it starts with `expected` and the rest
// is DW_CFA_nop padding.  Padding cannot simply be stripped from the parsed
// program: a trailing zero operand (e.g. a CFA offset of 0) looks the same.
void expectCfaProgram(
    const std::vector<uint8_t>& actual,
    const std::vector<uint8_t>& expected) {
  ASSERT_GE(actual.size(), expected.size());
  EXPECT_EQ(
      std::vector<uint8_t>(
          actual.begin(),
          actual.begin() + static_cast<std::ptrdiff_t>(expected.size())),
      expected);
  EXPECT_EQ(
      std::vector<uint8_t>(
          actual.begin() + static_cast<std::ptrdiff_t>(expected.size()),
          actual.end()),
      std::vector<uint8_t>(actual.size() - expected.size(), 0));
}

void someFunction() {}

// Restores a JIT config flag on scope exit, including when an assertion
// in the test fails.
struct RestoreConfigFlag {
  explicit RestoreConfigFlag(bool& flag) : flag_(flag), saved_(flag) {}
  ~RestoreConfigFlag() {
    flag_ = saved_;
  }

  bool& flag_;
  bool saved_;
};

} // namespace

#if defined(__x86_64__) || defined(__aarch64__)

TEST(JitUnwindTest, BuildEhFrameDescribesFramePointerFrame) {
  const uintptr_t code_addr = 0x100000000ULL;
  constexpr size_t kCodeSize = 0x1234;
  const size_t size = jitUnwindEhFrameSize();
  ASSERT_GT(size, 0u);
  std::vector<uint8_t> buffer(size, 0xcc);
  const size_t written = buildJitUnwindEhFrame(
      buffer.data(), buffer.size(), (const void*)code_addr, kCodeSize);
  ASSERT_EQ(written, size);
  // Padding bytes are DW_CFA_nop (0).
  for (size_t i = written; i < buffer.size(); ++i) {
    EXPECT_EQ(buffer[i], 0xcc);
  }

  ParsedEhFrame parsed = parseEhFrame(buffer.data(), buffer.size());
  EXPECT_EQ(parsed.cie_version, 1);
  EXPECT_EQ(parsed.data_alignment, -8);
  EXPECT_EQ(parsed.fde_encoding, 0x00); // Absolute pointer encoding.
  // The CIE pointer is the distance from its own field back to the CIE,
  // i.e. the FDE offset plus the FDE's length field.
  EXPECT_EQ(parsed.cie_pointer, parsed.fde_offset + 4);
  EXPECT_EQ(parsed.initial_location, code_addr);
  EXPECT_EQ(parsed.address_range, kCodeSize);

#if defined(__x86_64__)
  EXPECT_EQ(parsed.code_alignment, 1u);
  EXPECT_EQ(parsed.ra_reg, 16);
  // CIE: def_cfa SP+8, RA saved at CFA-8.
  expectCfaProgram(parsed.cie_program, {0x0c, 0x07, 0x08, 0x90, 0x01});
  // FDE: push rbp; CFA=SP+16, rbp at CFA-16; mov rbp,rsp; CFA=rbp+16.
  expectCfaProgram(
      parsed.fde_program, {0x41, 0x0e, 0x10, 0x86, 0x02, 0x43, 0x0d, 0x06});
#elif defined(__aarch64__)
  EXPECT_EQ(parsed.code_alignment, 4u);
  EXPECT_EQ(parsed.ra_reg, 30);
  // CIE: def_cfa SP+0.
  expectCfaProgram(parsed.cie_program, {0x0c, 0x1f, 0x00});
  // FDE: stp fp,lr; CFA=SP+16, fp at CFA-16, lr at CFA-8; mov fp,sp; CFA=fp+16.
  expectCfaProgram(
      parsed.fde_program,
      {0x41, 0x0e, 0x10, 0x9d, 0x02, 0x9e, 0x01, 0x41, 0x0d, 0x1d});
#endif
}

TEST(JitUnwindTest, BuildEhFrameRejectsBadInputs) {
  std::vector<uint8_t> buffer(jitUnwindEhFrameSize());
  const void* code = (const void*)(uintptr_t)0x1000;
  EXPECT_EQ(buildJitUnwindEhFrame(nullptr, buffer.size(), code, 16), 0u);
  EXPECT_EQ(
      buildJitUnwindEhFrame(buffer.data(), buffer.size(), nullptr, 16), 0u);
  EXPECT_EQ(buildJitUnwindEhFrame(buffer.data(), buffer.size(), code, 0), 0u);
  EXPECT_EQ(buildJitUnwindEhFrame(buffer.data(), 1, code, 16), 0u);
  EXPECT_EQ(buildJitUnwindEhFrame(buffer.data(), 0, code, 16), 0u);
}

#else // Unsupported architecture: no unwind info is generated.

TEST(JitUnwindTest, UnsupportedArchitectureGeneratesNothing) {
  EXPECT_EQ(jitUnwindEhFrameSize(), 0u);
  uint8_t buffer[16] = {};
  EXPECT_EQ(
      buildJitUnwindEhFrame(buffer, sizeof(buffer), (const void*)0x1000, 16),
      0u);
  EXPECT_EQ(registerJitUnwindInfo((const void*)0x1000, 16), nullptr);
}

#endif

#if defined(__linux__) && defined(__ELF__) && \
    (defined(__x86_64__) || defined(__aarch64__))

TEST(JitUnwindTest, RegisterAndUnregisterRoundTrip) {
  const void* code = (const void*)&someFunction;
  constexpr size_t kCodeSize = 64;
  // Libgcc registration is off by default; turn it on so a registration
  // is taken even when HP libunwind is not linked into this binary.
  RestoreConfigFlag restore(
      getMutableConfig().unwind.libgcc_frame_registration);
  (void)restore;
  getMutableConfig().unwind.libgcc_frame_registration = true;
  std::unique_ptr<JitUnwindRegistration> registration =
      registerJitUnwindInfo(code, kCodeSize);
  ASSERT_NE(registration, nullptr);
  EXPECT_NE(registration->eh_frame, nullptr);
  EXPECT_EQ(registration->eh_frame_size, jitUnwindEhFrameSize());
  EXPECT_GT(registration->eh_frame_size, 0u);

  // The libunwind dynamic registration describes exactly this code range
  // with one table entry pointing at the FDE in the generated frame.
  const JitUnwindDynInfo& dyn = registration->dyn_info;
  EXPECT_EQ(dyn.start_ip, (uintptr_t)code);
  EXPECT_EQ(dyn.end_ip, (uintptr_t)code + kCodeSize);
  EXPECT_EQ(dyn.format, JitUnwindDynInfo::kFormatTable);
  EXPECT_EQ(dyn.u.table.segbase, (uintptr_t)code);
  EXPECT_NE(dyn.u.table.table_data, nullptr);
  const JitUnwindDebugFrameList* frame_list =
      (const JitUnwindDebugFrameList*)dyn.u.table.table_data;
  EXPECT_EQ(frame_list->start, (uintptr_t)code);
  EXPECT_EQ(frame_list->end, (uintptr_t)code + kCodeSize);
  EXPECT_EQ(
      (const void*)frame_list->debug_frame,
      (const void*)registration->eh_frame.get());
  EXPECT_EQ(frame_list->debug_frame_size, registration->eh_frame_size);
  ASSERT_NE(frame_list->index, nullptr);
  EXPECT_EQ(frame_list->index_size, sizeof(JitUnwindTableEntry));
  EXPECT_EQ(frame_list->index[0].start_ip_offset, 0);
  ParsedEhFrame parsed =
      parseEhFrame(registration->eh_frame.get(), registration->eh_frame_size);
  EXPECT_EQ(frame_list->index[0].fde_offset, (int32_t)parsed.fde_offset);

  unregisterJitUnwindInfo(std::move(registration));
  // Unregistering nothing is a no-op.
  unregisterJitUnwindInfo(nullptr);
  EXPECT_EQ(registerJitUnwindInfo(nullptr, kCodeSize), nullptr);
  EXPECT_EQ(registerJitUnwindInfo(code, 0), nullptr);
}

TEST(JitUnwindTest, LibgccFrameRegistrationCanBeDisabled) {
  const void* code = (const void*)&someFunction;
  constexpr size_t kCodeSize = 64;
  RestoreConfigFlag restore(
      getMutableConfig().unwind.libgcc_frame_registration);
  (void)restore;

  // On by default.
  EXPECT_TRUE(getConfig().unwind.libgcc_frame_registration);

  // With the flag off, no `__register_frame` call happens.  HP libunwind
  // registration is unaffected: when libunwind is linked the
  // registration still comes back, dyn-registered only; when it is not
  // linked there is no unwinder left and registration is null.
  getMutableConfig().unwind.libgcc_frame_registration = false;
  std::unique_ptr<JitUnwindRegistration> disabled =
      registerJitUnwindInfo(code, kCodeSize);
  if (disabled != nullptr) {
    EXPECT_FALSE(disabled->libgcc_registered);
    EXPECT_TRUE(disabled->dyn_registered);
    unregisterJitUnwindInfo(std::move(disabled));
  }

  // With the flag on, libgcc registration runs.
  getMutableConfig().unwind.libgcc_frame_registration = true;
  std::unique_ptr<JitUnwindRegistration> enabled =
      registerJitUnwindInfo(code, kCodeSize);
  ASSERT_NE(enabled, nullptr);
  EXPECT_TRUE(enabled->libgcc_registered);
  unregisterJitUnwindInfo(std::move(enabled));
}

// Permanent registrations live in the registry owned by `ModuleState`,
// which only exists once the cinderx module is imported.  This test
// therefore runs under `RuntimeTest`, whose setup imports cinderx; a
// plain `TEST` runs with a null module state, so registration has
// nowhere to root and returns null.
class JitUnwindPermanentRegistryTest : public RuntimeTest {};

TEST_F(JitUnwindPermanentRegistryTest, PermanentRegistrationIsRetained) {
  // Trampoline registrations are kept for the lifetime of the module
  // state; the registry owns them, so there is nothing to unregister
  // here.  TearDown finalizes Python, destroying the module state and
  // unregistering the registration.
  ASSERT_NE(jitUnwindPermanentRegistry(), nullptr);
  // Libgcc registration is off by default; turn it on so the registration
  // is taken even when HP libunwind is not linked into this binary.
  RestoreConfigFlag restore(
      getMutableConfig().unwind.libgcc_frame_registration);
  (void)restore;
  getMutableConfig().unwind.libgcc_frame_registration = true;
  JitUnwindRegistration* registration =
      registerPermanentJitUnwindInfo((const void*)&someFunction, 64);
  ASSERT_NE(registration, nullptr);
  EXPECT_EQ(registration->dyn_info.start_ip, (uintptr_t)&someFunction);
  EXPECT_EQ(registerPermanentJitUnwindInfo(nullptr, 64), nullptr);
  EXPECT_EQ(
      registerPermanentJitUnwindInfo((const void*)&someFunction, 0), nullptr);
}

#endif

} // namespace cinderx::jit

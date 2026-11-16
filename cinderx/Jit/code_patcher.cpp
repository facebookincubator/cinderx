// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/code_patcher.h"

#include "cinderx/Common/atomic_ref.h"
#include "cinderx/Common/define.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/write_protect.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace cinderx::jit {

namespace {

static_assert(
    sizeof(CodePatcher) == 24,
    "CodePatcher should be kept small as there could be many per function");

#if defined(CINDER_X86_64)
// 5-byte nop - https://www.felixcloutier.com/x86/nop
//
// Asmjit supports multi-byte nops but for whatever reason we can't get it to
// emit the 5-byte version.
constexpr auto kJmpNopBytes =
    std::to_array<uint8_t>({0x0f, 0x1f, 0x44, 0x00, 0x00});
#elif defined(CINDER_AARCH64)
constexpr auto kJmpNopBytes = std::to_array<uint8_t>({0x1f, 0x20, 0x03, 0xd5});
#else
CINDER_UNSUPPORTED
constexpr auto kJmpNopBytes = std::to_array<uint8_t>({0x00});
#endif

// Compute a signed 32-bit jump displacement.
int32_t jumpDisplacement(uintptr_t from, uintptr_t to) {
  auto disp = static_cast<intptr_t>(to - from);

#if defined(CINDER_X86_64)
  disp -= kJmpNopBytes.size();
#endif

  JIT_CHECK(
      fitsSignedInt<32>(disp),
      "Can't encode jump from {:#x} to {:#x} as relative",
      from,
      to);
  return static_cast<int32_t>(disp);
}

// Given the starting address and displacement operand of a jump instruction,
// resolve it to a target address.
uintptr_t resolveDisplacement(uintptr_t from, int32_t displacement) {
  auto disp =
      from + static_cast<uintptr_t>(static_cast<intptr_t>(displacement));

#if defined(CINDER_X86_64)
  disp += kJmpNopBytes.size();
#endif

  return disp;
}

// Encode a relative call (x86 CALL rel32 / aarch64 BL) from `from` to `target`.
std::array<uint8_t, kJmpNopBytes.size()> encodeCall(
    uintptr_t from,
    uintptr_t target) {
  int32_t disp = jumpDisplacement(from, target);
  std::array<uint8_t, kJmpNopBytes.size()> buf{};

#if defined(CINDER_X86_64)
  // CALL rel32 - https://www.felixcloutier.com/x86/call
  buf[0] = 0xe8;
  std::memcpy(buf.data() + 1, &disp, sizeof(disp));
#elif defined(CINDER_AARCH64)
  JIT_CHECK(
      disp % 4 == 0, "Call displacement must be a multiple of 4, got {}", disp);
  disp /= 4;
  JIT_CHECK(fitsSignedInt<26>(disp), "Not enough bits to encode relative call");
  // BL imm26.
  uint32_t insn = 0x94000000 | (static_cast<uint32_t>(disp) & 0x03ffffff);
  std::memcpy(buf.data(), &insn, sizeof(uint32_t));
#else
  (void)disp;
  CINDER_UNSUPPORTED
#endif

  return buf;
}

// Encode a relative jump (x86 JMP rel32 / aarch64 B) from `from` to `target`.
std::array<uint8_t, kJmpNopBytes.size()> encodeJump(
    uintptr_t from,
    uintptr_t target) {
  int32_t disp = jumpDisplacement(from, target);
  std::array<uint8_t, kJmpNopBytes.size()> buf{};

#if defined(CINDER_X86_64)
  buf[0] = 0xe9;
  std::memcpy(buf.data() + 1, &disp, sizeof(disp));
#elif defined(CINDER_AARCH64)
  JIT_CHECK(
      disp % 4 == 0, "Jump displacement must be a multiple of 4, got {}", disp);
  disp /= 4;
  JIT_CHECK(fitsSignedInt<26>(disp), "Not enough bits to encode relative jump");
  uint32_t insn = 0x14000000 | (static_cast<uint32_t>(disp) & 0x03ffffff);
  std::memcpy(buf.data(), &insn, sizeof(uint32_t));
#else
  (void)disp;
  CINDER_UNSUPPORTED
#endif

  return buf;
}

// Overwrite up to 8 bytes of live code at `dst` and flush the instruction
// cache.  In free-threaded x86 builds this is a single atomic 8-byte store, so
// `dst` must be 8-byte aligned; otherwise a thread executing the instruction
// could observe a torn update.
void writeCode(uint8_t* dst, std::span<const uint8_t> bytes) {
  JIT_CHECK(bytes.size() <= 8, "Can only patch up to 8 bytes at a time");

  if constexpr (kFreeThreadedBuild && kBuildArch == Arch::kX86_64) {
    JIT_CHECK(
        reinterpret_cast<uintptr_t>(dst) % 8 == 0,
        "Not 8-byte aligned: {}",
        static_cast<const void*>(dst));
    // Read the existing word atomically (not via memcpy): a concurrently
    // executing thread may be patching the same location with the atomic store
    // below, and mixing a non-atomic read with that store would be a data race.
    // We only modify the patched bytes; the remaining (high) bytes belong to
    // the following, never-patched instruction, so reading them back is safe.
    std::atomic_ref<uint64_t> word{*reinterpret_cast<uint64_t*>(dst)};
    uint64_t qword = word.load(std::memory_order_relaxed);
    std::memcpy(&qword, bytes.data(), bytes.size());
    word.store(qword, std::memory_order_relaxed);
  } else if constexpr (kFreeThreadedBuild && kBuildArch == Arch::kAarch64) {
    // A64 instructions are a fixed 4 bytes; the architecture only guarantees a
    // concurrently-executing core sees an all-or-nothing update for a single
    // naturally-aligned 32-bit store of a CMODX-safe instruction
    // (B/BL/NOP/...). memcpy is not guaranteed to be a single store, so use an
    // aligned 4-byte atomic store.  All patch sites we modify are one 4-byte
    // instruction.
    JIT_CHECK(
        bytes.size() == 4 && reinterpret_cast<uintptr_t>(dst) % 4 == 0,
        "Not a 4-byte aligned word: {}",
        static_cast<const void*>(dst));
    uint32_t word;
    std::memcpy(&word, bytes.data(), sizeof(word));
    std::atomic_ref<uint32_t>{*reinterpret_cast<uint32_t*>(dst)}.store(
        word, std::memory_order_relaxed);
  } else {
    // A plain non-atomic copy is only safe when nothing executes the code
    // concurrently -- i.e. when the GIL serializes patching against execution.
    JIT_CHECK(
        !kFreeThreadedBuild,
        "writeCode: non-atomic patch on an unhandled free-threaded "
        "architecture");
    std::memcpy(dst, bytes.data(), bytes.size());
  }

  // No-op on x86 (caches are coherent); performs the required i-cache
  // maintenance for self-modifying code on aarch64.
  __builtin___clear_cache(
      reinterpret_cast<char*>(dst),
      reinterpret_cast<char*>(dst) + bytes.size());
}

// Decode the signed relative displacement operand of the CALL/JMP at `addr`.
//
// On free-threaded builds a concurrent thread may be patching this call site
// with a single atomic store (see writeCode), so the operand is read back with
// a matching atomic load to avoid a torn read.  This makes the decode safe
// without the world being stopped.
int32_t decodeDisplacement(uintptr_t addr) {
  int32_t disp = 0;

#if defined(CINDER_X86_64)
  if constexpr (kFreeThreadedBuild) {
    // The patch is a single atomic 8-byte store to an 8-byte aligned site, so
    // load the whole word atomically and extract the disp32 operand at
    // offset 1.
    JIT_CHECK(addr % 8 == 0, "Not 8-byte aligned: {:#x}", addr);
    uint64_t qword =
        std::atomic_ref<uint64_t>{*reinterpret_cast<uint64_t*>(addr)}.load(
            std::memory_order_relaxed);
    std::memcpy(
        &disp, reinterpret_cast<const uint8_t*>(&qword) + 1, sizeof(disp));
  } else {
    std::memcpy(
        &disp, reinterpret_cast<const uint8_t*>(addr) + 1, sizeof(disp));
  }
#elif defined(CINDER_AARCH64)
  uint32_t insn;
  if constexpr (kFreeThreadedBuild) {
    // A64 instructions are a naturally-aligned 4-byte word patched with a
    // single atomic store; read it back the same way.
    JIT_CHECK(addr % 4 == 0, "Not 4-byte aligned: {:#x}", addr);
    insn = std::atomic_ref<uint32_t>{*reinterpret_cast<uint32_t*>(addr)}.load(
        std::memory_order_relaxed);
  } else {
    std::memcpy(&insn, reinterpret_cast<const uint8_t*>(addr), sizeof(insn));
  }
  // Extract 26-bit immediate and sign-extend to 32 bits.
  disp = static_cast<int32_t>(insn << 6) >> 6;
  disp *= 4;
#else
  (void)addr;
  CINDER_UNSUPPORTED
#endif

  return disp;
}

} // namespace

void CodePatcher::link(uintptr_t patchpoint, std::span<const uint8_t> data) {
  JIT_CHECK(!isLinked(), "Trying to re-link a patcher");

  patchpoint_ = reinterpret_cast<uint8_t*>(patchpoint);

  JIT_CHECK(
      data.size() <= data_.size(),
      "Trying to link a patch point with {} bytes of data but only {} are "
      "supported",
      data.size(),
      data_.size());

  std::memcpy(data_.data(), data.data(), data.size());
  flags_.data_len = data.size();

  onLink();
}

void CodePatcher::patch() {
  JIT_CHECK(isLinked(), "Trying to patch a patcher that isn't linked");
  JIT_DLOG("Patching DeoptPatchPoint at {}", static_cast<void*>(patchpoint_));

  swap();

  flags_.is_patched = true;
  onPatch();
}

void CodePatcher::unpatch() {
  JIT_CHECK(isLinked(), "Trying to unpatch a patcher that isn't linked");
  JIT_DLOG("Unpatching DeoptPatchPoint at {}", static_cast<void*>(patchpoint_));

  swap();

  flags_.is_patched = false;
  onUnpatch();
}

bool CodePatcher::isLinked() const {
  return patchpoint_ != nullptr;
}

bool CodePatcher::isPatched() const {
  return flags_.is_patched;
}

void CodePatcher::reset() {
  JIT_CHECK(
      (flags_byte_ & lockBit()) == 0,
      "Cannot reset a patcher while it is being patched");
  patchpoint_ = nullptr;
  // Clearing data_len as well keeps storedBytes() empty until the next
  // link() overwrites data_ and sets it again.
  flags_byte_ = 0;
}

uint8_t* CodePatcher::patchpoint() const {
  return patchpoint_;
}

std::span<const uint8_t> CodePatcher::storedBytes() const {
  return std::span{data_.data(), flags_.data_len};
}

void CodePatcher::swap() {
  SwapLockGuard lock{*this};

  // The patchpoint lives in JIT code, which on Apple Silicon is only writable
  // for a thread that has turned write protection off.
  jitEnableWriting();

  // On x86 the patchpoint is up to 7 bytes (aligned to 8 bytes by the code
  // generator). However, we work with 8 bytes here as that should be an
  // atomically writable size on x86.
  if constexpr (kFreeThreadedBuild && kBuildArch == Arch::kX86_64) {
    static_assert(sizeof(uint64_t) >= sizeof(data_));
    static_assert(AtomicRef<uint64_t>::kIsAlwaysLockFree);
    JIT_CHECK(
        reinterpret_cast<uintptr_t>(patchpoint_) % 8 == 0,
        "Not 8-byte aligned");
    uint64_t qword;
    std::memcpy(&qword, patchpoint_, sizeof(qword));

    auto* qword_bytes = reinterpret_cast<uint8_t*>(&qword);
    std::swap_ranges(qword_bytes, qword_bytes + flags_.data_len, data_.data());

    auto* patchpoint_qword = reinterpret_cast<uint64_t*>(patchpoint_);
    AtomicRef<uint64_t>{*patchpoint_qword}.store(
        qword, std::memory_order_relaxed);
  } else {
    decltype(data_) temp;
    std::memcpy(temp.data(), patchpoint_, flags_.data_len);
    std::memcpy(patchpoint_, data_.data(), flags_.data_len);
    std::memcpy(data_.data(), temp.data(), flags_.data_len);
  }

  jitEnableExecuting(patchpoint_, flags_.data_len);

  // Flush the instruction cache so the core that executes the patchpoint next
  // sees the new bytes rather than a stale decoding of the old ones.  Note for
  // x86 this is a no-op as caches are coherent; aarch64 needs it even
  // single-threaded.  (On Apple Silicon jitEnableExecuting() has already done
  // this, but it is cheap and this keeps the non-Apple aarch64 case honest.)
  __builtin___clear_cache(
      reinterpret_cast<char*>(patchpoint_),
      reinterpret_cast<char*>(patchpoint_) + flags_.data_len);
}

// We use a custom spin-lock implementation as I'm not aware of a generic way of
// implementing a lock where the mutex is bit-packed with other data. This
// should be fine as the critical section is a short, slow-path, and should
// only happen very rarely.
CodePatcher::SwapLockGuard::SwapLockGuard(CodePatcher& patcher)
    : patcher_(patcher) {
  if constexpr (!kFreeThreadedBuild) {
    return;
  }
  AtomicRef<uint8_t> ref{patcher_.flags_byte_};
  while (true) {
    uint8_t expected = ref.load(std::memory_order_relaxed);
    if (!(expected & lockBit()) &&
        ref.compareExchangeWeak(
            expected,
            expected | lockBit(),
            std::memory_order_acquire,
            std::memory_order_relaxed)) {
      return;
    }
  }
}

CodePatcher::SwapLockGuard::~SwapLockGuard() {
  if constexpr (!kFreeThreadedBuild) {
    return;
  }
  AtomicRef<uint8_t>{patcher_.flags_byte_}.fetchAnd(
      static_cast<uint8_t>(~lockBit()), std::memory_order_release);
}

JumpPatcher::JumpPatcher() {
  // Initializes to a nop.
  std::memcpy(data_.data(), kJmpNopBytes.data(), kJmpNopBytes.size());
  flags_.data_len = kJmpNopBytes.size();
}

void JumpPatcher::linkJump(uintptr_t patchpoint, uintptr_t jump_target) {
  auto disp = jumpDisplacement(patchpoint, jump_target);
  std::array<uint8_t, kJmpNopBytes.size()> buf{};

#if defined(CINDER_X86_64)
  // 32 bit relative jump - https://www.felixcloutier.com/x86/jmp
  buf[0] = 0xe9;
  std::memcpy(buf.data() + 1, &disp, sizeof(disp));
#elif defined(CINDER_AARCH64)
  JIT_CHECK(
      disp % 4 == 0, "Jump displacement must be a multiple of 4, got {}", disp);

  disp /= 4;
  JIT_CHECK(fitsSignedInt<26>(disp), "Not enough bits to encode relative jump");

  uint32_t insn = 0x14000000 | (static_cast<uint32_t>(disp) & 0x03ffffff);
  std::memcpy(buf.data(), &insn, sizeof(uint32_t));
#else
  (void)disp;
  CINDER_UNSUPPORTED
#endif

  link(patchpoint, buf);
}

uint8_t* JumpPatcher::jumpTarget() const {
  JIT_CHECK(
      isLinked(), "Can't compute jump target before JumpPatcher is linked");

  std::span<const uint8_t> bytes = storedBytes();
  JIT_CHECK(
      bytes.size() == kJmpNopBytes.size(),
      "Must have linked a {}-byte jump instruction into a JumpPatcher",
      kJmpNopBytes.size());

  int32_t disp = 0;

#if defined(CINDER_X86_64)
  std::memcpy(&disp, bytes.data() + 1, bytes.size() - 1);
#elif defined(CINDER_AARCH64)
  uint32_t insn = 0;
  std::memcpy(&insn, bytes.data(), bytes.size());
  // Extract 26-bit immediate and sign-extend to 32 bits.
  disp = static_cast<int32_t>(insn << 6) >> 6;
  disp *= 4;
#else
  CINDER_UNSUPPORTED
#endif

  return reinterpret_cast<uint8_t*>(
      resolveDisplacement(reinterpret_cast<uintptr_t>(patchpoint_), disp));
}

void patchCall(uintptr_t call_site, uintptr_t target) {
  std::array<uint8_t, kJmpNopBytes.size()> buf = encodeCall(call_site, target);
  writeCode(reinterpret_cast<uint8_t*>(call_site), buf);
}

void patchJump(uintptr_t entry, uintptr_t target) {
  std::array<uint8_t, kJmpNopBytes.size()> buf = encodeJump(entry, target);
  writeCode(reinterpret_cast<uint8_t*>(entry), buf);
}

bool inRelBranchRange(uintptr_t from, uintptr_t to) {
#if defined(CINDER_X86_64)
  intptr_t disp = static_cast<intptr_t>(to - from) -
      static_cast<intptr_t>(kJmpNopBytes.size());
  return fitsSignedInt<32>(disp);
#elif defined(CINDER_AARCH64)
  intptr_t disp = static_cast<intptr_t>(to - from);
  return (disp % 4 == 0) && fitsSignedInt<26>(disp / 4);
#else
  (void)from;
  (void)to;
  return false;
#endif
}

uintptr_t decodeCallTarget(uintptr_t addr) {
  return resolveDisplacement(addr, decodeDisplacement(addr));
}

uintptr_t callSiteFromReturnAddress(uintptr_t return_address) {
  // A relative call/jump is the same size as kJmpNopBytes on each architecture
  // (5 bytes on x86-64, 4 bytes on aarch64).
  return return_address - kJmpNopBytes.size();
}

namespace {
// Published once by the codegen factory; read on hot/cold paths thereafter.
std::atomic<void*> g_jit_to_jit_link_trampoline{nullptr};
std::atomic<void*> g_jit_to_jit_relink_trampoline{nullptr};
} // namespace

void* jitToJitLinkTrampoline() {
  return g_jit_to_jit_link_trampoline.load(std::memory_order_acquire);
}

void* jitToJitRelinkTrampoline() {
  return g_jit_to_jit_relink_trampoline.load(std::memory_order_acquire);
}

void setJitToJitTrampolines(void* link, void* relink) {
  g_jit_to_jit_link_trampoline.store(link, std::memory_order_release);
  g_jit_to_jit_relink_trampoline.store(relink, std::memory_order_release);
}

} // namespace cinderx::jit

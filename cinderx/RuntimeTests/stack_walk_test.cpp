// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/stack_walk.h"

#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace cinderx;

namespace cinderx {

// A synthetic chain of frame records standing in for a real stack. Records sit
// at increasing, pointer-aligned addresses so the cursor's plausibility check
// accepts each step, and the outermost record's `frame_pointer` is null to
// terminate the chain.
class FakeStack {
 public:
  explicit FakeStack(size_t depth) : records_(depth) {
    for (size_t i = 0; i < depth; i++) {
      records_[i].frame_pointer = i + 1 < depth ? &records_[i + 1] : nullptr;
      records_[i].return_address = returnAddressFor(i);
    }
  }

  const StackFrame* innermost() const {
    return records_.data();
  }

  const StackFrame* at(size_t i) const {
    return &records_[i];
  }

  size_t depth() const {
    return records_.size();
  }

  // Distinct, never-dereferenced stand-ins for the address executing in each
  // frame.
  static const void* returnAddressFor(size_t i) {
    return reinterpret_cast<const void*>(uintptr_t{0x1000} + i * 0x10);
  }

 private:
  std::vector<StackFrame> records_;
};

// A frame as (frame pointer, address executing in it). Compared as pairs so
// gtest can print mismatches without an operator== on StackFrame.
using FramePair = std::pair<const void*, const void*>;

// What a completed walk delivered: the frames, in order, and the number of
// batches it took to hand them over.
struct WalkResult {
  std::vector<FramePair> frames;
  size_t batches = 0;
};

struct StackWalkTestAccess {
  static constexpr size_t kBatchSize = StackWalk::kBatchSize;

  // The PC the fabricated context is interrupted at.
  static const void* interruptedPc() {
    return reinterpret_cast<const void*>(uintptr_t{0xbeef0});
  }
};

} // namespace cinderx

namespace {

constexpr size_t kBatchSize = StackWalkTestAccess::kBatchSize;

// A thread that parks in a known-deep call chain until it is told to stop, so
// that a cross-thread walk has something stable to find. `extra_frames`
// recursive frames are pushed on top of it, for walks that need a stack of a
// chosen length.
class SpinningThread {
 public:
  explicit SpinningThread(size_t extra_frames = 0) {
    thread_ = std::thread{[this, extra_frames] { spin(extra_frames); }};
    // The walk needs the thread actually inside spin(), not merely spawned.
    while (!ready_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

  ~SpinningThread() {
    stop_.store(true, std::memory_order_release);
    thread_.join();
  }

  StackWalk::ThreadId id() {
    return thread_.native_handle();
  }

 private:
  __attribute__((noinline)) void spin(size_t remaining) {
    if (remaining > 0) {
      spin(remaining - 1);
      // Stops this being a tail call, which would collapse the whole chain
      // back down into one frame.
      asm volatile("");
      return;
    }
    ready_.store(true, std::memory_order_release);
    while (!stop_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    // Keeps the compiler from tail-calling or folding this frame away.
    asm volatile("");
  }

  std::atomic<bool> ready_{false};
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// A thread state is a plain struct here: only `thread_id` is ever read, and
// reading it needs no live interpreter.
PyThreadState fakeThreadState(StackWalk::ThreadId thread) {
  PyThreadState tstate = {};
  std::memcpy(&tstate.thread_id, &thread, sizeof(thread));
  return tstate;
}

// The frames a walk of `stack` should produce: the innermost record paired with
// the interrupted PC, then every caller paired with the address its callee
// returns to.
std::vector<FramePair> expectedFrames(const FakeStack& stack) {
  std::vector<FramePair> expected;
  expected.emplace_back(
      stack.innermost(), StackWalkTestAccess::interruptedPc());
  for (size_t i = 1; i < stack.depth(); i++) {
    expected.emplace_back(stack.at(i), FakeStack::returnAddressFor(i - 1));
  }
  return expected;
}

TEST(StackWalkCursorTest, StepFollowsAChainToItsEnd) {
  FakeStack stack{4};
  StackWalk::Cursor cursor{stack.innermost()};

  EXPECT_EQ(cursor.returnAddress(), FakeStack::returnAddressFor(0));
  for (size_t i = 0; i + 1 < stack.depth(); i++) {
    EXPECT_EQ(cursor.step(), stack.at(i + 1)) << "at frame " << i;
  }
  EXPECT_EQ(cursor.step(), nullptr);
}

TEST(StackWalkCursorTest, StepEndsImmediatelyOnASingleFrameChain) {
  FakeStack stack{1};
  StackWalk::Cursor cursor{stack.innermost()};

  EXPECT_EQ(cursor.step(), nullptr);
}

// A resumed JIT generator runs with the frame-pointer register aimed at its
// heap-allocated spill data, so the chain leaves the stack for exactly one
// record. Recognising that costs a read of the off-stack record, which is the
// one place the walker is obliged to touch an address it has no reason to
// trust. Laid out with the generator's record below the anchor so it fails the
// on-stack test, and its caller above, so it rejoins.
TEST(StackWalkCursorTest, StepFollowsAGeneratorRecordOffTheStackAndBack) {
  std::vector<StackFrame> r(3);
  const auto* generator = &r[0];
  const auto* innermost = &r[1];
  const auto* caller = &r[2];
  r[1].frame_pointer = generator;
  r[0].frame_pointer = caller;
  r[2].frame_pointer = nullptr;

  StackWalk::Cursor cursor{innermost};
  EXPECT_EQ(cursor.step(), generator);
  EXPECT_EQ(cursor.step(), caller);
  EXPECT_EQ(cursor.step(), nullptr);
}

// The same shape, but the off-stack record does not lead back to the stack.
// Garbage that happens to be aligned looks exactly like this.
TEST(StackWalkCursorTest, StepStopsAtAnOffStackRecordThatDoesNotRejoin) {
  std::vector<StackFrame> r(2);
  // Below the anchor, and so is the caller it claims.
  r[1].frame_pointer = &r[0];
  r[0].frame_pointer = &r[0];

  StackWalk::Cursor cursor{&r[1]};
  EXPECT_EQ(cursor.step(), nullptr);
}

TEST(StackBoundsTest, ContainsRequiresTheWholeRecordToFit) {
  const StackBounds bounds{0x1000, 0x2000};
  ASSERT_TRUE(bounds.known());

  EXPECT_TRUE(bounds.contains(reinterpret_cast<const void*>(0x1000), 16));
  EXPECT_TRUE(bounds.contains(reinterpret_cast<const void*>(0x1ff0), 16));
  // Starts inside but runs off the top, which is how a direct read would walk
  // off the end of a stack and into whatever follows it.
  EXPECT_FALSE(bounds.contains(reinterpret_cast<const void*>(0x1ff8), 16));
  EXPECT_FALSE(bounds.contains(reinterpret_cast<const void*>(0x2000), 16));
  EXPECT_FALSE(bounds.contains(reinterpret_cast<const void*>(0xff8), 16));
}

TEST(StackBoundsTest, UnknownBoundsContainNothing) {
  const StackBounds unknown{0, 0};

  EXPECT_FALSE(unknown.known());
  EXPECT_FALSE(unknown.contains(reinterpret_cast<const void*>(0x1000), 16));
  // Would underflow if the emptiness were not checked first.
  EXPECT_FALSE(
      unknown.contains(reinterpret_cast<const void*>(~uintptr_t{0}), 16));
}

TEST(StackBoundsTest, ClampingRaisesTheFloorToTheStackPointer) {
  const StackBounds bounds{0x1000, 0x2000};

  const StackBounds clamped = bounds.clampedToStackPointer(0x1800);
  EXPECT_EQ(clamped.low, uintptr_t{0x1800});
  EXPECT_EQ(clamped.high, uintptr_t{0x2000});
  // Everything below the stack pointer is untouched address space as far as
  // anyone knows, so it stops being readable by assumption.
  EXPECT_FALSE(clamped.contains(reinterpret_cast<const void*>(0x1000), 16));
}

// A stack pointer outside the bounds means they belong to some other stack -
// the way a reissued thread id would show up - so they are dropped entirely.
TEST(StackBoundsTest, ClampingRejectsAStackPointerFromElsewhere) {
  const StackBounds bounds{0x1000, 0x2000};

  EXPECT_FALSE(bounds.clampedToStackPointer(0x800).known());
  EXPECT_FALSE(bounds.clampedToStackPointer(0x3000).known());
  EXPECT_FALSE(bounds.clampedToStackPointer(0x2000).known());
}

// Uses the machine frame pointer rather than the address of a local: under
// ASAN a local can live on a heap-allocated fake stack, which is nowhere near
// the thread's real one. Frame records never move like that, which is why the
// fast path still works under ASAN.
TEST(StackBoundsTest, TheCallersOwnFrameIsInsideItsStackBounds) {
  const StackBounds bounds = StackWalk::currentStackBounds();
  ASSERT_TRUE(bounds.known());

  EXPECT_TRUE(bounds.contains(__builtin_frame_address(0), 16));
}

// Bounds spanning exactly `records`, as a stand-in for a stack containing
// them. Stated outright rather than taken from currentStackBounds() so the
// test does not depend on where the compiler chose to put the array.
StackBounds boundsOver(const StackFrame* records, size_t count) {
  auto low = reinterpret_cast<uintptr_t>(records);
  return {low, low + count * sizeof(*records)};
}

// The point of the whole exercise: records the bounds vouch for are followed
// with plain loads, so cost does not scale with a syscall per frame.
TEST(StackWalkCursorTest, AnOnStackChainIsWalkedWithoutAnySafeReads) {
  // In address order, so each record is a plausible caller of the one before.
  StackFrame chain[4] = {};
  chain[0].frame_pointer = &chain[1];
  chain[1].frame_pointer = &chain[2];
  chain[2].frame_pointer = &chain[3];
  chain[3].frame_pointer = nullptr;
  const StackBounds bounds = boundsOver(chain, 4);

  const uint64_t before = StackWalk::safeReadCount();
  StackWalk::Cursor cursor{&chain[0], bounds};
  EXPECT_EQ(cursor.step(), &chain[1]);
  EXPECT_EQ(cursor.step(), &chain[2]);
  EXPECT_EQ(cursor.step(), &chain[3]);
  EXPECT_EQ(cursor.step(), nullptr);

  EXPECT_EQ(StackWalk::safeReadCount(), before);
}

// Without bounds the same chain is correct but pays a syscall per record.
TEST(StackWalkCursorTest, TheSameChainWithoutBoundsFallsBackToSafeReads) {
  StackFrame chain[3] = {};
  chain[0].frame_pointer = &chain[1];
  chain[1].frame_pointer = &chain[2];
  chain[2].frame_pointer = nullptr;

  const uint64_t before = StackWalk::safeReadCount();
  StackWalk::Cursor cursor{&chain[0]};
  while (cursor.step() != nullptr) {
  }

  // One for the starting record and one for each step that found a caller.
  EXPECT_EQ(StackWalk::safeReadCount() - before, uint64_t{3});
}

// A generator's spill data is on the heap, so it is outside the bounds however
// exact they are, and reading it is exactly the case the safe read exists for.
TEST(StackWalkCursorTest, AnOffStackRecordStillGoesThroughTheSafeRead) {
  StackFrame on_stack[2] = {};
  std::vector<StackFrame> heap(1);
  on_stack[0].frame_pointer = &heap[0];
  heap[0].frame_pointer = &on_stack[1];
  on_stack[1].frame_pointer = nullptr;

  const StackBounds bounds = boundsOver(on_stack, 2);
  ASSERT_FALSE(bounds.contains(&heap[0], sizeof(heap[0])));

  const uint64_t before = StackWalk::safeReadCount();
  StackWalk::Cursor cursor{&on_stack[0], bounds};
  EXPECT_EQ(cursor.step(), &heap[0]);
  EXPECT_EQ(cursor.step(), &on_stack[1]);

  // Exactly one: the heap record. The two around it were loaded directly.
  EXPECT_EQ(StackWalk::safeReadCount() - before, uint64_t{1});
}

// Real bounds are narrower than the fixed window they replace, so an address
// that is readable and above the anchor is still rejected once it is known not
// to be on the stack. The old 16 MiB window could not draw this distinction:
// it is twice the size of a whole stack.
TEST(StackWalkCursorTest, BoundsRejectAReadableRecordThatIsOffTheStack) {
  std::vector<StackFrame> r(2);
  r[0].frame_pointer = &r[1];
  r[1].frame_pointer = nullptr;

  // Bounds covering the innermost record but stopping short of its caller.
  const StackBounds bounds{
      reinterpret_cast<uintptr_t>(&r[0]), reinterpret_cast<uintptr_t>(&r[1])};

  StackWalk::Cursor cursor{&r[0], bounds};
  // Perfectly readable, but off-stack, so it is only acceptable as a generator
  // record - and its own caller is null, so the chain does not rejoin.
  EXPECT_EQ(cursor.step(), nullptr);

  // Widening the bounds to include it makes it an ordinary caller again.
  const StackBounds wider{
      reinterpret_cast<uintptr_t>(&r[0]),
      reinterpret_cast<uintptr_t>(&r[1]) + sizeof(r[1])};
  StackWalk::Cursor included{&r[0], wider};
  EXPECT_EQ(included.step(), &r[1]);
}

// Addresses a corrupted or unwound stack can leave in a frame slot, which the
// cursor must decline to dereference. Every one of these is non-null and
// pointer-aligned, so nothing but an attempt to read the memory can tell them
// apart from a genuine frame record.
class HostileAddressTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // The walker's one-time setup allocates, and the hole made below is the
    // topmost free gap in the address space, so it is exactly what the next
    // mmap is handed back. Under ASAN that mmap is certain: intercepting the
    // first std::call_once takes it through __tls_get_addr, which maps a page
    // to track the dynamic TLS. Doing the setup here, before there is a hole,
    // keeps the hole a hole.
    StackWalk::canReadFramesSafely();

    page_ = static_cast<size_t>(::sysconf(_SC_PAGESIZE));

    // Two pages with the upper one made unreadable. Leaving it mapped rather
    // than unmapping it pins the boundary in place for the straddling case,
    // and PROT_NONE is precisely what a thread stack's guard page is.
    auto* guarded = ::mmap(
        nullptr,
        page_ * 2,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    ASSERT_NE(guarded, MAP_FAILED);
    guarded_ = static_cast<char*>(guarded);
    ASSERT_EQ(::mprotect(guarded_ + page_, page_, PROT_NONE), 0);

    // A hole in the address space, with no mapping at all.
    auto* hole = ::mmap(
        nullptr,
        page_,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    ASSERT_NE(hole, MAP_FAILED);
    ASSERT_EQ(::munmap(hole, page_), 0);
    hole_ = static_cast<char*>(hole);
  }

  void TearDown() override {
    if (guarded_ != nullptr) {
      ::munmap(guarded_, page_ * 2);
    }
  }

  static const StackFrame* asRecord(const void* addr) {
    return static_cast<const StackFrame*>(addr);
  }

  // Nothing keeps an address unmapped: an allocation made since SetUp() could
  // have been handed this one back, and a hole that has quietly become
  // readable turns the tests below into no-ops that pass. Checked rather than
  // assumed, so that shows up as this failing instead of as the walker
  // appearing not to reject the address.
  void assertHoleIsUnmapped() {
    unsigned char resident = 0;
    ASSERT_EQ(::mincore(hole_, page_, &resident), -1);
    ASSERT_EQ(errno, ENOMEM) << "something has mapped the hole";
  }

  // Steps a one-record chain whose caller is `next`, and reports where the
  // cursor stopped. The record itself lives on the stack, so only `next` is
  // ever in question.
  static const StackFrame* stepOnto(const StackFrame* next) {
    StackFrame record = {next, nullptr};
    StackWalk::Cursor cursor{&record};
    return cursor.step();
  }

  size_t page_ = 0;
  char* guarded_ = nullptr;
  char* hole_ = nullptr;
};

TEST_F(HostileAddressTest, StepStopsAtARecordInAGuardPage) {
  const uint64_t before = StackWalk::unreadableFrameCount();

  EXPECT_EQ(stepOnto(asRecord(guarded_ + page_)), nullptr);

  // The page is mapped, so asking the kernel whether a mapping exists - what
  // mincore() and msync() answer - would have said yes and the load would
  // still have died. Only an attempted read rejects this.
  EXPECT_GT(StackWalk::unreadableFrameCount(), before);
}

TEST_F(HostileAddressTest, StepStopsAtARecordStraddlingAMappingBoundary) {
  const auto* straddling = asRecord(guarded_ + page_ - sizeof(void*));
  // Precondition: the record's first word is readable and its second is not.
  ASSERT_EQ(
      reinterpret_cast<uintptr_t>(&straddling->return_address),
      reinterpret_cast<uintptr_t>(guarded_ + page_));

  const uint64_t before = StackWalk::unreadableFrameCount();

  EXPECT_EQ(stepOnto(straddling), nullptr);

  // Reading only the `frame_pointer` word would have succeeded here and left
  // the cursor sitting on a record whose return address cannot be loaded.
  EXPECT_GT(StackWalk::unreadableFrameCount(), before);
}

TEST_F(HostileAddressTest, StepStopsAtARecordInUnmappedMemory) {
  ASSERT_NO_FATAL_FAILURE(assertHoleIsUnmapped());
  const uint64_t before = StackWalk::unreadableFrameCount();

  EXPECT_EQ(stepOnto(asRecord(hole_)), nullptr);

  EXPECT_GT(StackWalk::unreadableFrameCount(), before);
}

TEST_F(HostileAddressTest, AnUnreadableStartingFrameLeavesTheCursorExhausted) {
  ASSERT_NO_FATAL_FAILURE(assertHoleIsUnmapped());

  // The frame-pointer register of a thread interrupted inside code built
  // without frame pointers holds whatever that code was using it for.
  StackWalk::Cursor cursor{asRecord(hole_)};

  EXPECT_EQ(cursor.step(), nullptr);
  EXPECT_EQ(cursor.returnAddress(), nullptr);
}

TEST_F(HostileAddressTest, StepStopsAtAlignedNonPointerGarbage) {
  // Both are pointer-aligned and non-null, so they reach the read.
  for (uintptr_t garbage : {uintptr_t{0x38}, uintptr_t{0xdead000000000000}}) {
    const uint64_t before = StackWalk::unreadableFrameCount();

    EXPECT_EQ(
        stepOnto(asRecord(reinterpret_cast<const void*>(garbage))), nullptr)
        << "at " << garbage;

    EXPECT_GT(StackWalk::unreadableFrameCount(), before) << "at " << garbage;
  }
}

TEST_F(HostileAddressTest, StepStopsAtNullAndMisalignedRecordsWithoutReading) {
  for (uintptr_t rejected : {uintptr_t{0}, ~uintptr_t{0}}) {
    const uint64_t before = StackWalk::unreadableFrameCount();

    EXPECT_EQ(
        stepOnto(asRecord(reinterpret_cast<const void*>(rejected))), nullptr)
        << "at " << rejected;

    // Arithmetic disqualifies these, so no read is attempted and the walk
    // ending here is an ordinary end-of-chain rather than a truncation.
    EXPECT_EQ(StackWalk::unreadableFrameCount(), before) << "at " << rejected;
  }
}

TEST_F(HostileAddressTest, ReadableChainsAreUnaffected) {
  FakeStack stack{4};
  const uint64_t before = StackWalk::unreadableFrameCount();

  StackWalk::Cursor cursor{stack.innermost()};
  for (size_t i = 0; i + 1 < stack.depth(); i++) {
    EXPECT_EQ(cursor.step(), stack.at(i + 1)) << "at frame " << i;
  }
  EXPECT_EQ(cursor.step(), nullptr);

  // A chain that simply ran out is not a truncation: the outermost record
  // holds a null caller, which needs no read to recognise.
  EXPECT_EQ(StackWalk::unreadableFrameCount(), before);
}

// If the mechanism is missing the walker still cannot crash, but every walk
// comes back empty. That is a deployment problem worth failing loudly on here
// rather than discovering as mysteriously absent stacks.
TEST(StackWalkSafeReadTest, TheSafeReadMechanismIsAvailable) {
  EXPECT_TRUE(StackWalk::canReadFramesSafely());
}

TEST(StackWalkThreadTest, NativeThreadIdRoundTripsThroughAThreadState) {
  PyThreadState tstate = fakeThreadState(::pthread_self());

  EXPECT_TRUE(
      ::pthread_equal(StackWalk::nativeThreadId(&tstate), ::pthread_self()));
}

TEST(StackWalkThreadTest, IsCurrentThreadDistinguishesSelfFromOthers) {
  SpinningThread other;

  EXPECT_TRUE(StackWalk::isCurrentThread(::pthread_self()));
  EXPECT_FALSE(StackWalk::isCurrentThread(other.id()));
}

// The thread-state overload has to route a walk of the calling thread to
// walkSelf(); walk(ThreadId) rejects it outright.
TEST(StackWalkThreadTest, WalkOfOwnThreadStateWalksSelfRatherThanFailing) {
  StackWalk sw;
  PyThreadState tstate = fakeThreadState(::pthread_self());

  size_t frames = 0;
  const bool walked = sw.walk(&tstate, [&](const void*, const void*) {
    frames++;
    return true;
  });

  EXPECT_TRUE(walked);
  EXPECT_GT(frames, 0u);
  // The same thread by native id is not walkable, which is what makes the
  // dispatch above worth having.
  EXPECT_FALSE(
      sw.walk(::pthread_self(), [](const void*, const void*) { return true; }));
}

TEST(StackWalkThreadTest, WalksAnotherThreadThroughItsThreadState) {
  StackWalk sw;
  SpinningThread other;
  PyThreadState tstate = fakeThreadState(other.id());

  std::vector<FramePair> frames;
  const bool walked = sw.walk(&tstate, [&](const void* fp, const void* ra) {
    frames.emplace_back(fp, ra);
    return true;
  });

  EXPECT_TRUE(walked);
  // A parked thread sits several frames deep inside libstdc++ and libc.
  EXPECT_GT(frames.size(), 1u);
}

// End to end over a real signalled walk of a real thread: the sampler measures
// the target's stack before signalling, so the target reads its own frames
// directly. Only whatever the chain ends on is off-stack.
TEST(StackWalkThreadTest, ACrossThreadWalkCostsAFewSafeReadsNotOnePerFrame) {
  StackWalk sw;
  SpinningThread other;
  PyThreadState tstate = fakeThreadState(other.id());

  const uint64_t before = StackWalk::safeReadCount();
  size_t frames = 0;
  ASSERT_TRUE(sw.walk(&tstate, [&](const void*, const void*) {
    frames++;
    return true;
  }));
  const uint64_t reads = StackWalk::safeReadCount() - before;

  // A parked thread sits several frames deep inside libstdc++ and libc.
  ASSERT_GT(frames, 3u);
  EXPECT_LE(reads, 2u) << "walked " << frames << " frames but spent " << reads
                       << " safe reads; the target's stack bounds are not "
                          "reaching the walk";
}

// Walking our own stack takes the same fast path, via currentStackBounds().
TEST(StackWalkThreadTest, WalkingOurOwnStackCostsAFewSafeReadsNotOnePerFrame) {
  const uint64_t before = StackWalk::safeReadCount();
  size_t frames = 0;
  ASSERT_TRUE(StackWalk::walkSelf([&](const void*, const void*) {
    frames++;
    return true;
  }));
  const uint64_t reads = StackWalk::safeReadCount() - before;

  ASSERT_GT(frames, 3u);
  EXPECT_LE(reads, 2u) << "walked " << frames << " frames but spent " << reads
                       << " safe reads";
}

TEST(StackWalkThreadTest, CallbackReturningFalseStopsACrossThreadWalk) {
  StackWalk sw;
  SpinningThread other;
  PyThreadState tstate = fakeThreadState(other.id());

  size_t frames = 0;
  EXPECT_TRUE(sw.walk(&tstate, [&](const void*, const void*) {
    frames++;
    return false;
  }));
  EXPECT_EQ(frames, 1u);

  // Stopping early must leave the walker reusable rather than wedged.
  size_t again = 0;
  EXPECT_TRUE(sw.walk(&tstate, [&](const void*, const void*) {
    again++;
    return true;
  }));
  EXPECT_GT(again, 1u);
}

// The protocol announces the end of a walk with a batch that is not full, so a
// chain whose length is an exact multiple of kBatchSize has to be followed by
// an empty batch to say so. Getting that handshake wrong wedges both threads
// for good: the sampler blocks waiting for a batch the target has already
// decided not to send, and since the sampler never returns from walk() it never
// runs endWalk() either, leaving the target parked inside the signal handler.
//
// The walker's own frames and the thread's entry frames sit below the
// recursion, so the absolute depth cannot be predicted. Sweeping kBatchSize + 1
// consecutive lengths covers every residue modulo kBatchSize, which reaches the
// aligned case without having to know where the stack begins.
//
// A regression here hangs rather than fails - semWait has no timeout - so the
// symptom to expect is a test that never finishes.
TEST(StackWalkBatchBoundaryTest, WalksAtEveryDepthResidueFinish) {
  StackWalk sw;

  size_t deepest = 0;
  for (size_t extra = 0; extra <= kBatchSize; extra++) {
    SpinningThread other{extra};
    PyThreadState tstate = fakeThreadState(other.id());

    size_t frames = 0;
    ASSERT_TRUE(sw.walk(
        &tstate,
        [&](const void*, const void*) {
          frames++;
          return true;
        }))
        << "at depth " << extra;
    EXPECT_GT(frames, extra) << "at depth " << extra;
    deepest = std::max(deepest, frames);
  }

  // Without this the sweep could sit entirely inside a single batch and quietly
  // stop exercising the boundary it exists to cover.
  EXPECT_GT(deepest, kBatchSize);
}

} // namespace

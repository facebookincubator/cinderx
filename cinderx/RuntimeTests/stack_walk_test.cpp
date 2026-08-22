// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/stack_walk.h"

#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
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

// Short enough that a test can wait out a park several times over, long enough
// that ordinary scheduling noise does not trip it.
constexpr auto kTestParkTimeout = std::chrono::milliseconds{50};

// A thread that parks in a known-deep call chain until it is told to stop, so
// that a cross-thread walk has something stable to find. `extra_frames`
// recursive frames are pushed on top of it, for walks that need a stack of a
// chosen length.
//
// `deaf_to` names a signal the thread blocks in its own mask before it reports
// itself ready, which is how a test builds a target that can be signalled but
// will never run the handler.
class SpinningThread {
 public:
  explicit SpinningThread(size_t extra_frames = 0, int deaf_to = 0) {
    thread_ = std::thread{
        [this, extra_frames, deaf_to] { spin(extra_frames, deaf_to); }};
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

  // How many times the thread has been round its wait loop. A test that
  // signals this thread watches this climb to tell that the signal has been
  // dealt with and the thread is running ordinary code again - a handler that
  // answered would have parked it instead. Cheaper and steadier than sleeping
  // for a duration and hoping.
  uint64_t laps() const {
    return laps_.load(std::memory_order_acquire);
  }

  // Undoes `deaf_to`, and does not return until the thread has actually done
  // it. A mask belongs to its own thread, so this can only ask.
  //
  // Unblocking is what delivers anything that was left pending, and the kernel
  // does that before the unblocking call returns - so by the time this comes
  // back, a signal sent to this thread while it was deaf has been through
  // whatever disposition is now installed for it.
  void listen() {
    listen_.store(true, std::memory_order_release);
    while (!listening_.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }

 private:
  __attribute__((noinline)) void spin(size_t remaining, int deaf_to) {
    if (remaining > 0) {
      spin(remaining - 1, deaf_to);
      // Stops this being a tail call, which would collapse the whole chain
      // back down into one frame.
      asm volatile("");
      return;
    }
    // A signal mask belongs to the thread that sets it, so this has to happen
    // here rather than in the constructor - and before `ready_`, so that a test
    // which signals as soon as the thread is up cannot beat it.
    if (deaf_to != 0) {
      sigset_t deaf;
      sigemptyset(&deaf);
      sigaddset(&deaf, deaf_to);
      ::pthread_sigmask(SIG_BLOCK, &deaf, nullptr);
    }
    ready_.store(true, std::memory_order_release);
    while (!stop_.load(std::memory_order_acquire)) {
      if (deaf_to != 0 && !listening_.load(std::memory_order_relaxed) &&
          listen_.load(std::memory_order_acquire)) {
        sigset_t deaf;
        sigemptyset(&deaf);
        sigaddset(&deaf, deaf_to);
        ::pthread_sigmask(SIG_UNBLOCK, &deaf, nullptr);
        listening_.store(true, std::memory_order_release);
      }
      laps_.fetch_add(1, std::memory_order_release);
      std::this_thread::yield();
    }
    // Keeps the compiler from tail-calling or folding this frame away.
    asm volatile("");
  }

  std::atomic<bool> ready_{false};
  std::atomic<bool> stop_{false};
  std::atomic<bool> listen_{false};
  std::atomic<bool> listening_{false};
  std::atomic<uint64_t> laps_{0};
  std::thread thread_;
};

// Blocks until `thread` has been round its wait loop enough times to prove it
// is running ordinary code rather than sitting inside a signal handler.
void waitForProgress(const SpinningThread& thread) {
  constexpr uint64_t kLaps = 100;
  const uint64_t start = thread.laps();
  while (thread.laps() - start < kLaps) {
    std::this_thread::yield();
  }
}

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
  const WalkResult walked = sw.walk(&tstate, [&](const void*, const void*) {
    frames++;
    return true;
  });

  EXPECT_EQ(walked, WalkResult::Completed);
  EXPECT_GT(frames, 0u);
  // The same thread by native id is not walkable, which is what makes the
  // dispatch above worth having.
  EXPECT_EQ(
      sw.walk(::pthread_self(), [](const void*, const void*) { return true; }),
      WalkResult::Failed);
}

TEST(StackWalkThreadTest, WalksAnotherThreadThroughItsThreadState) {
  StackWalk sw;
  SpinningThread other{1};
  PyThreadState tstate = fakeThreadState(other.id());

  std::vector<FramePair> frames;
  const WalkResult walked =
      sw.walk(&tstate, [&](const void* fp, const void* ra) {
        frames.emplace_back(fp, ra);
        return true;
      });

  EXPECT_EQ(walked, WalkResult::Completed);
  EXPECT_GT(frames.size(), 1u);
}

// End to end over a real signalled walk of a real thread: the sampler measures
// the target's stack before signalling, so the target reads its own frames
// directly. Only whatever the chain ends on is off-stack.
TEST(StackWalkThreadTest, ACrossThreadWalkCostsAFewSafeReadsNotOnePerFrame) {
  StackWalk sw;
  SpinningThread other{8};
  PyThreadState tstate = fakeThreadState(other.id());

  const uint64_t before = StackWalk::safeReadCount();
  size_t frames = 0;
  ASSERT_EQ(
      sw.walk(
          &tstate,
          [&](const void*, const void*) {
            frames++;
            return true;
          }),
      WalkResult::Completed);
  const uint64_t reads = StackWalk::safeReadCount() - before;

  EXPECT_LE(reads, 2u) << "walked " << frames << " frames but spent " << reads
                       << " safe reads; the target's stack bounds are not "
                          "reaching the walk";
  EXPECT_GT(frames, 8u) << "the walk did not reach every recursive fixture "
                           "frame";
}

// Walking our own stack takes the same fast path, via currentStackBounds().
TEST(StackWalkThreadTest, WalkingOurOwnStackCostsAFewSafeReadsNotOnePerFrame) {
  const uint64_t before = StackWalk::safeReadCount();
  size_t frames = 0;
  ASSERT_EQ(
      StackWalk::walkSelf([&](const void*, const void*) {
        frames++;
        return true;
      }),
      WalkResult::Completed);
  const uint64_t reads = StackWalk::safeReadCount() - before;

  EXPECT_LE(reads, 2u) << "walked " << frames << " frames but spent " << reads
                       << " safe reads";
  EXPECT_GT(frames, reads)
      << "the walk did not find more frames than it read through the fallback";
}

TEST(StackWalkThreadTest, CallbackReturningFalseStopsACrossThreadWalk) {
  StackWalk sw;
  SpinningThread other;
  PyThreadState tstate = fakeThreadState(other.id());

  size_t frames = 0;
  EXPECT_EQ(
      sw.walk(
          &tstate,
          [&](const void*, const void*) {
            frames++;
            return false;
          }),
      WalkResult::Completed);
  EXPECT_EQ(frames, 1u);

  // Stopping early must leave the walker reusable rather than wedged.
  size_t again = 0;
  EXPECT_EQ(
      sw.walk(
          &tstate,
          [&](const void*, const void*) {
            again++;
            return true;
          }),
      WalkResult::Completed);
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
// A regression here hangs rather than fails - the sampler's wait for a batch
// has no timeout, and the target that would have run one out is not parked -
// so the symptom to expect is a test that never finishes.
TEST(StackWalkBatchBoundaryTest, WalksAtEveryDepthResidueFinish) {
  StackWalk sw;

  size_t deepest = 0;
  for (size_t extra = 0; extra <= kBatchSize; extra++) {
    SpinningThread other{extra};
    PyThreadState tstate = fakeThreadState(other.id());

    size_t frames = 0;
    ASSERT_EQ(
        sw.walk(
            &tstate,
            [&](const void*, const void*) {
              frames++;
              return true;
            }),
        WalkResult::Completed)
        << "at depth " << extra;
    EXPECT_GT(frames, extra) << "at depth " << extra;
    deepest = std::max(deepest, frames);
  }

  // Without this the sweep could sit entirely inside a single batch and quietly
  // stop exercising the boundary it exists to cover.
  EXPECT_GT(deepest, kBatchSize);
}

// A sampler that stalls - or that something else suspends - must not leave the
// thread it signalled parked in a signal handler for good. The target gives up
// on its own, and the sampler is told rather than carrying on through frames
// that describe a stack which has started running again.
TEST(StackWalkParkTimeoutTest, AParkedTargetGivesUpOnAStalledSampler) {
  StackWalk sw{SIGUSR1, kTestParkTimeout};
  SpinningThread other;
  PyThreadState tstate = fakeThreadState(other.id());

  size_t frames = 0;
  const WalkResult result = sw.walk(&tstate, [&](const void*, const void*) {
    // Stalls inside the first callback, so the park runs out while the sampler
    // is still working through the batch the target published.
    if (frames++ == 0) {
      std::this_thread::sleep_for(kTestParkTimeout * 4);
    }
    return true;
  });

  EXPECT_EQ(result, WalkResult::TimedOut);
  // Noticed rather than slept through: the walk stopped at the frame after the
  // one that stalled, without handing that frame over.
  EXPECT_EQ(frames, 1u);
}

// Recovering from a timeout is the whole point of doing it with a claim rather
// than a flag. Both semaphores have to come out of the abandoned walk balanced,
// and the target has to be back out of the handler and able to take another
// signal - which the second walk succeeding is exactly the evidence for.
TEST(StackWalkParkTimeoutTest, TheWalkerAndTheTargetSurviveATimeout) {
  StackWalk sw{SIGUSR1, kTestParkTimeout};
  SpinningThread other;
  PyThreadState tstate = fakeThreadState(other.id());

  bool stalled = false;
  ASSERT_EQ(
      sw.walk(
          &tstate,
          [&](const void*, const void*) {
            if (!std::exchange(stalled, true)) {
              std::this_thread::sleep_for(kTestParkTimeout * 4);
            }
            return true;
          }),
      WalkResult::TimedOut);

  size_t frames = 0;
  EXPECT_EQ(
      sw.walk(
          &tstate,
          [&](const void*, const void*) {
            frames++;
            return true;
          }),
      WalkResult::Completed);
  EXPECT_GT(frames, 1u);
}

// The compare-exchanges in the handshake exist for the one instant where the
// park runs out just as the sampler releases it. Either side may win; what
// neither may do is leave a `resume_` post uncollected, because the walk after
// that either reads a batch nobody published or waits for one nobody will send.
//
// Reaching that instant means holding the target across a batch boundary for
// almost exactly as long as it is willing to wait, which no single duration
// does reliably. Sweeping the stall from well inside the park to well past it
// puts every attempt at a different distance from the boundary, and lands on it
// often enough to matter.
//
// Like the batch-boundary sweep above, a regression shows up as a test that
// never finishes rather than one that fails.
TEST(StackWalkParkTimeoutTest, ATimeoutRacingAReleaseLeavesTheWalkerUsable) {
  constexpr auto kRacyTimeout = std::chrono::microseconds{200};
  constexpr size_t kAttempts = 200;

  StackWalk sw{SIGUSR1, kRacyTimeout};
  // Deep enough to need more than one batch, so a stall in the middle of the
  // walk is a stall the target is waiting out.
  SpinningThread other{kBatchSize};
  PyThreadState tstate = fakeThreadState(other.id());

  for (size_t i = 0; i < kAttempts; i++) {
    const auto stall = kRacyTimeout * 2 * i / kAttempts;
    size_t frames = 0;
    const WalkResult result = sw.walk(&tstate, [&](const void*, const void*) {
      if (++frames % kBatchSize == 0) {
        std::this_thread::sleep_for(stall);
      }
      return true;
    });

    // Failing is a legitimate outcome here, and accepting it is not slack. The
    // bound this test races the park against is also the bound on startWalk()'s
    // wait for the very first batch, and 200us is short enough that a
    // scheduling hiccup before the target reaches the handler exhausts it. What
    // a walk that never sampled the target cannot do is deliver frames on the
    // way, so that is what gets asserted instead.
    if (result == WalkResult::Failed) {
      ASSERT_EQ(frames, 0u) << "at attempt " << i;
    }
  }
}

// Never runs. Installed purely so that a signal's disposition stops being
// SIG_DFL, which is the only thing discovery looks at.
void occupyingHandler(int) {}

// Presents the scan with a process whose signals are already spoken for, and
// hands them back afterwards so the rest of the suite sees the process it
// expects. Clears the cached choice at both ends, since a signal occupied or
// released after a scan says nothing about what that scan decided.
class OccupiedSignals {
 public:
  // `handler` may be SIG_IGN: an ignored signal is as much somebody's decision
  // as a handled one, and the scan has to leave both alone.
  OccupiedSignals(const std::vector<int>& signums, void (*handler)(int)) {
    struct sigaction action = {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    for (int signum : signums) {
      struct sigaction prev = {};
      if (::sigaction(signum, &action, &prev) == 0) {
        signums_.push_back(signum);
        prev_.push_back(prev);
      }
    }
    StackWalk::resetSignumCache();
  }

  ~OccupiedSignals() {
    for (size_t i = 0; i < signums_.size(); i++) {
      ::sigaction(signums_[i], &prev_[i], nullptr);
    }
    StackWalk::resetSignumCache();
  }

  OccupiedSignals(const OccupiedSignals&) = delete;
  OccupiedSignals& operator=(const OccupiedSignals&) = delete;

  size_t count() const {
    return signums_.size();
  }

 private:
  std::vector<int> signums_;
  std::vector<struct sigaction> prev_;
};

// Every signal a scan would consider, in the order it considers them.
std::vector<int> everyCandidateSignal() {
  std::vector<int> signums;
#if defined(SIGRTMIN) && defined(SIGRTMAX)
  for (int signum = SIGRTMAX; signum >= SIGRTMIN; signum--) {
    signums.push_back(signum);
  }
#endif
  signums.push_back(SIGUSR1);
  signums.push_back(SIGUSR2);
  return signums;
}

// What discovery settles on right now, as a scan rather than as a memory of an
// earlier one.
int freshlyChosenSignum() {
  StackWalk::resetSignumCache();
  StackWalk sw;
  return sw.signum();
}

// Whether `signum` currently has no handler and is not being ignored, which is
// what discovery means by unused.
bool dispositionIsDefault(int signum) {
  struct sigaction cur = {};
  return ::sigaction(signum, nullptr, &cur) == 0 && cur.sa_handler == SIG_DFL;
}

// Walks the thread `id` names and reports how it ended, counting the frames
// delivered on the way.
WalkResult walkCounting(StackWalk& sw, StackWalk::ThreadId id, size_t& count) {
  PyThreadState tstate = fakeThreadState(id);
  return sw.walk(&tstate, [&](const void*, const void*) {
    count++;
    return true;
  });
}

// The point of discovering rather than hardcoding: the walker lands on a
// real-time signal, which is the range designed to have no other meaning.
TEST(StackWalkSignalDiscoveryTest, DiscoveryClaimsAnUnusedRealTimeSignal) {
  StackWalk::resetSignumCache();
  StackWalk sw;

  ASSERT_TRUE(sw.canSampleOtherThreads());
#if defined(SIGRTMIN) && defined(SIGRTMAX)
  EXPECT_GE(sw.signum(), SIGRTMIN);
  EXPECT_LE(sw.signum(), SIGRTMAX);
#endif

  SpinningThread other;
  size_t frames = 0;
  EXPECT_EQ(walkCounting(sw, other.id(), frames), WalkResult::Completed);
  EXPECT_GT(frames, 1u);
}

// The signal the walker would otherwise have taken is not available, so it has
// to take another - and the walk has to keep working on it.
TEST(StackWalkSignalDiscoveryTest, DiscoverySkipsASignalSomebodyElseHandles) {
  const int wanted = freshlyChosenSignum();
  ASSERT_GT(wanted, 0);

  OccupiedSignals occupied{{wanted}, &occupyingHandler};
  StackWalk sw;

  ASSERT_TRUE(sw.canSampleOtherThreads());
  EXPECT_NE(sw.signum(), wanted);

  SpinningThread other;
  size_t frames = 0;
  EXPECT_EQ(walkCounting(sw, other.id(), frames), WalkResult::Completed);
  EXPECT_GT(frames, 1u);

  // The occupant is untouched: the walker went around it rather than through
  // it, which is the whole reason to scan.
  struct sigaction cur = {};
  ASSERT_EQ(::sigaction(wanted, nullptr, &cur), 0);
  EXPECT_EQ(cur.sa_handler, &occupyingHandler);
}

// SIG_IGN is a decision somebody made, not an empty slot.
TEST(StackWalkSignalDiscoveryTest, DiscoverySkipsAnIgnoredSignal) {
  const int wanted = freshlyChosenSignum();
  ASSERT_GT(wanted, 0);

  OccupiedSignals occupied{{wanted}, SIG_IGN};
  StackWalk sw;

  EXPECT_NE(sw.signum(), wanted);

  struct sigaction cur = {};
  ASSERT_EQ(::sigaction(wanted, nullptr, &cur), 0);
  EXPECT_EQ(cur.sa_handler, SIG_IGN);
}

// A walker is built per walk, so the scan cannot be per walk. The second
// instance reuses what the first found rather than paying for the search again.
TEST(StackWalkSignalDiscoveryTest, TheChosenSignalIsReusedByLaterWalkers) {
  StackWalk::resetSignumCache();
  int first = 0;
  {
    StackWalk sw;
    first = sw.signum();
    ASSERT_GT(first, 0);
  }
  // A discovered signal is reserved for the process, not borrowed for the walk,
  // so the handler is still on it with no walker alive at all. That is what
  // makes the reuse below free of syscalls - and, more to the point, it is what
  // stops a signal this walker sent and that has not been delivered yet from
  // meeting a default disposition, which for a real-time signal terminates the
  // process.
  ASSERT_FALSE(dispositionIsDefault(first));

  StackWalk sw;
  EXPECT_EQ(sw.signum(), first);
}

// Why a discovered signal is never handed back, stated as the thing that would
// otherwise happen.
//
// A walk leaves its signal pending whenever the target does not run the handler
// for it - a blocked mask here, but the park timeout gets there too. The walker
// is then destroyed with that signal still in flight. Handing the signal back
// means putting SIG_DFL on it, and the default action for a real-time signal is
// to terminate: the process would die, from a stack walk that did nothing worse
// than come back empty.
//
// A regression is this test taking the whole process down with it, so there is
// no assertion to write for the important half. Getting to the end is the pass.
TEST(StackWalkSignalDiscoveryTest, APendingSignalOutlivesItsWalkerHarmlessly) {
  // Asked before the thread exists, because the thread has to be deaf to the
  // signal from the moment it starts and only discovery knows which one.
  const int signum = freshlyChosenSignum();
  ASSERT_GT(signum, 0);
#if defined(SIGRTMIN) && defined(SIGRTMAX)
  // The whole hazard is the default action, and only a real-time signal has the
  // one that kills. If discovery had fallen back to SIGUSR1 this would still
  // pass and prove nothing.
  ASSERT_GE(signum, SIGRTMIN);
#endif

  SpinningThread deaf{0, signum};
  {
    StackWalk sw{StackWalk::kAutoSignum, kTestParkTimeout};
    ASSERT_EQ(sw.signum(), signum);

    // Delivered nowhere: pthread_kill reports success, the signal goes pending
    // on a thread that has it blocked, and the walk gives up waiting.
    size_t frames = 0;
    ASSERT_EQ(walkCounting(sw, deaf.id(), frames), WalkResult::Failed);
    ASSERT_EQ(frames, 0u);
  }

  // The walker is gone; the signal it sent is not. Letting it through is the
  // act that used to be fatal, so it comes before anything that could assert
  // its way out of performing it.
  deaf.listen();

  // And the reason it was survivable: the handler is still there to swallow it.
  EXPECT_FALSE(dispositionIsDefault(signum));
}

// Somebody installs over the walker's handler mid-life. Signalling anyway would
// run their handler and nothing would ever answer, so the walk has to fail
// instead - and the next walker has to go and find somewhere else to live.
TEST(StackWalkSignalDiscoveryTest, AWalkerRescansAfterItsSignalIsTakenOver) {
  StackWalk::resetSignumCache();

  int taken = 0;
  {
    StackWalk sw;
    // Declared after the walker so that it is joined before the walker is
    // destroyed. Releasing a target does not wait for it to leave the handler,
    // so a walker torn down while its last target is still on the way out would
    // be read after it had gone.
    SpinningThread other;

    taken = sw.signum();
    ASSERT_GT(taken, 0);

    size_t before = 0;
    ASSERT_EQ(walkCounting(sw, other.id(), before), WalkResult::Completed);

    struct sigaction action = {};
    action.sa_handler = &occupyingHandler;
    sigemptyset(&action.sa_mask);
    ASSERT_EQ(::sigaction(taken, &action, nullptr), 0);

    size_t frames = 0;
    EXPECT_EQ(walkCounting(sw, other.id(), frames), WalkResult::Failed);
    // Failed here means the signal was never sent: sending it would have run
    // the new owner's handler, and nothing would ever have answered.
    EXPECT_EQ(frames, 0u);
  }

  // The displaced walker did not take the new owner's handler with it when it
  // was destroyed.
  struct sigaction cur = {};
  ASSERT_EQ(::sigaction(taken, nullptr, &cur), 0);
  EXPECT_EQ(cur.sa_handler, &occupyingHandler);

  // And the next walker settles somewhere else and works.
  {
    StackWalk sw;
    SpinningThread other;

    EXPECT_NE(sw.signum(), taken);
    size_t frames = 0;
    EXPECT_EQ(walkCounting(sw, other.id(), frames), WalkResult::Completed);
    EXPECT_GT(frames, 1u);
  }

  struct sigaction restore = {};
  restore.sa_handler = SIG_DFL;
  ::sigaction(taken, &restore, nullptr);
  StackWalk::resetSignumCache();
}

// The exhaustion path. A process that has spoken for every signal the walker
// would consider gets a walker that reports no frames, not one that seizes a
// signal somebody is depending on - and not an aborted process either.
TEST(StackWalkSignalDiscoveryTest, NoFreeSignalLeavesOnlySelfWalksWorking) {
  const std::vector<int> candidates = everyCandidateSignal();
  OccupiedSignals occupied{candidates, &occupyingHandler};
  ASSERT_EQ(occupied.count(), candidates.size());

  StackWalk sw;

  EXPECT_FALSE(sw.canSampleOtherThreads());
  EXPECT_EQ(sw.signum(), StackWalk::kAutoSignum);

  SpinningThread other;
  size_t frames = 0;
  EXPECT_EQ(walkCounting(sw, other.id(), frames), WalkResult::Failed);
  // Failed means the target was never touched, so the callback never ran.
  EXPECT_EQ(frames, 0u);

  // Walking the calling thread needs no signal at all, so it is unaffected -
  // which is why this is a degraded walker rather than no walker.
  size_t own = 0;
  EXPECT_EQ(walkCounting(sw, ::pthread_self(), own), WalkResult::Completed);
  EXPECT_GT(own, 0u);
}

// Being handed a signal is how a caller says the occupant does not matter, so
// it bypasses the scan that would have refused this one.
TEST(StackWalkSignalDiscoveryTest, AnExplicitSignalBypassesDiscovery) {
  OccupiedSignals occupied{{SIGUSR1}, &occupyingHandler};

  {
    StackWalk sw{SIGUSR1};
    EXPECT_EQ(sw.signum(), SIGUSR1);

    SpinningThread other;
    size_t frames = 0;
    EXPECT_EQ(walkCounting(sw, other.id(), frames), WalkResult::Completed);
    EXPECT_GT(frames, 1u);
  }

  // And it puts the occupant back, which is what makes displacing one
  // survivable when a caller asks for it deliberately.
  struct sigaction cur = {};
  ASSERT_EQ(::sigaction(SIGUSR1, nullptr, &cur), 0);
  EXPECT_EQ(cur.sa_handler, &occupyingHandler);
}

// How many rounds the stray-signal tests below run. Delivery is asynchronous -
// pthread_kill only makes the signal pending - so a single round could in
// principle check before the handler has run. Repeating closes that off: a
// round whose signal lands late is caught by the next one, because the damage
// it would do neither undoes itself nor goes uncollected.
constexpr size_t kStrayRounds = 20;

// The walker's signal is shared with the rest of the process, so it can be
// delivered to a thread that is not the walk's target. Capturing frames for one
// would park an uninvolved thread inside the handler and publish a batch
// describing a stack nobody asked about.
TEST(StackWalkSignalGateTest, ASignalToANonTargetThreadCapturesNothing) {
  // The park timeout only bounds how long this takes when it is failing: an
  // answered signal parks the bystander, and waitForProgress() then waits it
  // out rather than hanging.
  StackWalk sw{StackWalk::kAutoSignum, kTestParkTimeout};
  ASSERT_TRUE(sw.canSampleOtherThreads());
  SpinningThread bystander;

  for (size_t i = 0; i < kStrayRounds; i++) {
    const uint64_t reads_before = StackWalk::safeReadCount();

    ASSERT_EQ(::pthread_kill(bystander.id(), sw.signum()), 0) << "round " << i;
    waitForProgress(bystander);

    // Capturing reads frame records, and with no bounds measured for a thread
    // the sampler never asked about, every one of those goes the slow way. So a
    // count that has not moved is the evidence that nothing was captured.
    ASSERT_EQ(StackWalk::safeReadCount(), reads_before) << "round " << i;
  }
}

// The desync the gate exists to prevent, end to end. A batch published by a
// bystander leaves an uncollected post on `frames_ready_`, and the next walk
// takes it and reports the bystander's frames as its target's.
TEST(StackWalkSignalGateTest, AStraySignalDoesNotDesyncTheFollowingWalk) {
  StackWalk sw{StackWalk::kAutoSignum, kTestParkTimeout};
  ASSERT_TRUE(sw.canSampleOtherThreads());
  // Deep enough that a batch of its frames is a full one, which a target only a
  // handful of frames deep can never produce.
  SpinningThread bystander{2 * kBatchSize};
  SpinningThread target;

  // Walked once first, so that the strays below land on a thread the walker has
  // already measured. A capture on one of those reads its frames directly
  // instead of through the fault-safe read, which is exactly the case the read
  // count above cannot see - the batch left behind is the only evidence of it.
  size_t deep = 0;
  ASSERT_EQ(walkCounting(sw, bystander.id(), deep), WalkResult::Completed);
  ASSERT_GT(deep, kBatchSize);

  for (size_t i = 0; i < kStrayRounds; i++) {
    ASSERT_EQ(::pthread_kill(bystander.id(), sw.signum()), 0) << "round " << i;

    size_t frames = 0;
    PyThreadState tstate = fakeThreadState(target.id());
    const WalkResult result = sw.walk(&tstate, [&](const void*, const void*) {
      // Stopped short rather than counted up and checked afterwards. Draining a
      // full batch is what sends the sampler back for another one, and with the
      // semaphores already a post out of step there is nobody left to publish
      // it - so a walk allowed to run on would hang here rather than fail.
      return ++frames < kBatchSize;
    });

    ASSERT_EQ(result, WalkResult::Completed) << "round " << i;
    // No timing assumption is needed for this one: an uncollected post stays
    // uncollected until some walk drains it, so a stray answered in any round
    // shows up as a full batch in this round or a later one.
    ASSERT_LT(frames, kBatchSize) << "round " << i;
    ASSERT_GT(frames, 1u) << "round " << i;
  }
}

// A delivered signal is not a handler that runs. A target holding this signal
// in its mask leaves it pending for as long as it likes - pthread_kill reports
// success either way - so the batch the sampler is waiting for never arrives.
// An unbounded wait there costs the sampling thread for the life of the
// process; a bounded one costs a walk.
//
// A regression shows up as a test that never finishes rather than one that
// fails, which is the nature of the bug being guarded against.
TEST(StackWalkSignalTimeoutTest, ATargetDeafToTheSignalFailsRatherThanHanging) {
  StackWalk sw{StackWalk::kAutoSignum, kTestParkTimeout};
  ASSERT_TRUE(sw.canSampleOtherThreads());
  SpinningThread deaf{0, sw.signum()};

  size_t frames = 0;
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(walkCounting(sw, deaf.id(), frames), WalkResult::Failed);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  // Failed means the target was never sampled, so nothing reached the callback.
  EXPECT_EQ(frames, 0u);
  // Loose by a wide margin - this is here to catch a bound that exists but is
  // not the one the walker was configured with, not to measure scheduling.
  EXPECT_LT(elapsed, kTestParkTimeout * 20);
}

// The walk that gave up must leave nothing behind it. Nothing was published, so
// no post is uncollected, and the walker goes on working.
TEST(StackWalkSignalTimeoutTest, AWalkerSurvivesATargetThatNeverAnswers) {
  StackWalk sw{StackWalk::kAutoSignum, kTestParkTimeout};
  ASSERT_TRUE(sw.canSampleOtherThreads());
  SpinningThread deaf{0, sw.signum()};
  SpinningThread listening;

  for (size_t i = 0; i < 3; i++) {
    size_t ignored = 0;
    ASSERT_EQ(walkCounting(sw, deaf.id(), ignored), WalkResult::Failed)
        << "round " << i;

    // An uncollected post from the walk that timed out would be drained here,
    // and this walk would report frames belonging to nothing.
    size_t frames = 0;
    ASSERT_EQ(walkCounting(sw, listening.id(), frames), WalkResult::Completed)
        << "round " << i;
    ASSERT_GT(frames, 1u) << "round " << i;
  }
}

// A sampler that stops waiting just as the target publishes its first batch is
// the instant the bound has to get right. Both sides come out of `Running` with
// a compare-exchange, so one of them wins - and a sampler that loses still has
// to collect the post the target made, or it stays on the semaphore and the
// next walk starts by draining a batch published for a walk that is over.
//
// That instant is nanoseconds wide and cannot be aimed at: a signalled thread
// already on another core usually answers before the sampler even reaches its
// wait, in which case the wait finds the post and never times out at all. So
// this sweeps the bound across the region where the two are comparable and
// takes what it gets, checking every outcome for sense. Like the park-timeout
// sweep above, the regression it is really guarding against shows up as a test
// that never finishes rather than one that fails.
TEST(
    StackWalkSignalTimeoutTest,
    ATimeoutRacingTheFirstBatchLeavesTheWalkerUsable) {
  constexpr size_t kSteps = 24;
  constexpr size_t kAttemptsPerStep = 20;

  for (size_t step = 0; step < kSteps; step++) {
    // From far too short for any walk to finish up to comfortably enough for
    // most of them.
    const auto timeout = std::chrono::nanoseconds{250} * (step + 1);
    StackWalk sw{StackWalk::kAutoSignum, timeout};
    SpinningThread other;

    for (size_t i = 0; i < kAttemptsPerStep; i++) {
      size_t frames = 0;
      const WalkResult result = walkCounting(sw, other.id(), frames);
      switch (result) {
        case WalkResult::Failed:
          // The target was never sampled, so nothing can have reached the
          // callback - least of all a batch belonging to an earlier round.
          ASSERT_EQ(frames, 0u) << "step " << step << " attempt " << i;
          break;
        case WalkResult::Completed:
          ASSERT_GT(frames, 1u) << "step " << step << " attempt " << i;
          break;
        case WalkResult::TimedOut:
          // The target stopped waiting part way through. Whatever it handed
          // over came off a real stack; with a bound this short there is no
          // saying how much of it there was, including none.
          break;
      }
    }
  }
}

// Holds the calling thread for `duration` without sleeping. Spinning rather
// than sleeping because a stalled sampler is what these tests are arranging,
// and sleep_for in a test is what the linter is entitled to object to.
void stallFor(std::chrono::nanoseconds duration) {
  const auto until = std::chrono::steady_clock::now() + duration;
  while (std::chrono::steady_clock::now() < until) {
    std::this_thread::yield();
  }
}

// A walker is a local, built and destroyed once per walk - frame.cpp does
// exactly that on the deopt path - so a walk has to leave nothing of the target
// inside it. Releasing the target only wakes it: it still has to read
// `stop_requested_` and unwind out of the handler, and every one of those
// touches is on the walker. A sampler that carried straight on from the release
// would be free to run the destructor underneath it.
//
// The thread outlives the walker here, deliberately inverting the declaration
// order the rest of this file uses. That order is what has been hiding this:
// joining the thread first guarantees it is out of the handler, so a walker
// declared before its target can never catch the race.
//
// A regression is an ASan stack-use-after-scope inside publishBatch(), reported
// on the target thread - not a failed assertion.
TEST(StackWalkHandlerExitTest, AWalkerCanBeDestroyedTheInstantItsWalkReturns) {
  SpinningThread other;

  // Repeated because it is a race, even though it is one the sampler is
  // overwhelmingly likely to win: the woken target has a futex wake and a
  // reschedule ahead of it, and the sampler has a destructor.
  for (size_t i = 0; i < 50; i++) {
    size_t frames = 0;
    {
      StackWalk sw{StackWalk::kAutoSignum, kTestParkTimeout};
      ASSERT_TRUE(sw.canSampleOtherThreads()) << "round " << i;
      ASSERT_EQ(walkCounting(sw, other.id(), frames), WalkResult::Completed)
          << "round " << i;
    }

    // Load-bearing, and the reason the walker gets a scope of its own rather
    // than the loop body. ASan poisons the walker's stack slot when that scope
    // ends and unpoisons it the moment the next round builds another one
    // there, so a round that ran straight into the next would hand a straggling
    // target a live object to land on and report nothing. Waiting for the
    // target to be demonstrably back in ordinary code holds the window open
    // across the whole of the time it could touch the walker in.
    waitForProgress(other);
    ASSERT_GT(frames, 1u) << "round " << i;
  }
}

// The other way a target leaves the handler: its park runs out and it goes on
// its own, so endWalk() finds nothing to release. It has to wait even so,
// because the target posts on its way out whether it was released or gave up.
//
// Waiting only when there was a target to release is the tempting shortcut, and
// what makes it a bug is not a race - the target is already several
// instructions past the state change by the time the sampler can act on it -
// but the count it leaves behind. An uncollected post is taken by the *next*
// walk's endWalk() in place of its own target's, and that walk then returns
// with a target still inside the handler. So the walk that catches it is the
// ordinary one that follows, and it catches it every time rather than by luck.
TEST(StackWalkHandlerExitTest, AWalkerStaysSafeToDestroyAfterATimedOutWalk) {
  SpinningThread other;

  for (size_t i = 0; i < 5; i++) {
    size_t frames = 0;
    {
      StackWalk sw{StackWalk::kAutoSignum, kTestParkTimeout};
      PyThreadState tstate = fakeThreadState(other.id());

      size_t stalled = 0;
      ASSERT_EQ(
          sw.walk(
              &tstate,
              [&](const void*, const void*) {
                if (stalled++ == 0) {
                  stallFor(kTestParkTimeout * 2);
                }
                return true;
              }),
          WalkResult::TimedOut)
          << "round " << i;

      // The walk that pays for it, on the same walker and the same target.
      ASSERT_EQ(walkCounting(sw, other.id(), frames), WalkResult::Completed)
          << "round " << i;
    }
    // Keeps the walker's slot poisoned for as long as the target could still
    // be in it, exactly as above.
    waitForProgress(other);
    ASSERT_GT(frames, 1u) << "round " << i;
  }
}

} // namespace

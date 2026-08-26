// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

// Which of the two mechanisms below this platform uses to hold a thread still.
// Darwin and Windows both stop a thread from the outside and walk it from the
// sampling thread; everywhere else the target is interrupted and walks itself.
// Almost every conditional in this header and its implementation is asking
// which mechanism is in play rather than which operating system it is on, so
// they ask it by this name. See the StackWalk class comment for the difference.
#if defined(__APPLE__) || defined(_WIN32)
#define CINDERX_STACK_WALK_SUSPENDS 1
#endif

#if defined(CINDERX_STACK_WALK_SUSPENDS) || defined(__linux__)

#include "cinderx/Common/util.h"

// Only what this header actually names. <windows.h> in particular stays out:
// this is included by every file that walks a stack, and the macros it brings
// with it would follow it into all of them. The one Windows type needed below
// is spelled out by hand instead.
#if defined(_WIN32)
// Nothing.
#elif defined(__APPLE__)
// Darwin stops a thread through Mach instead of interrupting it with a signal,
// so it needs a port name here and none of the POSIX semaphore machinery.
#include <mach/port.h>
#include <pthread.h>
#else
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace cinderx {

// Both the record the hardware pushes and the frame the walker reports.
// They are the same sixteen bytes, seen from the two ends of a single step.
//
// AArch64 (AAPCS64) and x86-64 alike push, at the address held in the
// frame-pointer register, the caller's frame pointer followed by the address
// this frame will return to. Both of those describe the *caller*, so the
// record sitting at one frame's frame pointer already is the description of
// the frame outside it, and stepping outwards is nothing more than reading
// one of these and passing it on.
//
// Read as a reported frame: `frame_pointer` owns the code that
// `return_address` points into. That still holds for the innermost frame,
// the one case the walker synthesises rather than reads, where the
// return_address is the interrupted PC - an address being executed rather
// than one that will be returned to, and stored in no record anywhere.
struct StackFrame {
  const StackFrame* frame_pointer;
  const void* return_address;
};

// The span of a thread's stack that is known to be mapped, as [low, high).
//
// Worth having because it turns the question "can this address be read" from
// something only the kernel can answer into two comparisons. A frame record
// lying wholly inside a live thread's stack cannot fault, so it can be
// loaded directly instead of through a syscall.
//
// A zeroed value means "unknown", which is always safe: every read then goes
// the slow way. Always brace-initialise one - it deliberately carries no
// default member initialisers, because a nested class cannot use them from
// inside the class that encloses it.
struct StackBounds {
  uintptr_t low;
  uintptr_t high;

  bool known() const {
    return low < high;
  }

  // Whether `size` bytes at `addr` lie wholly within the span.
  bool contains(const void* addr, size_t size) const {
    auto a = reinterpret_cast<uintptr_t>(addr);
    return known() && a >= low && a < high && high - a >= size;
  }

  // Narrows to the portion at or above `sp`, and reports nothing at all if
  // `sp` lies outside.
  //
  // Both halves matter. Only the part of a stack from the stack pointer up
  // to the base is certainly mapped: glibc reports the main thread's stack
  // as everything RLIMIT_STACK permits, most of which has never been touched
  // and some of which is the guard gap. And `sp` belonging to some other
  // stack entirely is how a thread id that was recycled since these bounds
  // were measured gives itself away, which is the one way they could
  // describe the wrong thread.
  StackBounds clampedToStackPointer(uintptr_t sp) const {
    if (!known() || sp < low || sp >= high) {
      return {};
    }
    return {sp, high};
  }
};

// How a walk ended.
enum class WalkResult {
  // The chain was followed to its end, or the callback asked to stop early.
  Completed,
  // The target could not be sampled at all, and no frames were delivered.
  Failed,
  // The target stopped waiting and resumed before the walk was finished.
  // Frames already delivered were describing a frozen stack when they were
  // delivered; nothing beyond that point could be collected, because the
  // stack started moving again.
  TimedOut,
};

// Samples another thread's native call stack while it is stopped, so the
// addresses handed to the callback always describe a stack that is still live.
//
// How a thread is stopped is the one thing that differs between platforms, and
// the difference is large enough that the two mechanisms share only the frame
// model, the cursor, and the fault-safe read.
//
// On Darwin and Windows the thread is stopped from the outside. thread_suspend
// or SuspendThread holds it, thread_get_state or GetThreadContext reads the
// registers the walk starts from, and the sampler follows the chain itself
// before thread_resume or ResumeThread lets the target go. Nothing runs on the
// target thread at all, which is why none of the machinery below this paragraph
// exists there.
//
// Everywhere else the thread is stopped from the inside, by interrupting it
// with a signal. The target walks its own frames within the handler and parks
// until the sampling thread is finished. Stacks of any depth are supported: the
// target publishes frames a batch at a time into a fixed buffer, and once the
// sampler has drained a batch it wakes the target, which resumes the walk where
// it left off. The target stays inside the handler for the whole sequence, so
// the frames remain frozen across batches and the batching is invisible to the
// callback.
//
// The sampling thread should do minimal work while the thread is stopped - this
// includes not taking any locks, on either mechanism, since a thread held
// mid-allocation will deadlock a sampler that allocates.
//
// The signalled mechanism adds a bound the suspending one does not need. A
// target that has been parked for kParkTimeout stops waiting and returns from
// the handler on its own, so a sampler that stalls, or that is itself suspended
// by something else, costs a truncated walk rather than a thread wedged in a
// signal handler for the life of the process. The walk then reports
// WalkResult::TimedOut, and everything it has not already delivered is lost:
// the stack those frames describe is running again. A suspended target is never
// waiting on its sampler, so a suspending walk has nothing to time out and
// never reports TimedOut.
//
// Only one instance may exist at a time, on the signalled mechanism: a signal
// handler cannot be given a context pointer, so the active instance is reached
// through a global. The suspending mechanism has no handler and so no such
// restriction.
//
// Windows carries one caveat the other two do not. Its x64 code keeps a frame
// pointer only where it needs one, so a thread stopped in frameless code cannot
// be walked at all, and one stopped in code that does keep a frame pointer
// still stops as soon as the chain reaches a caller that does not. That is the
// ordinary end-of-chain the cursor already recognises, so it costs short walks
// rather than wrong ones.
class StackWalk {
 public:
#if defined(_WIN32)
  // Windows' DWORD, spelled out so that <windows.h> and the macros it brings
  // with it stay out of every file that walks a stack. CPython stores exactly
  // this in PyThreadState::thread_id there, since PyThread_get_thread_ident()
  // is GetCurrentThreadId(), so nativeThreadId() below hands OpenThread()
  // precisely what it wants.
  using ThreadId = unsigned long;
#else
  using ThreadId = pthread_t;
#endif

  // How long a signalled thread stays parked before it abandons the walk.
  //
  // A parked target is entirely at the mercy of its sampler: if that thread
  // stalls, or is suspended by something else, or dies between startWalk() and
  // endWalk(), nothing is left to release the target and it sits inside a
  // signal handler indefinitely. The cap turns that into a bounded delay and a
  // walk that comes back short.
  //
  // Means nothing on the suspending mechanism, where no target ever waits for
  // its sampler.
  static constexpr std::chrono::nanoseconds kParkTimeout =
      std::chrono::seconds{1};

  // The layout the compilers and the JIT's own prologue all agree on, and that
  // reading a record straight out of a stack depends on. See
  // codegen/arch.h's kFrameRecordSize, which emits it.
  static_assert(sizeof(StackFrame) == 2 * sizeof(void*));

  // Asks the constructor to go and find a signal rather than be told one.
  //
  // Not a valid signal number, and deliberately not zero: zero is a legal
  // argument to kill(2) and so cannot double as "none".
  static constexpr int kAutoSignum = -1;

  // Installs a handler for `signum`, saving whatever was there before, and
  // sets up the fault-safe read if nothing has yet. Doing the latter here as
  // well as lazily is what keeps it off the signalled path: by the time a
  // handler runs, the instance that made the signal possible has seen to it.
  //
  // With kAutoSignum the signal is discovered instead of dictated: the
  // real-time range is scanned for one whose disposition is still the default,
  // once per process, and every later instance reuses that answer - handler and
  // all, since a discovered signal is reserved for the life of the process
  // rather than claimed and released around each walk. Hardcoding a signal
  // cannot be right in a process the walker does not own - SIGUSR1 in
  // particular is what CPython's faulthandler is usually registered on - and
  // quietly displacing the owner's handler is a worse failure than not walking.
  //
  // A process with no signal to spare gets an instance that cannot sample other
  // threads at all rather than one that fights over a signal something else is
  // relying on. See canSampleOtherThreads().
  //
  // Passing a signal explicitly skips the scan and installs on exactly that
  // one, over whatever is already there. That is what the tests want, and what
  // an embedder that has reserved a signal of its own wants.
  //
  // `park_timeout` controls how long a target signalled by this instance will
  // wait for it. Worth overriding only in tests, which would otherwise have to
  // sleep for a whole second to reach the timeout at all.
  //
  // Both arguments are accepted and ignored on the suspending mechanism, which
  // reaches its targets through Mach or the Win32 thread API rather than
  // through a signal. They stay in the signature so that callers and tests are
  // written once.
  explicit StackWalk(
      int signum = kAutoSignum,
      std::chrono::nanoseconds park_timeout = kParkTimeout);

  // Hands back a signal that was named by the caller, and keeps one that
  // discovery found. See the definition for why the asymmetry is deliberate.
  ~StackWalk();

  StackWalk(const StackWalk&) = delete;
  StackWalk(StackWalk&&) = delete;
  StackWalk& operator=(const StackWalk&) = delete;
  StackWalk& operator=(StackWalk&&) = delete;

  // Whether `thread` is the one calling this.
  static bool isCurrentThread(ThreadId thread);

  // Whether this instance can sample threads other than the one calling it.
  //
  // False means the signal scan came up empty: every signal it would consider
  // is already spoken for. walkSelf(), and the thread-state overload aimed at
  // the calling thread, still work - neither involves a signal - and every
  // cross-thread walk reports WalkResult::Failed without disturbing the target.
  //
  // Always true on the suspending mechanism, which needs no signal to reach a
  // target.
  bool canSampleOtherThreads() const {
#if defined(CINDERX_STACK_WALK_SUSPENDS)
    return true;
#else
    return signum_ > 0;
#endif
  }

  // The signal this instance sends, or kAutoSignum if it has none - which on
  // the suspending mechanism is always, since nothing there is reached by
  // signalling it.
  int signum() const {
#if defined(CINDERX_STACK_WALK_SUSPENDS)
    return kAutoSignum;
#else
    return signum_;
#endif
  }

  // Forgets the signal chosen for this process and takes the handler back off
  // it, so the next instance scans afresh. Only for tests, which need each case
  // to start from a known state - production never gives a discovered signal
  // back. A no-op on the suspending mechanism, which claims no signal to give
  // back.
  static void resetSignumCache();

  // Reports the frames of the thread `tstate` is running on, innermost first,
  // as walk(ThreadId) does. Walking the calling thread needs no freezing at
  // all, so it is dispatched to walkSelf() rather than rejected.
  //
  // The self test is on the native thread rather than on tstate identity: one
  // thread can own several thread states at once, one per interpreter, and all
  // of them are still the calling thread.
  //
  // Which frames the walker's own machinery contributes at the head of the
  // sequence differs between the two paths, and depends on how much of this
  // inlines. Callers must therefore recognise the frame they want by identity
  // and skip whatever precedes it, never index from the top.
  template <typename F>
  WalkResult walk(PyThreadState* tstate, F&& callback) {
    ThreadId thread = nativeThreadId(tstate);
    return isCurrentThread(thread) ? walkSelf(std::forward<F>(callback))
                                   : walk(thread, std::forward<F>(callback));
  }

  // Freezes `thread` and invokes `callback(frame_pointer, return_address)` once
  // per frame, innermost first, until the stack is exhausted. The callback runs
  // on the calling thread while `thread` stays frozen; returning false stops
  // the walk early and lets the target resume without unwinding the rest.
  //
  // Darwin and Windows freeze it by suspending it and read its registers from
  // here; everywhere else the target is interrupted and freezes itself. Which
  // mechanism ran is invisible to the callback: both deliver the same frames in
  // the same order.
  //
  // `thread` must stay alive for the duration: a thread id outlives the thread
  // it named, and freezing a recycled one is undefined. Holding the GIL, or
  // stopping the world, is enough to guarantee that of a thread state's thread.
  //
  // Reports WalkResult::Failed without invoking the callback if the target
  // could not be sampled. Sampling the calling thread would deadlock and is
  // rejected; call walkSelf(), or the thread-state overload above, for that.
  //
  // Reports WalkResult::TimedOut if the target gave up waiting part way
  // through, which it does once it has been parked for kParkTimeout. Frames
  // handed over before that are as good as any other; there are simply no more
  // of them, and a caller that kept an address out of one is now holding a
  // pointer into a stack that has resumed. A suspended target waits for nobody,
  // so a suspending walk never reports this.
  //
  // Both addresses arrive as `const void*`, the frame pointer deliberately so.
  // It is only ever a plausible address, never one known to be readable - a
  // chain that has left the stack can point into a guard page - and the walker
  // never dereferences one itself. Handing over a `const StackFrame*` would
  // advertise a read that is not safe to make.
  template <typename F>
  WalkResult walk(ThreadId thread, F&& callback) {
#if defined(CINDERX_STACK_WALK_SUSPENDS)
    // Measured before the target is stopped rather than after. Deriving bounds
    // reads the thread descriptor, and there is no reason to hold a thread
    // still while doing it.
    const StackBounds bounds = stackBoundsFor(thread);

    SuspendedThread target{thread};
    if (!target.valid()) {
      return WalkResult::Failed;
    }
    // Everything from here to the end of the scope runs with `thread` held, so
    // it is the callback rather than any of this that decides how long the
    // target stays stopped. The destructor resumes it on every path out,
    // including a callback that throws.
    return walkStopped(
        target.framePointer(),
        target.programCounter(),
        bounds.clampedToStackPointer(target.stackPointer()),
        std::forward<F>(callback));
#else
    if (!startWalk(thread)) {
      return WalkResult::Failed;
    }
    // The target stays blocked until this runs, so it must happen even if the
    // callback throws.
    SCOPE_EXIT(endWalk());

    while (true) {
      for (size_t i = 0; i < num_frames_; i++) {
        // The batch in hand describes a frozen stack only for as long as the
        // target is still parked, and the callback is where all the time in a
        // walk goes. Asking before every frame rather than once per batch is
        // what stops a slow callback being handed an address belonging to a
        // stack that has already resumed.
        if (targetAbandonedWalk()) {
          return WalkResult::TimedOut;
        }
        const StackFrame& frame = frames_[i];
        if (!callback(
                static_cast<const void*>(frame.frame_pointer),
                frame.return_address)) {
          return WalkResult::Completed;
        }
      }
      // A batch the target could not fill is the last one: it stopped because
      // the chain ran out, not because the buffer did. A chain that ends
      // exactly on a batch boundary therefore costs one more round trip, to
      // collect the empty batch that says so.
      if (num_frames_ < kBatchSize) {
        return WalkResult::Completed;
      }
      if (!nextBatch()) {
        return targetAbandonedWalk() ? WalkResult::TimedOut
                                     : WalkResult::Completed;
      }
    }
#endif
  }

  // Walks the calling thread's own stack, invoking the same callback as
  // walk(), starting with the caller of walkSelf() and working outwards. No
  // signal is involved: a thread's own frames cannot move while it is the one
  // inspecting them, so this is just a pointer chase and needs no instance.
  template <typename F>
  static WalkResult walkSelf(F&& callback) {
    Cursor cursor{
        static_cast<const StackFrame*>(__builtin_frame_address(0)),
        currentStackBounds()};
    while (true) {
      const void* return_address = cursor.returnAddress();
      const StackFrame* caller = cursor.step();
      if (caller == nullptr ||
          !callback(static_cast<const void*>(caller), return_address)) {
        return WalkResult::Completed;
      }
    }
  }

  // Follows a chain of frame records outwards from a starting frame.
  //
  // Stacks grow down, so each record must sit above the last, and one that is
  // misaligned or absurdly far away means the chain has run into a frameless
  // function or garbage rather than the bottom of the stack. A resumed JIT
  // generator is the one exception: it runs with the frame-pointer register
  // aimed at its heap-allocated data, so the chain leaves the stack for a
  // single record before rejoining it just above where it left. Plausibility
  // is therefore measured against the innermost record seen *on the stack*
  // rather than against the immediately preceding one.
  //
  // Those checks say only that an address is a believable frame pointer, never
  // that it is readable, and the difference is fatal: a chain that leaves the
  // stack has nothing to stop it pointing into a guard page or a hole in the
  // address space. The cursor therefore never dereferences a frame pointer.
  // Each record is copied in once through a read that cannot fault, and every
  // subsequent load comes from that copy.
  //
  // Given the walked thread's stack bounds that copy is usually free: a record
  // inside a live stack is readable by definition, so it can just be loaded.
  // Only records outside the stack - a resumed generator's heap data, and
  // whatever garbage terminates the chain - need the syscall, which is a
  // handful per walk rather than one per frame.
  class Cursor {
   public:
    // Reads the record at `frame`. A cursor over an unreadable address starts
    // out exhausted rather than failing later: step() reports the end of the
    // chain immediately and returnAddress() is null.
    //
    // `bounds` describes the stack being walked. Passing none costs only
    // speed; the walk is equally correct either way.
    explicit Cursor(const StackFrame* frame, StackBounds bounds = {});

    // The address being executed in the frame the cursor is on. Read this
    // before stepping: it is stored in the callee's record, not the caller's.
    const void* returnAddress() const {
      return record_.return_address;
    }

    // Moves to the caller, or returns null at the end of the chain.
    const StackFrame* step();

   private:
    // Copies the record at `addr`, directly when the stack bounds prove that
    // cannot fault and through the fault-safe read otherwise.
    bool readRecord(const StackFrame* addr, StackFrame* out) const;

    // Whether `frame` is a believable caller of the frame at `anchor_`: a
    // record further up the same stack. Known bounds make this exact; without
    // them it falls back to a generous fixed window, which cannot tell a stack
    // address from something just past the end of the stack.
    bool isAboveAnchor(const StackFrame* frame) const;

    // The contents of the record at `frame_`, read once when the cursor
    // arrived there. Reading `frame_` again would be a second chance to fault
    // on an address that was only ever plausible, so nothing does.
    StackFrame record_ = {};
    const StackFrame* frame_ = nullptr;
    const StackFrame* anchor_ = nullptr;
    StackBounds bounds_ = {};
    // Whether `record_` was actually read. A cursor that never got a first
    // record has no chain to follow.
    bool valid_ = false;
  };

  // The native thread `tstate` is running on.
  //
  // Reads the field rather than calling into CPython, both because the walker
  // is used from a signal handler and because it keeps this header free of any
  // symbol that would have to be linked.
  static ThreadId nativeThreadId(PyThreadState* tstate) {
    static_assert(sizeof(ThreadId) <= sizeof(tstate->thread_id));
    ThreadId thread;
    std::memcpy(&thread, &tstate->thread_id, sizeof(thread));
    return thread;
  }

  // The calling thread's own stack, for walking it directly. Measured once per
  // thread and remembered, because deriving it for the main thread means
  // parsing /proc/self/maps.
  //
  // Not usable from a signal handler on a thread that has never asked before,
  // since that first measurement allocates. Nothing on the signalled path does
  // ask: a sampled thread is handed bounds its sampler measured for it.
  static StackBounds currentStackBounds();

  // Whether frame records can be read without risking a fault. False means a
  // sandbox has taken the mechanism away, and every walk will come back empty
  // rather than crash. Probes if nothing has yet, so it answers the same
  // whether or not anything has walked.
  static bool canReadFramesSafely();

  // How many fault-safe reads have been performed across the whole process,
  // which is to say how many syscalls the walker has spent on frames it could
  // not prove were on a stack. Expect a couple per walk; one per frame means
  // the stack bounds are not reaching the walk.
  static uint64_t safeReadCount();

  // How many frame records have been rejected as unreadable across the whole
  // process. A chain that simply ran out does not count: the outermost record
  // holds a null caller, which is recognised without a read. So a non-zero
  // count means some walk was cut short by an address that looked like a frame
  // pointer but was not backed by memory, and a count that climbs with every
  // walk means stacks are being silently truncated.
  static uint64_t unreadableFrameCount();

  // Sized purely as a memory/round-trip tradeoff; deeper stacks just take more
  // batches.
  static constexpr size_t kBatchSize = 64;

 private:
  // Probes the fault-safe read, once per process. Every entry point that can
  // start a walk calls this, so no caller has to have arranged it - but only
  // ever from an ordinary thread. Reaching the probe itself from a signal
  // handler would mean allocating and blocking there; see the definition for
  // why nothing signalled can.
  static void ensureSafeReadInitialized();

  // The stack of `thread`, remembered between walks. Measuring it is cheap for
  // a spawned thread but not for the main one, and the deopt path walks the
  // same thread over and over.
  //
  // A thread id outliving its thread and being reissued would make this stale.
  // That is caught on the other side, where the bounds are checked against the
  // stack pointer of the thread actually holding them.
  StackBounds stackBoundsFor(ThreadId thread);

#if defined(CINDERX_STACK_WALK_SUSPENDS)
  // Holds a thread stopped for as long as it is alive, and captures the
  // registers a walk of it starts from.
  //
  // A scope guard rather than a pair of calls because both suspends are
  // counted: a resume that does not happen leaves a thread that never runs
  // again, and the walk between the two ends is a callback that may return
  // early or throw. Tying the resume to a destructor is the only way to owe
  // exactly one of them on every path out. Windows additionally owns a thread
  // handle, which the same destructor closes.
  //
  // Reading the registers is part of construction so that a caller cannot
  // reach them without holding the guard that makes them meaningful.
  class SuspendedThread {
   public:
    explicit SuspendedThread(ThreadId thread);
    ~SuspendedThread();

    SuspendedThread(const SuspendedThread&) = delete;
    SuspendedThread(SuspendedThread&&) = delete;
    SuspendedThread& operator=(const SuspendedThread&) = delete;
    SuspendedThread& operator=(SuspendedThread&&) = delete;

    // Whether the thread was stopped and its registers read, which is the only
    // state in which the accessors below mean anything. False covers a thread
    // that could not be suspended and one whose registers could not be read;
    // the destructor resumes it either way, so the difference matters only to
    // the walk.
    bool valid() const {
      return valid_;
    }

    // The frame-pointer register, which is where the chain starts.
    const StackFrame* framePointer() const {
      return frame_;
    }

    // The address the target was executing when it was stopped. Stored in no
    // frame record, which is why the walk emits it separately.
    const void* programCounter() const {
      return pc_;
    }

    // The stack-pointer register, for vouching that the bounds measured
    // beforehand really do describe this thread.
    uintptr_t stackPointer() const {
      return sp_;
    }

   private:
    // What the destructor has to undo, or a null value if nothing was ever
    // suspended. Distinct from `valid_`: a thread whose registers could not be
    // read is still suspended and still owed a resume.
#if defined(_WIN32)
    // Windows' HANDLE, kept as void* so that <windows.h> stays out of this
    // header. Non-null means both suspended and owned: the constructor closes
    // the handle itself if the suspend does not land, so there is no state
    // where this is set but no resume is owed.
    void* handle_ = nullptr;
#else
    mach_port_t port_ = MACH_PORT_NULL;
#endif
    const StackFrame* frame_ = nullptr;
    const void* pc_ = nullptr;
    uintptr_t sp_ = 0;
    bool valid_ = false;
  };

  // Reports the frames of a stack that is being held still, innermost first, in
  // the order walk() promises: the interrupted PC alongside the frame executing
  // it, then one pair per record in the chain.
  //
  // The caller is responsible for the stack actually being still. Nothing here
  // stops the target, so this must run inside the scope of the guard that does.
  template <typename F>
  static WalkResult walkStopped(
      const StackFrame* frame,
      const void* pc,
      StackBounds bounds,
      F&& callback) {
    // A target whose frame-pointer register holds nothing usable has no chain
    // to follow, and no PC worth reporting on its own.
    if (frame == nullptr) {
      return WalkResult::Completed;
    }
    if (!callback(static_cast<const void*>(frame), pc)) {
      return WalkResult::Completed;
    }
    Cursor cursor{frame, bounds};
    while (true) {
      const void* return_address = cursor.returnAddress();
      const StackFrame* caller = cursor.step();
      if (caller == nullptr ||
          !callback(static_cast<const void*>(caller), return_address)) {
        return WalkResult::Completed;
      }
    }
  }
#else
  // Installs this class's handler on `signum`, whatever is already there,
  // saving what that was into `prev` if that is not null. Reports whether it
  // went in.
  static bool installHandler(int signum, struct sigaction* prev);

  // Whether this class's handler is the one currently installed on `signum`.
  static bool handlerIsOurs(int signum);

  // Scans for a signal nothing else is using and installs this class's handler
  // on it, reporting which one or kAutoSignum if the process has none to spare.
  static int claimSignum();

  // The signal this process has reserved for walking, scanning for one if this
  // is the first time it has been asked. Reports kAutoSignum if the process has
  // none to spare.
  //
  // The handler goes on once and stays on: see the destructor for why taking it
  // off again is the dangerous half, and this for why leaving it on is also the
  // cheap half. Every instance after the first therefore costs no syscall at
  // all, which matters because frame.cpp builds one per walk.
  static int acquireSignal();

  // Where the handshake with the target has got to.
  //
  // Only one transition is contested, and it is the one the timeout creates:
  // the target giving up on a park, and the sampler releasing it, can happen
  // at the same instant. Both are compare-exchanges out of `Parked`, so
  // exactly one wins and the loser can tell that it lost. That is what keeps
  // the semaphores balanced - a park that ran out just as `resume_` was posted
  // would otherwise leave that post uncollected, and the following walk would
  // read a batch nobody published.
  enum class State : uint8_t {
    // No target is parked: either no walk is in progress, or the target is
    // away collecting the next batch.
    Running,
    // The target has published a batch and is waiting on `resume_`.
    Parked,
    // The target's park ran out and it has left the handler. The stack it was
    // holding still is running again.
    Abandoned,
  };

  // Whether the target has stopped waiting for us, which invalidates every
  // frame not yet handed to the callback.
  bool targetAbandonedWalk() const {
    return state_.load(std::memory_order_acquire) == State::Abandoned;
  }

  // Signals `thread` and waits for its first batch of frames.
  bool startWalk(ThreadId thread);

  // Wakes the parked target to collect the next batch and waits for it.
  // Returns false if there was no target left to wake.
  bool nextBatch();

  // Tells the parked target to abandon the walk and return from the handler,
  // and waits until it has actually left.
  //
  // The wait is what makes a walker safe to destroy - or to walk with again -
  // the instant a walk returns. Releasing the target only wakes it: it still
  // has to read `stop_requested_` and unwind out of the handler, and every one
  // of those touches is on this object. A sampler that carried straight on
  // would be free to run the destructor, and for the stack-allocated walker
  // frame.cpp builds per call that means nothing more than returning from the
  // enclosing function.
  void endWalk();

  // Wakes a parked target, reporting whether there was one to wake. A target
  // whose park ran out is already out of the handler and must not be posted
  // to: the post would sit uncollected and desync the next walk.
  bool wakeTarget();

  static void handleSignal(int signum, siginfo_t* info, void* ucontext);

  // Whether the calling thread is the one a walk is currently waiting on.
  //
  // The handler answers a signal only when this is true, because a delivery to
  // any other thread is not this walker's to answer. Answering one would park
  // an uninvolved thread inside the handler and post `frames_ready_` with a
  // batch describing a stack nobody asked about - and the next walk would drain
  // that post and hand those frames to its callback.
  //
  // Nothing guarantees the only sender is us. Our signal is shared with the
  // rest of the process: discovery picks one that looked unclaimed, and the gap
  // between looking and installing cannot be closed, so a previous owner may
  // still signal it. A stray kill(2) reaches it just as easily.
  //
  // Runs inside the handler, so it must be async-signal-safe. An atomic load
  // and pthread_equal are both nothing more than comparisons.
  bool isWalkTarget() const;

  // Runs on the target thread inside the signal handler, so everything it
  // touches must be async-signal-safe.
  void captureFrames(const void* ucontext);

  // What became of a batch offered to the sampler.
  enum class Publish : uint8_t {
    // Taken, and the sampler wants the next one.
    Taken,
    // Taken, and the walk is over - either the sampler said so or the park ran
    // out. Somebody is waiting for this handler to finish.
    Finished,
    // Not taken, because the sampler had already given up on this walk before
    // the batch was offered. Nothing was posted for it and nobody is waiting
    // for it, which is the difference that matters on the way out.
    Refused,
  };

  // Publishes the batch built so far and blocks until the sampler asks for
  // more.
  //
  // A short batch is how the end of the chain is announced, so a full one
  // always costs another round trip even when nothing is left to send.
  Publish publishBatch(size_t count);

  // A handler may not touch a lock, so the pointer above has to be reachable
  // without one. True everywhere this builds; asserted so that a port to
  // somewhere it is not fails here rather than deadlocking in a signal.
  static_assert(std::atomic<StackWalk*>::is_always_lock_free);

  std::chrono::nanoseconds park_timeout_;
  struct sigaction prev_action_ = {};

  // C++20 semaphore's aren't signal safe, so we use semaphore.h
  sem_t frames_ready_{};
  sem_t resume_{};

  // Posted by the target as the last thing it does inside the handler, and
  // collected by endWalk(). One post per handler that answered a walk, one
  // wait per walk that got an answer - the same balance the other two keep,
  // and for the same reason: a count left behind would be taken by a later
  // walk in place of its own target's, which is exactly the wait this exists
  // to perform being silently skipped.
  sem_t handler_exited_{};

  // Read and written from inside the handler as well as from the sampler, so
  // it has to be reachable without a lock for the same reason `s_active` does.
  std::atomic<State> state_ = State::Running;
  static_assert(std::atomic<State>::is_always_lock_free);

  // The thread the walk in progress is waiting on, or a zeroed id when no walk
  // is in progress. Written by the sampler either side of a walk and read by
  // the handler, on whichever thread the signal landed on, so it is held to the
  // same lock-free standard as `state_`.
  //
  // A zeroed id is the "no walk" marker rather than a thread. pthread_self()
  // never produces one - a thread id is the address of a thread descriptor
  // everywhere this builds - so nothing real is ever mistaken for it.
  std::atomic<ThreadId> walk_target_ = {};
  static_assert(std::atomic<ThreadId>::is_always_lock_free);

  // Written by the target thread's handler, read by the sampling thread while
  // the target is parked, and vice versa for `stop_requested_`. The semaphores
  // supply the happens-before edges.
  //
  // A count below kBatchSize doubles as the end-of-walk signal, which is why
  // the target publishes an empty batch rather than just returning when a
  // chain happens to end on a batch boundary.
  std::array<StackFrame, kBatchSize> frames_ = {};
  size_t num_frames_ = 0;

  // The target's stack, measured by the sampler before signalling and read by
  // the target inside the handler. pthread_kill orders the two.
  StackBounds target_bounds_ = {};

  // The signal this instance sends, settled in the constructor and never
  // changed after: startWalk() reacts to being displaced by dropping the
  // process-wide choice, so that the next instance looks elsewhere, rather than
  // by moving this one to another signal underneath a walk.
  int signum_ = kAutoSignum;
  bool stop_requested_ = false;

  // Whether the signal was taken from somebody rather than reserved by
  // discovery, and so has to be handed back when this instance goes. Only a
  // caller who named a signal can produce one: discovery refuses an occupied
  // signal, so what it claims is owed to nobody.
  bool signal_is_borrowed_ = false;
#endif

  // Last thread stackBoundsFor() was asked about, and its answer. Shared: both
  // mechanisms measure the target's stack on the sampling thread, and both walk
  // the same thread repeatedly.
  ThreadId bounds_cache_thread_ = {};
  StackBounds bounds_cache_ = {};
  bool bounds_cache_valid_ = false;
};

} // namespace cinderx

#endif

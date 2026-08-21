// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/stack_walk.h"

#if defined(__APPLE__) || defined(__linux__)

#include <fmt/format.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace cinderx {

namespace {

// How far above the previous record a caller's record may sit when the stack's
// real extent is unknown. Only a guess at "no frame is bigger than this", and a
// poor filter: it is twice the size of a default thread stack, so it accepts
// plenty of addresses that are nowhere near the stack.
constexpr uintptr_t kMaxFrameSize = 16 * 1024 * 1024;

// Our own pid, for the self-reads below. Cached because getpid() is a real
// syscall - glibc dropped its cache in 2.25 - and this would otherwise double
// the cost of reading a frame.
//
// Atomic because it is written on whichever thread builds the first StackWalk
// and read from a signal handler on the thread being sampled.
std::atomic<pid_t> s_pid = -1;

std::atomic<uint64_t> s_unreadable_frames = 0;
std::atomic<uint64_t> s_safe_reads = 0;

// Copies the record at `addr`, reporting failure for an address that is not
// backed by readable memory instead of faulting on it.
//
// process_vm_readv against our own pid is the mechanism because it is the only
// one that answers the question actually being asked. Probing the page tables
// first - mincore(), msync() - only reveals whether a mapping exists, so a
// PROT_NONE guard page reads as fine and the load that follows still dies. A
// SIGSEGV handler would work, but the handler is process-wide and shared with
// CPython's faulthandler, which any Python code can re-arm underneath us.
//
// Reads the whole record rather than one word: a record in the last eight
// bytes of a mapping has a readable `frame_pointer` and an unreadable
// `return_address`, and only asking for both catches it, as a short read.
//
// Safe to call from a signal handler. glibc's wrapper is a bare syscall stub,
// and self-reads need no ptrace privilege - the kernel lets a thread group
// inspect itself regardless of the Yama scope.
bool safeReadUnchecked(const StackFrame* addr, StackFrame* out) {
#ifdef SYS_process_vm_readv
  struct iovec local = {out, sizeof(*out)};
  struct iovec remote = {const_cast<StackFrame*>(addr), sizeof(*out)};
  return ::syscall(
             SYS_process_vm_readv,
             s_pid.load(std::memory_order_relaxed),
             &local,
             1UL,
             &remote,
             1UL,
             0UL) == static_cast<long>(sizeof(*out));
#else
  (void)addr;
  (void)out;
  return false;
#endif
}

void refreshPid() {
  s_pid.store(::getpid(), std::memory_order_relaxed);
}

// What the probe found. False also covers "not probed yet", which is the safe
// reading of it: a read attempted before then would be using a pid of -1.
std::atomic<bool> s_safe_read_available = false;

// Whether the probe has happened at all, which `s_safe_read_available` cannot
// say on its own - a process where the syscall is unavailable leaves that
// false forever, and every read would go looking for an initialisation that
// has already been done. The release/acquire pair on this is also what
// publishes `s_pid` to the threads that read frames.
std::atomic<bool> s_safe_read_initialized = false;

std::once_flag s_safe_read_once;

// Deferred out of process startup, so a process that never walks a stack pays
// for none of this. Not deferred as far as the reads themselves, which happen
// inside a signal handler: pthread_atfork may allocate and the once-flag can
// block, so neither belongs there.
void initSafeRead() {
  refreshPid();
  // fork() gives the child a new pid, and process_vm_readv needs it.
  ::pthread_atfork(nullptr, nullptr, refreshPid);

  // Establish up front whether the syscall works at all rather than
  // rediscovering per frame that it does not: a seccomp policy can remove it,
  // as Docker's default profile does without CAP_SYS_PTRACE. A local is
  // readable by construction, so a failure here is about the mechanism.
  StackFrame probe = {};
  StackFrame out{};
  s_safe_read_available.store(
      safeReadUnchecked(&probe, &out), std::memory_order_release);
  s_safe_read_initialized.store(true, std::memory_order_release);
}

bool safeRead(const StackFrame* addr, StackFrame* out) {
  s_safe_reads.fetch_add(1, std::memory_order_relaxed);
  if (s_safe_read_available.load(std::memory_order_acquire) &&
      safeReadUnchecked(addr, out)) {
    return true;
  }
  s_unreadable_frames.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// The stack of `thread`, or nothing if it cannot be determined.
//
// Reads the thread descriptor, so it must not run on a thread that is parked
// in a signal handler waiting for us - and for the main thread it parses
// /proc/self/maps and allocates. Both are reasons this belongs on the sampling
// thread, before the target is signalled, rather than in the handler.
StackBounds threadStackBounds(pthread_t thread) {
#if defined(__GLIBC__)
  pthread_attr_t attr;
  if (::pthread_getattr_np(thread, &attr) != 0) {
    return {};
  }
  void* base = nullptr;
  size_t size = 0;
  const int rc = ::pthread_attr_getstack(&attr, &base, &size);
  ::pthread_attr_destroy(&attr);
  if (rc != 0 || base == nullptr || size == 0) {
    return {};
  }
  // pthread_attr_getstack reports the usable stack, with the guard page
  // already excluded.
  auto low = reinterpret_cast<uintptr_t>(base);
  return {low, low + size};
#else
  (void)thread;
  return {};
#endif
}

// Constant-initialised, so reading them needs no guard variable and no
// lazily-run initialiser.
thread_local StackBounds s_self_bounds = {};
thread_local bool s_self_bounds_known = false;

// The signal every StackWalk in this process uses, or zero if none has been
// chosen yet. Shared rather than settled per instance because a walker is built
// for each walk - see frame.cpp's getIPStackAddr() - and a scan costs a syscall
// per candidate.
//
// A non-zero value means more than "chosen": it means this class's handler is
// installed on it and staying there. That is what lets every instance after the
// first take the signal without a single syscall, so the two places that could
// falsify it both put this back to zero first - startWalk() when it finds the
// handler displaced, and resetSignumCache() when a test asks for the signal
// back.
std::atomic<int> s_signum = 0;
static_assert(std::atomic<int>::is_always_lock_free);

// Guards the scan, so that two threads arriving at once make one choice rather
// than two.
//
// Never taken from a signal handler, and nothing signalled can reach it:
// choosing a signal happens in a constructor, on an ordinary thread, before
// this instance's handler exists to be run.
std::mutex s_signum_mutex;

// Whether nothing has installed a handler on `signum` or asked for it to be
// ignored.
bool dispositionIsDefault(int signum) {
  struct sigaction cur = {};
  if (::sigaction(signum, nullptr, &cur) != 0) {
    return false;
  }
  // SIG_IGN is as much a decision as a handler is, and not ours to overrule.
  return cur.sa_handler == SIG_DFL;
}

// sem_wait, but retried until it either succeeds or fails for a reason other
// than EINTR.
//
// A bare sem_wait() is not enough: it is a cancellation point that returns
// EINTR whenever any handler runs on the waiting thread, and SA_RESTART does
// not cover it. Neither side of the batch handshake can treat that as an
// answer. On the sampler side an EINTR looks like "the target never parked",
// so the walk would be abandoned while the target is still blocked in the
// handler on resume_ - and endWalk() skips the sem_post that would release it,
// wedging that thread for good. On the target side an early return would carry
// the thread out of the handler and let it keep running while the sampler is
// still reading its frames, which is exactly the freeze the parking exists to
// provide.
int semWait(sem_t* sem) {
  int rc;
  while ((rc = ::sem_wait(sem)) != 0 && errno == EINTR) {
  }
  return rc;
}

// glibc added sem_clockwait in 2.30. Before that the only timed wait is
// sem_timedwait, whose deadline is a CLOCK_REALTIME one and so moves when the
// wall clock is adjusted.
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 30)
#define CINDERX_HAVE_SEM_CLOCKWAIT 1
#endif
#endif

// semWait(), but giving up once `timeout` has elapsed.
//
// Returns zero if the semaphore was taken and non-zero if it was not, which in
// practice always means the deadline passed. Every other failure is reported
// the same way deliberately: the caller uses this to put a bound on how long
// it will wait, and a wait that cannot be performed has to end like one that
// ran out rather than quietly becoming an unbounded one.
//
// Called from inside the signal handler, so it is held to the same standard as
// the rest of the handshake: clock_gettime() is async-signal-safe outright,
// and the timed waits are the same lock-free futex machinery as sem_wait().
int semWaitFor(sem_t* sem, std::chrono::nanoseconds timeout) {
#if defined(__linux__)
#if defined(CINDERX_HAVE_SEM_CLOCKWAIT)
  constexpr clockid_t clock = CLOCK_MONOTONIC;
#else
  constexpr clockid_t clock = CLOCK_REALTIME;
#endif
  constexpr int64_t kNanosPerSecond = 1'000'000'000;

  struct timespec deadline = {};
  if (::clock_gettime(clock, &deadline) != 0) {
    return -1;
  }
  const int64_t nanos = deadline.tv_nsec + timeout.count();
  deadline.tv_sec += nanos / kNanosPerSecond;
  deadline.tv_nsec = nanos % kNanosPerSecond;

  int rc;
#if defined(CINDERX_HAVE_SEM_CLOCKWAIT)
  while ((rc = ::sem_clockwait(sem, clock, &deadline)) != 0 && errno == EINTR) {
  }
#else
  while ((rc = ::sem_timedwait(sem, &deadline)) != 0 && errno == EINTR) {
  }
#endif
  return rc;
#else
  // macOS has no timed semaphore wait. It has no working unnamed semaphores
  // either - sem_init() fails with ENOSYS - so the constructor has already
  // thrown and no target can ever get here to park.
  (void)timeout;
  return semWait(sem);
#endif
}

} // namespace

void StackWalk::ensureSafeReadInitialized() {
  // The load is the whole of the fast path, and the only part of this a signal
  // handler ever executes. Nothing signalled can reach the call_once below: a
  // signalled walk needs a live StackWalk, and its constructor did this on an
  // ordinary thread.
  if (s_safe_read_initialized.load(std::memory_order_acquire)) {
    return;
  }
  std::call_once(s_safe_read_once, initSafeRead);
}

bool StackWalk::canReadFramesSafely() {
  // Asking the question is reason enough to answer it properly, rather than
  // reporting the "not probed yet" false to a caller deciding whether walking
  // is worth attempting.
  ensureSafeReadInitialized();
  return s_safe_read_available.load(std::memory_order_acquire);
}

uint64_t StackWalk::unreadableFrameCount() {
  return s_unreadable_frames.load(std::memory_order_relaxed);
}

uint64_t StackWalk::safeReadCount() {
  return s_safe_reads.load(std::memory_order_relaxed);
}

StackBounds StackWalk::currentStackBounds() {
  if (!s_self_bounds_known) {
    s_self_bounds = threadStackBounds(::pthread_self());
    s_self_bounds_known = true;
  }
  // Everything a walk of this thread will read lies at or above the frame this
  // is running in, and that portion is certainly mapped. Below it, the main
  // thread's reported stack is mostly untouched address space.
  return s_self_bounds.clampedToStackPointer(
      reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
}

bool StackWalk::Cursor::readRecord(const StackFrame* addr, StackFrame* out)
    const {
  if (bounds_.contains(addr, sizeof(*out))) {
    // Inside a live stack, so the load cannot fault and the syscall would be
    // pure overhead.
    *out = *addr;
    return true;
  }
  return safeRead(addr, out);
}

bool StackWalk::Cursor::isAboveAnchor(const StackFrame* frame) const {
  auto addr = reinterpret_cast<uintptr_t>(frame);
  auto anchor_addr = reinterpret_cast<uintptr_t>(anchor_);
  if (addr % sizeof(void*) != 0 || addr <= anchor_addr) {
    return false;
  }
  if (bounds_.known()) {
    return bounds_.contains(frame, sizeof(StackFrame));
  }
  return addr - anchor_addr < kMaxFrameSize;
}

StackWalk::Cursor::Cursor(const StackFrame* frame, StackBounds bounds)
    : frame_{frame}, anchor_{frame}, bounds_{bounds} {
  // Every read a cursor goes on to make depends on this, and a cursor is the
  // only way frames are ever read - including from walkSelf(), which is
  // nothing but a cursor over the calling thread.
  ensureSafeReadInitialized();

  // The starting frame is the least trustworthy one in the chain: it comes
  // from the frame-pointer register of an interrupted thread, and code built
  // without frame pointers uses that register for whatever it likes.
  valid_ = frame != nullptr &&
      reinterpret_cast<uintptr_t>(frame) % sizeof(void*) == 0 &&
      readRecord(frame, &record_);
}

const StackFrame* StackWalk::Cursor::step() {
  if (!valid_) {
    return nullptr;
  }
  const StackFrame* next = record_.frame_pointer;
  if (next == nullptr ||
      reinterpret_cast<uintptr_t>(next) % sizeof(void*) != 0) {
    return nullptr;
  }

  // One read serves both the generator test below and every load made from
  // this record once the cursor moves onto it.
  StackFrame next_record{};
  if (!readRecord(next, &next_record)) {
    return nullptr;
  }

  if (isAboveAnchor(next)) {
    anchor_ = next;
  } else if (!isAboveAnchor(next_record.frame_pointer)) {
    // Neither a step up the stack nor a resumed JIT generator's off-stack
    // record, which garbage that merely happens to be aligned effectively
    // never looks like. Treat it as the end of the chain.
    return nullptr;
  }
  frame_ = next;
  record_ = next_record;
  return next;
}

bool StackWalk::isCurrentThread(ThreadId thread) {
  return ::pthread_equal(thread, ::pthread_self()) != 0;
}

// The instance the handler dispatches to, published by the constructor once
// the object is fit to be signalled and cleared before it stops being so. Null
// therefore doubles as "the slot is free", which is what makes the one-instance
// rule enforceable with a compare-exchange rather than a check and a store.
//
// Atomic because the two ends run on different threads with nothing else
// ordering them: the constructing thread stores, and an arbitrary target
// thread loads from inside a signal handler. A release/acquire pair is what
// makes the members the handler goes on to touch - the semaphores, the batch
// buffer, the bounds - visible to it.
static std::atomic<StackWalk*> s_active;

bool StackWalk::installHandler(int signum, struct sigaction* prev) {
  struct sigaction action = {};
  action.sa_sigaction = handleSignal;
  // Darwin exposes the signal-set helpers as macros, so they cannot be
  // namespace-qualified.
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  return ::sigaction(signum, &action, prev) == 0;
}

bool StackWalk::handlerIsOurs(int signum) {
  struct sigaction cur = {};
  return signum > 0 && ::sigaction(signum, nullptr, &cur) == 0 &&
      (cur.sa_flags & SA_SIGINFO) != 0 && cur.sa_sigaction == handleSignal;
}

// Finds a signal nothing else is using and installs this class's handler on it,
// for good. See acquireSignal() for why the installation is permanent.
//
// The real-time range is the natural home: those signals carry no default
// meaning, they queue rather than collapse - so one walk's signal cannot be
// swallowed by another's still pending - and glibc has already hidden the two
// it reserves for itself below SIGRTMIN, which is why nothing here has to know
// about SIGCANCEL or SIGSETXID.
//
// Scanned downwards from the top, because the bottom of the range is where
// everything that hardcodes a real-time signal lands. SIGRTMIN+4 alone is
// Tupperware's container-stop signal, heapprofd's profiling signal and
// dyninst's breakpoint; under Tupperware it is not merely somebody else's, it
// terminates the process.
int StackWalk::claimSignum() {
  sigset_t blocked;
  sigemptyset(&blocked);
  ::pthread_sigmask(SIG_BLOCK, nullptr, &blocked);

  auto claim = [&blocked](int signum) {
    // A blocked signal is the one shape of "in use" a disposition cannot show.
    // A signal consumed with sigwait() or signalfd() has no handler at all, so
    // it reads as unclaimed below and taking it would be undetectable - but a
    // thread that waits on a signal has to block it first, and the convention
    // is to block it in every thread before any of them start.
    if (sigismember(&blocked, signum) == 1) {
      return false;
    }
    // Already carrying this class's handler, which means an earlier scan
    // claimed it and the choice was forgotten rather than given back. It is
    // still ours; reclaiming it costs nothing and stops a process that keeps
    // rescanning from working its way through the range.
    if (handlerIsOurs(signum)) {
      return true;
    }
    // Querying before installing, rather than installing and inspecting what
    // came back, is what keeps a foreign handler intact: one displaced even for
    // the instant it takes to notice and put it back is a window in which its
    // owner's signal runs this handler instead of theirs. The gap between the
    // query and the install cannot be closed - POSIX has no atomic
    // test-and-install - but it is a gap rather than a certainty.
    //
    // Nothing is saved from the install: what was there was the default
    // disposition, and it is never going back.
    return dispositionIsDefault(signum) && installHandler(signum, nullptr);
  };

#if defined(SIGRTMIN) && defined(SIGRTMAX)
  // Runtime calls into glibc rather than constants, so this cannot be a
  // compile-time range.
  for (int signum = SIGRTMAX; signum >= SIGRTMIN; signum--) {
    if (claim(signum)) {
      return signum;
    }
  }
#endif

  // No real-time signal left - or no real-time signals at all, as on macOS - so
  // fall back to the two the standard sets aside for applications. SIGUSR1
  // first: SIGUSR2 is what common/base's thread stack tracer takes by default.
  constexpr int kFallbacks[] = {SIGUSR1, SIGUSR2};
  for (int signum : kFallbacks) {
    if (claim(signum)) {
      return signum;
    }
  }
  return kAutoSignum;
}

int StackWalk::acquireSignal() {
  // The common case, and the reason this is worth caching at all: frame.cpp
  // builds a walker per getIPStackAddr() call, so anything done here is done
  // once per walk. A signal already claimed needs no syscall to go on using -
  // not even a query to confirm the handler is still ours, because startWalk()
  // checks that before every walk anyway and is the place that can do something
  // about the answer.
  int signum = s_signum.load(std::memory_order_relaxed);
  if (signum > 0) {
    return signum;
  }

  std::lock_guard<std::mutex> guard{s_signum_mutex};
  // Another thread may have scanned while this one waited for the lock.
  signum = s_signum.load(std::memory_order_relaxed);
  if (signum > 0) {
    return signum;
  }

  signum = claimSignum();
  if (signum <= 0) {
    // Deliberately not remembered as a lasting verdict: whoever holds the
    // signals may give one back, and rescanning costs nothing next to a walk
    // that never happens.
    return kAutoSignum;
  }
  // Published only once the handler is actually on it, so that a non-zero
  // `s_signum` always means "installed", which is what lets every later
  // instance skip the syscalls entirely.
  s_signum.store(signum, std::memory_order_relaxed);
  return signum;
}

void StackWalk::resetSignumCache() {
  // Hands the signal back as well as forgetting it. Nothing in production calls
  // this - the handler is meant to stay for the life of the process - but a
  // test asking for a fresh scan wants the process put back as it found it, and
  // a forgotten-but-still-installed signal would otherwise be one the next scan
  // steps over.
  const int signum = s_signum.exchange(0, std::memory_order_relaxed);
  if (signum > 0 && handlerIsOurs(signum)) {
    struct sigaction action = {};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    ::sigaction(signum, &action, nullptr);
  }
}

StackWalk::StackWalk(int signum, std::chrono::nanoseconds park_timeout)
    : park_timeout_{park_timeout} {
  // Duplicates the cursor's own call, and the timing is the point: this
  // instance's cursors are built inside a signal handler, which is no place to
  // discover the probe has not been done yet.
  ensureSafeReadInitialized();

  if (!canReadFramesSafely()) {
    // Not fatal - walks come back empty instead of crashing - but the symptom
    // on its own would just look like stacks mysteriously going missing.
    fmt::print(
        stderr,
        "StackWalk: process_vm_readv is unavailable, so frame records cannot "
        "be read without risking a fault; every walk will report no frames\n");
  }

  if (::sem_init(&frames_ready_, 0, 0)) {
    throw std::runtime_error("failed to initialize frames_ready_ semaphore");
  }
  if (::sem_init(&resume_, 0, 0)) {
    // Throwing from a constructor runs no destructor, so whatever did
    // initialise has to be handed back here or it is leaked. Any semaphore
    // added above this point needs the same treatment.
    ::sem_destroy(&frames_ready_);
    throw std::runtime_error("failed to initialize resume_ semaphore");
  }
  if (::sem_init(&handler_exited_, 0, 0)) {
    ::sem_destroy(&frames_ready_);
    ::sem_destroy(&resume_);
    throw std::runtime_error("failed to initialize handler_exited_ semaphore");
  }

  // Claiming the slot and publishing into it are the same operation, so two
  // threads constructing at once cannot both find it empty and go on to
  // install handlers over each other.
  //
  // Claimed here rather than on entry because the sem_inits above can throw,
  // and a constructor that throws runs no destructor: a pointer published
  // before them would outlive the object it names. Still before the handler is
  // installed, though, so the first signal the installation makes possible
  // already finds it. Release, because everything the handler will touch was
  // built above.
  StackWalk* unclaimed = nullptr;
  if (!s_active.compare_exchange_strong(
          unclaimed, this, std::memory_order_release)) {
    fmt::print(stderr, "StackWalk: only one instance may be active\n");
    std::abort();
  }

  if (signum != kAutoSignum) {
    // A signal named by the caller is a decision already made, so it goes in
    // over whatever is there. Discovery would refuse an occupied signal; being
    // told one is the way to say that the occupant does not matter.
    if (!installHandler(signum, &prev_action_)) {
      s_active.store(nullptr, std::memory_order_release);
      fmt::print(
          stderr,
          "StackWalk: sigaction failed: {}\n",
          std::system_category().message(errno));
      std::abort();
    }
    // Borrowed rather than reserved: the occupant is owed its signal back, and
    // the destructor is the only place that can return it.
    signal_is_borrowed_ = true;
    signum_ = signum;
    return;
  }

  signum_ = acquireSignal();
  if (signum_ == kAutoSignum) {
    // Not fatal, for the same reason an unavailable process_vm_readv is not:
    // this instance can still walk the thread asking, and a walk that reports
    // nothing beats stealing a signal something else is depending on.
    fmt::print(
        stderr,
        "StackWalk: every signal it would use is already spoken for, so only "
        "the calling thread can be walked; other threads will report no "
        "frames\n");
  }
}

StackWalk::~StackWalk() {
  // A discovered signal is not given back, and that is the point rather than an
  // omission. Restoring one would mean putting SIG_DFL back on a real-time
  // signal, whose default action is to terminate the process - so a signal this
  // walker sent that has not been delivered yet, which is exactly what the park
  // timeout and a blocked target both leave behind, would kill the process on
  // the way out of a stack walk. Leaving the handler in place closes that
  // window, and costs nothing: with no walk in progress `s_active` is null and
  // the handler returns without touching anything.
  //
  // A borrowed one still goes back, and only if the handler installed is still
  // this instance's - one that was displaced would be destroying the new
  // owner's handler rather than its own.
  if (signal_is_borrowed_ && handlerIsOurs(signum_) &&
      ::sigaction(signum_, &prev_action_, nullptr)) {
    JIT_ABORT("failed to restore signal handler");
  }
  s_active.store(nullptr, std::memory_order_release);
  // Safe to take these apart because endWalk() does not return until the
  // target is out of the handler, so no walk this instance ran can still have
  // a thread waiting on one of them.
  ::sem_destroy(&frames_ready_);
  ::sem_destroy(&resume_);
  ::sem_destroy(&handler_exited_);
}

StackBounds StackWalk::stackBoundsFor(ThreadId thread) {
  if (bounds_cache_valid_ && ::pthread_equal(bounds_cache_thread_, thread)) {
    return bounds_cache_;
  }
  bounds_cache_ = threadStackBounds(thread);
  bounds_cache_thread_ = thread;
  bounds_cache_valid_ = true;
  return bounds_cache_;
}

bool StackWalk::startWalk(ThreadId thread) {
  // Signalling ourselves would park the only thread that could resume us.
  if (isCurrentThread(thread)) {
    return false;
  }
  // An instance that found no signal to claim has no way to reach the target.
  if (!canSampleOtherThreads()) {
    return false;
  }
  // Something may have installed its own handler over this one since the
  // constructor ran. Checking before signalling rather than discovering it
  // afterwards is the whole point: the signal would run their handler, nothing
  // would ever post `frames_ready_`, and the wait for the first batch is not a
  // timed one - so this walk would not come back short, it would not come back
  // at all.
  if (!handlerIsOurs(signum_)) {
    // Give up the process-wide choice so the next instance looks somewhere
    // else, rather than moving this one out from under a walk. Nothing is
    // reinstalled here: the signal belongs to whoever took it.
    int taken = signum_;
    s_signum.compare_exchange_strong(taken, 0, std::memory_order_relaxed);
    return false;
  }
  // Measured here rather than in the handler: deriving it is not something a
  // signal handler may do, and the target is about to be parked anyway.
  target_bounds_ = stackBoundsFor(thread);
  stop_requested_ = false;
  // Clears whatever the previous walk ended on. No target is parked yet, so
  // nothing else is looking at this.
  state_.store(State::Running, std::memory_order_release);
  // Opens the handler's gate, and has to happen before the signal that will
  // find it: release pairs with the acquire in isWalkTarget(). Naming the
  // thread rather than setting a flag is what stops the signal being answered
  // by whichever thread it happens to land on.
  walk_target_.store(thread, std::memory_order_release);
  if (::pthread_kill(thread, signum_) != 0) {
    // No target was ever reached, so leaving the gate open would mean a later
    // delivery to `thread` was answered on behalf of a walk that never ran.
    walk_target_.store(ThreadId{}, std::memory_order_release);
    return false;
  }

  // Bounded, because a delivered signal is not a handler that runs. A target
  // with this signal blocked in its mask leaves it pending indefinitely -
  // pthread_kill still reports success - and an unbounded wait here would then
  // wedge the sampling thread for the life of the process rather than costing
  // one walk. The same bound the target uses for its own park serves: both are
  // "how long this handshake is allowed to take".
  if (semWaitFor(&frames_ready_, park_timeout_) == 0) {
    return true;
  }

  // The wait ran out, and the target may be about to publish anyway. Claiming
  // the walk is what stops it: publishBatch() only posts if it can take
  // `Running` for itself, so the winner of this compare-exchange decides
  // whether a post is ever made.
  State expected = State::Running;
  if (!state_.compare_exchange_strong(
          expected, State::Abandoned, std::memory_order_acq_rel)) {
    // Lost the claim, so the target published at the very moment we gave up on
    // it. Its post is already made or a few instructions away, and taking it is
    // the only thing that keeps the semaphore balanced - so, exactly as on the
    // target's side of the same race, the walk carries on as if the wait had
    // never run out.
    semWait(&frames_ready_);
    return true;
  }

  // Won the claim: no batch will be published for this walk, so nothing is left
  // uncollected behind us. Close the gate too, since a signal still pending
  // will eventually be delivered and is no longer ours to answer.
  walk_target_.store(ThreadId{}, std::memory_order_release);
  return false;
}

bool StackWalk::wakeTarget() {
  State expected = State::Parked;
  if (!state_.compare_exchange_strong(
          expected, State::Running, std::memory_order_acq_rel)) {
    return false;
  }
  ::sem_post(&resume_);
  return true;
}

bool StackWalk::nextBatch() {
  // Set before the target is woken, since waking it is what lets it read this.
  stop_requested_ = false;
  if (!wakeTarget()) {
    // Either the target's park ran out and it is running again, or it never
    // parked in the first place. Nothing is left to build another batch, and
    // waiting for one would be waiting forever.
    return false;
  }
  return semWait(&frames_ready_) == 0;
}

void StackWalk::endWalk() {
  // Closed before the target is let go, so that a duplicate delivery queued
  // behind the one being answered - real-time signals queue rather than
  // collapse - finds the walk already over. The target parked inside the
  // handler passed the gate long ago and is released by what follows.
  walk_target_.store(ThreadId{}, std::memory_order_release);
  stop_requested_ = true;
  wakeTarget();

  // Waited for whether or not the release above found a target to wake: a park
  // that ran out took the target out of the handler on its own, but "on its
  // own" still means running the same few instructions on this object, and it
  // posts on the way past exactly as a released target does.
  //
  // Unbounded, unlike every other wait here, and the asymmetry is the point.
  // Those wait on something that may never be produced at all - a signal the
  // target has blocked, a sampler that has been suspended - so a bound turns a
  // wedged thread into a failed walk. This one waits on a thread that is
  // already running, inside a handler, owing exactly one post and blocked on
  // nothing to make it. Nothing can withhold that post; only the scheduler can
  // delay it. And a bound here could not be acted on in any case: carrying on
  // regardless is precisely the use-after-free this call exists to prevent, so
  // a timeout would leave nothing to do but wait again.
  semWait(&handler_exited_);
}

bool StackWalk::isWalkTarget() const {
  const ThreadId target = walk_target_.load(std::memory_order_acquire);
  // Checked before pthread_equal rather than left to it: the marker is not a
  // thread id, and handing one that never named a thread to pthread_equal is
  // not something POSIX promises anything about.
  return target != ThreadId{} && ::pthread_equal(target, ::pthread_self()) != 0;
}

void StackWalk::handleSignal(int, siginfo_t*, void* ucontext) {
  // The kernel does not save/restore errno around a handler.
  const int saved_errno = errno;

  // Null if a signal raced past the destructor's handler restore. Acquire
  // pairs with the constructor's release, so the members reached through this
  // pointer are the finished ones.
  StackWalk* self = s_active.load(std::memory_order_acquire);
  // Answered only on the thread a walk is actually waiting for. Every other
  // delivery - to a bystander, or to anyone at all between walks - is somebody
  // else's signal arriving on a number we happen to share, and capturing frames
  // for it would publish a batch no sampler asked for.
  if (self != nullptr && self->isWalkTarget()) {
    self->captureFrames(ucontext);
  }

  errno = saved_errno;
}

StackWalk::Publish StackWalk::publishBatch(size_t count) {
  num_frames_ = count;

  // Claimed before the post, so that a sampler woken by it never sees a state
  // left over from the last round trip - and, for the first batch, so that a
  // sampler whose own wait has just run out cannot be posted to after it has
  // stopped listening. Both sides come out of `Running` with a
  // compare-exchange, so exactly one of them wins and the loser can tell.
  State expected = State::Running;
  if (!state_.compare_exchange_strong(
          expected, State::Parked, std::memory_order_acq_rel)) {
    // Lost the claim, so the sampler stopped waiting for this batch before it
    // was built and is no longer listening. Posting now would leave the
    // semaphore a count up for whichever walk comes next to drain, so nothing
    // is posted and the target leaves the handler instead of parking for a
    // release nobody will send.
    return Publish::Refused;
  }

  // sem_post/sem_wait are async-signal-safe; mutexes and condition variables
  // are not, which makes semaphores the only usable handshake here.
  ::sem_post(&frames_ready_);

  // Park so the sampler sees a frozen stack rather than one that has already
  // unwound past the addresses it is about to inspect. Staying inside the
  // handler across batches is what keeps the frame pointers valid.
  if (semWaitFor(&resume_, park_timeout_) == 0) {
    return stop_requested_ ? Publish::Finished : Publish::Taken;
  }

  // The park ran out. Claiming it is what stops the sampler ever posting
  // `resume_`, so nothing is left uncollected behind us.
  expected = State::Parked;
  if (state_.compare_exchange_strong(
          expected, State::Abandoned, std::memory_order_acq_rel)) {
    return Publish::Finished;
  }

  // Lost the claim, which means the sampler released us at the very moment we
  // gave up on it. Its post is already made or on its way, and taking it is
  // the only thing that keeps the semaphore balanced - so the walk carries on
  // as if the park had never run out.
  semWait(&resume_);
  return stop_requested_ ? Publish::Finished : Publish::Taken;
}

void StackWalk::captureFrames(const void* ucontext) {
  auto uc = static_cast<const ucontext_t*>(ucontext);

#if defined(__APPLE__) && defined(__aarch64__)
  // StackWalk is compile-only on macOS because sem_init() is unsupported and
  // its constructor always throws. The context extraction still has to build
  // as part of the JIT library.
  const auto& state = uc->uc_mcontext->__ss;
  auto frame = reinterpret_cast<const StackFrame*>(
      __darwin_arm_thread_state64_get_fp(state));
  auto pc =
      reinterpret_cast<const void*>(__darwin_arm_thread_state64_get_pc(state));
  auto sp = static_cast<uintptr_t>(__darwin_arm_thread_state64_get_sp(state));
#elif defined(__linux__) && defined(__x86_64__)
  auto frame =
      reinterpret_cast<const StackFrame*>(uc->uc_mcontext.gregs[REG_RBP]);
  auto pc = reinterpret_cast<const void*>(uc->uc_mcontext.gregs[REG_RIP]);
  auto sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
#elif defined(__linux__) && defined(__aarch64__)
  auto frame = reinterpret_cast<const StackFrame*>(uc->uc_mcontext.regs[29]);
  auto pc = reinterpret_cast<const void*>(uc->uc_mcontext.pc);
  auto sp = static_cast<uintptr_t>(uc->uc_mcontext.sp);
#else
#error "StackWalk supports x86-64 and AArch64 only"
#endif

  // Where this thread's stack really is, as opposed to where the frame-pointer
  // register claims the frames are. Vouching for the bounds with the stack
  // pointer costs two comparisons and means bounds that turn out to describe
  // some other thread cost only speed.
  const StackBounds bounds = target_bounds_.clampedToStackPointer(sp);

  // The interrupted PC is not stored in any frame record, so it is emitted
  // once up front alongside the frame that is executing it.
  bool emit_pc = frame != nullptr;
  bool at_end = frame == nullptr;
  Cursor cursor{frame, bounds};

  // Whether any batch was taken, which is the same question as whether a
  // sampler is waiting for this handler to come back out. A delivery whose
  // very first batch is refused answers no walk at all - it is a signal that
  // arrived after the walk it would have belonged to was over - and posting
  // `handler_exited_` on its way out would leave a count for some later walk
  // to take in place of its own target's.
  bool answered_a_walk = false;

  while (true) {
    size_t count = 0;
    if (emit_pc) {
      frames_[count++] = {frame, pc};
      emit_pc = false;
    }

    while (!at_end && count < kBatchSize) {
      const void* return_address = cursor.returnAddress();
      const StackFrame* caller = cursor.step();
      if (caller == nullptr) {
        at_end = true;
        break;
      }
      frames_[count++] = {caller, return_address};
    }

    const Publish published = publishBatch(count);
    if (published != Publish::Refused) {
      answered_a_walk = true;
    }
    // The loop above leaves `count == kBatchSize` exactly when it stopped for
    // want of room rather than for want of frames, so a short batch tells the
    // sampler the walk is over and nothing else has to.
    if (published != Publish::Taken || at_end) {
      break;
    }
  }

  // The last thing this handler touches on the walker, and the whole of what
  // endWalk() is waiting for: after this the target is on its way out through
  // frames that hold no reference to the object, so the sampler is free to
  // destroy it.
  if (answered_a_walk) {
    ::sem_post(&handler_exited_);
  }
}
} // namespace cinderx

#endif

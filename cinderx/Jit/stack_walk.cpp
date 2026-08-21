// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/stack_walk.h"

#include <fmt/format.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace cinderx {

#if defined(__APPLE__) || defined(__linux__)
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

StackWalk::StackWalk(int signum) : signum_{signum} {
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

  struct sigaction action = {};
  action.sa_sigaction = handleSignal;
  ::sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_RESTART;

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

  if (::sigaction(signum_, &action, &prev_action_) != 0) {
    s_active.store(nullptr, std::memory_order_release);
    fmt::print(
        stderr,
        "StackWalk: sigaction failed: {}\n",
        std::system_category().message(errno));
    std::abort();
  }
}

StackWalk::~StackWalk() {
  if (::sigaction(signum_, &prev_action_, nullptr)) {
    JIT_ABORT("failed to restore signal handler");
  }
  s_active.store(nullptr, std::memory_order_release);
  ::sem_destroy(&frames_ready_);
  ::sem_destroy(&resume_);
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
  // Measured here rather than in the handler: deriving it is not something a
  // signal handler may do, and the target is about to be parked anyway.
  target_bounds_ = stackBoundsFor(thread);
  stop_requested_ = false;
  if (::pthread_kill(thread, signum_) != 0) {
    return false;
  }
  target_parked_ = semWait(&frames_ready_) == 0;
  return target_parked_;
}

bool StackWalk::nextBatch() {
  stop_requested_ = false;
  target_parked_ = false;
  ::sem_post(&resume_);
  target_parked_ = semWait(&frames_ready_) == 0;
  return target_parked_;
}

void StackWalk::endWalk() {
  if (!target_parked_) {
    return;
  }
  stop_requested_ = true;
  target_parked_ = false;
  ::sem_post(&resume_);
}

void StackWalk::handleSignal(int, siginfo_t*, void* ucontext) {
  // The kernel does not save/restore errno around a handler.
  const int saved_errno = errno;

  // Null if a signal raced past the destructor's handler restore. Acquire
  // pairs with the constructor's release, so the members reached through this
  // pointer are the finished ones.
  StackWalk* self = s_active.load(std::memory_order_acquire);
  if (self != nullptr) {
    self->captureFrames(ucontext);
  }

  errno = saved_errno;
}

bool StackWalk::publishBatch(size_t count) {
  num_frames_ = count;

  // sem_post/sem_wait are async-signal-safe; mutexes and condition variables
  // are not, which makes semaphores the only usable handshake here.
  ::sem_post(&frames_ready_);

  // Park so the sampler sees a frozen stack rather than one that has already
  // unwound past the addresses it is about to inspect. Staying inside the
  // handler across batches is what keeps the frame pointers valid.
  semWait(&resume_);

  return !stop_requested_;
}

void StackWalk::captureFrames(const void* ucontext) {
  auto uc = static_cast<const ucontext_t*>(ucontext);

#if defined(__x86_64__)
  auto frame =
      reinterpret_cast<const StackFrame*>(uc->uc_mcontext.gregs[REG_RBP]);
  auto pc = reinterpret_cast<const void*>(uc->uc_mcontext.gregs[REG_RIP]);
  auto sp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
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

    // The loop above leaves `count == kBatchSize` exactly when it stopped for
    // want of room rather than for want of frames, so a short batch tells the
    // sampler the walk is over and nothing else has to.
    if (!publishBatch(count) || at_end) {
      return;
    }
  }
}
#endif
} // namespace cinderx

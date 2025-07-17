// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"

#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace jit {

// Used to punt out of optimizations that require holding the GIL.
#define RETURN_MULTITHREADED_COMPILE(RETVAL)            \
  if constexpr (PY_VERSION_HEX >= 0x030C0000) {         \
    if (getThreadedCompileContext().compileRunning()) { \
      return RETVAL;                                    \
    }                                                   \
  }

class ThreadedCompileContext;
// Get a reference to the global ThreadedCompileContext.
ThreadedCompileContext& getThreadedCompileContext();

// Threaded-compile state for the whole process.
class ThreadedCompileContext {
 public:
  using WorkList = std::vector<BorrowedRef<>>;

  // Accept a list of translation units and set them as being compiled by
  // multiple worker threads.
  void startCompile(WorkList&& work_list) {
    // Can't use JIT_CHECK because this is included by log.h.
    assert(!compile_running_);
    work_list_ = std::move(work_list);
    compile_running_ = true;
    interpreter_ = PyInterpreterState_Get();
  }

  // Stop the current iteration of the multi-threaded compile, and return the
  // list of translation units that failed to compile.
  WorkList endCompile() {
    assert(compile_running_);
    compile_running_ = false;
    return std::move(retry_list_);
  }

  // Fetch the next translation unit to compile.
  BorrowedRef<> nextUnit() {
    BorrowedRef<> unit;
    lock();
    if (!work_list_.empty()) {
      unit = std::move(work_list_.back());
      work_list_.pop_back();
    }
    unlock();
    return unit;
  }

  // Mark a unit as having failed to compile and to be retried in the future.
  void retryUnit(BorrowedRef<> unit) {
    lock();
    retry_list_.emplace_back(std::move(unit));
    unlock();
  }

  // Check if there's a multi-threaded compile currently running.
  bool compileRunning() const {
    return compile_running_;
  }

  // Returns true if it's safe for the current thread to access data protected
  // by the threaded compile lock, either because no threaded compile is active
  // or the current thread holds the lock. May return true erroneously, but
  // shouldn't return false erroneously.
  bool canAccessSharedData() const {
    return !compileRunning() || holder() == std::this_thread::get_id();
  }

  static PyInterpreterState* interpreter() {
    if (getThreadedCompileContext().compileRunning()) {
      return getThreadedCompileContext().interpreter_;
    }
    return PyInterpreterState_Get();
  }

 private:
  friend class ThreadedCompileSerialize;

  void lock() {
    if (!compileRunning()) {
      return;
    }

    mutex_.lock();

    auto us = std::this_thread::get_id();

    auto prev_level = mutex_recursion_.fetch_add(1, std::memory_order_relaxed);
    if (prev_level == 0) {
      assert(holder() == std::thread::id{});
    } else {
      assert(holder() == us);
    }

    setHolder(us);
  }

  void unlock() {
    if (!compileRunning()) {
      return;
    }

    auto prev_level = mutex_recursion_.fetch_sub(1, std::memory_order_relaxed);
    if (prev_level == 1) {
      setHolder(std::thread::id{});
    } else {
      assert(holder() == std::this_thread::get_id());
    }

    mutex_.unlock();
  }

  std::thread::id holder() const {
    return mutex_holder_.load(std::memory_order_relaxed);
  }

  void setHolder(std::thread::id holder) {
    mutex_holder_.store(holder, std::memory_order_relaxed);
  }

  // This is only written by the main thread, and only when no worker threads
  // exist. While worker threads exist, it is only read (mostly by the worker
  // threads).
  bool compile_running_{false};

  // Despite the compiler not being recursive, it is not yet disciplined enough
  // to acquire the lock only when it absolutely knows it doesn't already have
  // it.
  std::recursive_mutex mutex_;

  // mutex_holder_ and mutex_recursion_ are used only in assertions, to protect
  // against one thread accessing data it shouldn't while a threaded compile is
  // active. False negatives in these assertions are OK, and can't be prevented
  // without additional locking that wouldn't be worth the overhead.
  //
  // False positives are not OK, and would be caused either by a thread reading
  // compile_running_ == true after the threaded compile has finished, or by a
  // thread reading someone else's id from mutex_holder_ while the first thread
  // has the lock. The former shouldn't happen because all stores to
  // compile_running_ happen while no worker threads exist, so there's no
  // opportunity for a data race. The latter shouldn't be possible because a
  // thread writes its own id to mutex_holder_, and within that thread the write
  // is sequenced before any reads of mutex_holder_ while doing work later.
  std::atomic<std::thread::id> mutex_holder_;
  std::atomic_size_t mutex_recursion_{0};

  // List of translation units to iterate through and compile.
  WorkList work_list_;

  // List of translation units that have failed to compile.
  WorkList retry_list_;

  // The interpreter state that kicked off the multi-threaded compile.
  PyInterpreterState* interpreter_;
};

// RAII device for acquiring the global threaded-compile lock.
class ThreadedCompileSerialize {
 public:
  ThreadedCompileSerialize() {
    getThreadedCompileContext().lock();
  }

  ~ThreadedCompileSerialize() {
    getThreadedCompileContext().unlock();
  }
};

/*
 * ThreadedRef is like Ref but it directly manipulates the
 * reference count instead of using the macros. The macros
 * have an issue on 3.12 and later where in debug builds
 * they need a PyInterpreterState to update the ref count stats.
 *
 * When doing multi-threaded compile we cannot access this from
 * the other threads because the main thread holds the GIL.
 *
 * This means these values will not have their ref counts
 * tracked in the debug build during ref leak tests, so care
 * must be used to use them safely.
 *
 * Unlike Ref a ThreadedRef cannot be stolen because it would
 * result in an imbalance of the reference count stats.
 */

template <typename T = PyObject>
  requires(!std::is_pointer_v<T>)
class ThreadedRef : public RefBase<T> {
 public:
  using RefBase<T>::RefBase;

  ~ThreadedRef() {
    decref(ptr_);
    ptr_ = nullptr;
  }

  explicit ThreadedRef(ThreadedRef&& other) noexcept {
    ptr_ = other.ptr_;
    other.ptr_ = nullptr;
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  explicit ThreadedRef(ThreadedRef<>&& other) {
    ptr_ = reinterpret_cast<T*>(other.release());
  }

  ThreadedRef& operator=(ThreadedRef&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    decref(ptr_);
    ptr_ = other.ptr_;
    other.ptr_ = nullptr;
    return *this;
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  ThreadedRef& operator=(ThreadedRef<>&& other) {
    if (this->get() == reinterpret_cast<T*>(other.get())) {
      return *this;
    }
    decref(ptr_);
    ptr_ = reinterpret_cast<T*>(other.release());
    return *this;
  }

  void reset(T* obj = nullptr) {
    incref(obj);
    decref(ptr_);
    ptr_ = obj;
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  void reset(PyObject* obj) {
    reset(reinterpret_cast<T*>(obj));
  }

  static ThreadedRef create(T* obj) {
    return ThreadedRef(obj);
  }

  // Stealing from another ThreadedRef doesn't make sense; either move it or
  // explicitly copy it.
  template <typename V>
  static ThreadedRef steal(const ThreadedRef<V>&) = delete;

  template <typename X = T>
    requires(!IsPyObject<X>)
  static ThreadedRef create(PyObject* obj) {
    return Ref(reinterpret_cast<T*>(obj));
  }

 private:
  ThreadedRef(const ThreadedRef&) = delete;
  ThreadedRef& operator=(const ThreadedRef&) = delete;

  explicit ThreadedRef(T* obj) {
    ptr_ = obj;
    incref(ptr_);
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  static void incref(T* obj) {
    incref(reinterpret_cast<PyObject*>(obj));
  }

  static void incref(PyObject* obj) {
    if (obj != nullptr && !_Py_IsImmortal(obj)) {
      incref_total(ThreadedCompileContext::interpreter());
      obj->ob_refcnt++;
    }
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  static void decref(T* obj) {
    decref(reinterpret_cast<PyObject*>(obj));
  }

  static void decref(PyObject* obj) {
    if (obj != nullptr && !_Py_IsImmortal(obj)) {
      decref_total(ThreadedCompileContext::interpreter());
      if (--obj->ob_refcnt == 0) {
        _Py_Dealloc((PyObject*)obj);
      }
    }
  }

  using RefBase<T>::ptr_;
};

} // namespace jit

template <typename T>
struct std::hash<jit::ThreadedRef<T>> {
  size_t operator()(const jit::ThreadedRef<T>& ref) const {
    std::hash<T*> hasher;
    return hasher(ref.get());
  }
};

template <typename T>
struct TransparentThreadedRefHasher {
  using is_transparent = void;

  size_t operator()(const jit::ThreadedRef<T>& ref) const {
    return std::hash<jit::ThreadedRef<T>>{}(ref);
  }
};

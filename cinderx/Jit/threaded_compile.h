// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compilation_lock.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cinderx::jit {

// Threaded-compile state. Stack-allocated in pyjit.cpp when a multi-threaded
// or background compile starts. Its address is published via thread-local
// storage so workers can find it.
class ThreadedCompileContext {
 public:
  using WorkList = std::vector<Ref<>>;

  // Used for background compilation (single background thread, no work list).
  ThreadedCompileContext();
  // Used for batch multi-threaded compilation.
  explicit ThreadedCompileContext(WorkList&& work_list);
  ~ThreadedCompileContext();

  ThreadedCompileContext(const ThreadedCompileContext&) = delete;
  ThreadedCompileContext(ThreadedCompileContext&&) = delete;
  ThreadedCompileContext& operator=(const ThreadedCompileContext&) = delete;
  ThreadedCompileContext& operator=(ThreadedCompileContext&&) = delete;

  // Stop the current iteration of the multi-threaded compile, and return the
  // list of translation units that failed to compile.
  WorkList endCompile();

  Ref<> nextUnit();

  // Mark a unit as having failed to compile and to be retried in the future.
  void retryUnit(Ref<>&& unit);

  // Mark a unit as being compiled and store it's reference for releasing later.
  void retireUnit(Ref<>&& unit);

  // Check if the current thread is currently participating in a multi-threaded
  // or background compile.
  static bool compileRunning();

  // Worker-side: set TLS to indicate we are inside a worker with a saved
  // tstate.
  void beginWorker(PyThreadState* tstate);
  void endWorker();

  void releaseGil();

  // Returns true if it's safe for the current thread to access data protected
  // by the threaded compile lock, either because no threaded compile is active
  // or the current thread holds the GIL via ThreadedCompileGILHolder.
  static bool canAccessSharedData();

  static PyInterpreterState* interpreter();
  static PyThreadState* tstate();

  static ThreadedCompileContext* current();

 private:
  friend class ThreadedCompileGILHolder;

  static void lock();
  static void unlock();

  // List of translation units to iterate through and compile.
  WorkList work_list_;

  // List of translation units that have failed to compile.
  WorkList retry_list_;

  // References handed back by workers, released by endCompile().
  WorkList retired_list_;

  // The interpreter state that kicked off the multi-threaded compile.
  PyInterpreterState* interpreter_{nullptr};

  static thread_local ThreadedCompileContext* current_;
  // Thread-local state for GIL-based serialization.
  // threaded_compile_tstate_ holds the PyThreadState* that was saved when the
  // GIL was released. When non-null, the thread is running without the GIL and
  // threaded_compile_tstate_ is the threadstate needed to restore (acquire) the
  // GIL via PyEval_RestoreThread. gil_lock_depth_ tracks recursion of the
  // GIL-based lock.
  static thread_local PyThreadState* threaded_compile_tstate_;
  static thread_local int gil_lock_depth_;

  // Saved main thread state when GIL is released (batch compile).
  PyThreadState* main_{nullptr};
  bool ended_{false};
};

// RAII device for acquiring the GIL when doing multi-threaded compilation.
class ThreadedCompileGILHolder {
 public:
  ThreadedCompileGILHolder() {
    ThreadedCompileContext::lock();
  }

  ~ThreadedCompileGILHolder() {
    ThreadedCompileContext::unlock();
  }

  ThreadedCompileGILHolder(const ThreadedCompileGILHolder&) = delete;
  ThreadedCompileGILHolder(ThreadedCompileGILHolder&&) = delete;
  ThreadedCompileGILHolder& operator=(const ThreadedCompileGILHolder&) = delete;
  ThreadedCompileGILHolder& operator=(ThreadedCompileGILHolder&&) = delete;
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
 * Can only be stolen from another strong reference which loses
 * its reference to avoid an imbalance of the reference count stats.
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

  static ThreadedRef steal(T* obj) {
    return ThreadedRef(reinterpret_cast<T*>(obj), StealTag{});
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  static ThreadedRef create(PyObject* obj) {
    return ThreadedRef(reinterpret_cast<T*>(obj));
  }

 private:
  ThreadedRef(const ThreadedRef&) = delete;
  ThreadedRef& operator=(const ThreadedRef&) = delete;

  enum class StealTag {};
  ThreadedRef(T* obj, StealTag) {
    ptr_ = obj;
  }

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
#ifdef Py_GIL_DISABLED
      incref_total(ThreadedCompileContext::tstate());
#else
      incref_total(ThreadedCompileContext::interpreter());
#endif
#ifdef Py_GIL_DISABLED
      uint32_t local = _Py_atomic_load_uint32_relaxed(&obj->ob_ref_local);
      uint32_t new_local = local + 1;
      if (new_local == 0) {
        return;
      }
      if (_Py_IsOwnedByCurrentThread(obj)) {
        _Py_atomic_store_uint32_relaxed(&obj->ob_ref_local, new_local);
      } else {
        _Py_atomic_add_ssize(&obj->ob_ref_shared, (1 << _Py_REF_SHARED_SHIFT));
      }
#else
      obj->ob_refcnt++;
#endif
    }
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  static void decref(T* obj) {
    decref(reinterpret_cast<PyObject*>(obj));
  }

  static void decref(PyObject* obj) {
    if (obj != nullptr && !_Py_IsImmortal(obj)) {
#ifdef Py_GIL_DISABLED
      decref_total(ThreadedCompileContext::tstate());
#else
      decref_total(ThreadedCompileContext::interpreter());
#endif
#ifdef Py_GIL_DISABLED
      uint32_t local = _Py_atomic_load_uint32_relaxed(&obj->ob_ref_local);
      if (local == _Py_IMMORTAL_REFCNT_LOCAL) {
        return;
      }
      if (_Py_IsOwnedByCurrentThread(obj)) {
        local--;
        _Py_atomic_store_uint32_relaxed(&obj->ob_ref_local, local);
        if (local == 0) {
          _Py_MergeZeroLocalRefcount(obj);
        }
      } else {
        _Py_DecRefShared(obj);
      }
#else
      if (--obj->ob_refcnt == 0) {
        _Py_Dealloc((PyObject*)obj);
      }
#endif
    }
  }

  using RefBase<T>::ptr_;
};

} // namespace cinderx::jit

template <typename T>
struct std::hash<cinderx::jit::ThreadedRef<T>> {
  size_t operator()(const cinderx::jit::ThreadedRef<T>& ref) const {
    std::hash<T*> hasher;
    return hasher(ref.get());
  }
};

template <typename T>
struct TransparentThreadedRefHasher {
  using is_transparent = void;

  size_t operator()(const cinderx::jit::ThreadedRef<T>& ref) const {
    return std::hash<cinderx::jit::ThreadedRef<T>>{}(ref);
  }
};

// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <concepts>
#include <functional>
#include <type_traits>

template <typename T>
concept IsPyObject = std::same_as<PyObject, T>;

template <typename T>
class RefBase {
 public:
  RefBase() = default;
  /* implicit */ RefBase(std::nullptr_t) {}

  operator T*() const {
    return ptr_;
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  operator PyObject*() const {
    return getObj();
  }

  T* release() {
    auto ref = ptr_;
    ptr_ = nullptr;
    return ref;
  }

  T* get() const {
    return ptr_;
  }

  PyObject* getObj() const {
    return reinterpret_cast<PyObject*>(ptr_);
  }

  T* operator->() const {
    return ptr_;
  }

  bool operator==(std::nullptr_t) const {
    return ptr_ == nullptr;
  }

  bool operator!=(std::nullptr_t) const {
    return ptr_ != nullptr;
  }

 protected:
  T* ptr_{nullptr};
};

void incref_total(PyInterpreterState* ts);
void decref_total(PyInterpreterState* ts);

/*
 * BorrowedRef owns a borrowed reference to a PyObject.
 *
 * It is intended to be used in place of a raw PyObject* to codify
 * the ownership semantics of the reference explicity in the type system
 * (as opposed to in a comment).
 *
 */
template <typename T = PyObject>
  requires(!std::is_pointer_v<T>)
class BorrowedRef : public RefBase<T> {
 public:
  using RefBase<T>::RefBase;

  BorrowedRef(T* obj) {
    ptr_ = obj;
  }

  // Allow conversion from PyObject* to any BorrowedRef<V>.
  //
  // PyObject doesn't have "subclasses" in the same way that C++ does, so we
  // can't do a std::is_base_of_v<PyObject, X> check here.
  template <typename X = T>
    requires(!IsPyObject<X>)
  BorrowedRef(PyObject* ptr) : BorrowedRef(reinterpret_cast<X*>(ptr)) {}

  // Allow conversion from any BorrowedRef<V> to BorrowedRef<PyObject>.
  template <typename V, typename X = T>
    requires IsPyObject<X>
  BorrowedRef(const BorrowedRef<V>& other)
      : BorrowedRef(reinterpret_cast<PyObject*>(other.get())) {}

  BorrowedRef(const RefBase<T>& other) {
    ptr_ = other.get();
  }

  BorrowedRef& operator=(const RefBase<T>& other) {
    ptr_ = other.get();
    return *this;
  }

  void reset(T* obj = nullptr) {
    ptr_ = obj;
  }

 private:
  using RefBase<T>::ptr_;
};

template <typename T>
struct std::hash<BorrowedRef<T>> {
  size_t operator()(const BorrowedRef<T>& ref) const {
    std::hash<T*> hasher;
    return hasher(ref.get());
  }
};

/*
 * Ref owns a reference to a PyObject.
 *
 * It is intended to be a drop-in replacement for a PyObject* with the added
 * benefit that it automatically decrefs the underlying PyObject* when the
 * Ref is destroyed.
 *
 * A Ref cannot be copied; it uniquely owns its reference. Ownership can be
 * transferred via a move, or a BorrowedRef can be constructed from a Ref.
 *
 * One common use case is to use a Ref to create a new reference from a
 * borrowed reference that was returned from a call to the runtime, e.g.
 *
 *   auto new_ref = Ref<>::create(PyDict_GetItemString(d, "key"));
 *
 * In many cases we want to use a Ref to manage a new reference that is
 * returned as a raw PyObject* from the runtime. To do so, we steal the
 * reference that was returned by the runtime and store it in a Ref:
 *
 *   auto stolen_ref = Ref<>::steal(PyLong_FromLong(100));
 *
 * Note that a Ref cannot be constructed directly with a pointer value, as it's
 * not evident whether this would be creating a new reference or stealing one:
 *
 *   Ref<> ref{functionThatReturnsPyObjectStar()}; // Will not compile!
 *
 * Refs should also be used to indicate the ownership semantics of functions
 * w.r.t their arguments. Arguments that will be stolen should be Refs, whereas
 * arguments that will be borrowed should either be a BorrowedRef or a
 * reference to a Ref (discouraged).
 *
 * For example, consider `MyPyTuple_SetItem`, modeled after `PyTuple_SetItem`,
 * which steals a reference to the value being stored. We would write it as:
 *
 *   void MyPyTuple_SetItem(BorrowedRef<> tup, Py_ssize_t pos, Ref<> val);
 *
 * and call it via:
 *
 *   auto tup = Ref<>::steal(PyTuple_New(1));
 *   auto val = Ref<>::steal(PyLong_AsLong(100));
 *   MyPyTuple_SetItem(tup, 0, std::move(val));
 *
 * It's clear that we're transferring ownership of the reference in `val` to
 * `MyPyTuple_SetItem`.
 *
 */
template <typename T = PyObject>
  requires(!std::is_pointer_v<T>)
class Ref : public RefBase<T> {
 public:
  using RefBase<T>::RefBase;

  ~Ref() {
    Py_XDECREF(ptr_);
    ptr_ = nullptr;
  }

  Ref(Ref&& other) {
    ptr_ = other.ptr_;
    other.ptr_ = nullptr;
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  Ref(Ref<>&& other) {
    ptr_ = reinterpret_cast<T*>(other.release());
  }

  Ref& operator=(Ref&& other) {
    if (this == &other) {
      return *this;
    }
    Py_XDECREF(ptr_);
    ptr_ = other.ptr_;
    other.ptr_ = nullptr;
    return *this;
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  Ref& operator=(Ref<>&& other) {
    if (this->get() == reinterpret_cast<T*>(other.get())) {
      return *this;
    }
    Py_XDECREF(ptr_);
    ptr_ = reinterpret_cast<T*>(other.release());
    return *this;
  }

  void reset(T* obj = nullptr) {
    Py_XINCREF(obj);
    Py_XDECREF(ptr_);
    ptr_ = obj;
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  void reset(PyObject* obj) {
    reset(reinterpret_cast<T*>(obj));
  }

  static Ref steal(T* obj) {
    return Ref(obj, StealTag{});
  }

  static Ref create(T* obj) {
    return Ref(obj, CreateTag{});
  }

  template <typename X = T>
    requires(!IsPyObject<X>)
  static Ref steal(PyObject* obj) {
    return Ref(reinterpret_cast<T*>(obj), StealTag{});
  }

  // Stealing from another Ref doesn't make sense; either move it or explicitly
  // copy it.
  template <typename V>
  static Ref steal(const Ref<V>&) = delete;

  template <typename X = T>
    requires(!IsPyObject<X>)
  static Ref create(PyObject* obj) {
    return Ref(reinterpret_cast<T*>(obj), CreateTag{});
  }

 private:
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;

  enum class StealTag {};
  Ref(T* obj, StealTag) {
    ptr_ = obj;
  }

  enum class CreateTag {};
  Ref(T* obj, CreateTag) {
    ptr_ = obj;
    Py_XINCREF(ptr_);
  }

  using RefBase<T>::ptr_;
};

template <typename T>
struct std::hash<Ref<T>> {
  size_t operator()(const Ref<T>& ref) const {
    std::hash<T*> hasher;
    return hasher(ref.get());
  }
};

template <typename T>
struct TransparentRefHasher {
  using is_transparent = void;

  size_t operator()(const BorrowedRef<T>& ref) const {
    return std::hash<BorrowedRef<T>>{}(ref);
  }

  size_t operator()(const Ref<T>& ref) const {
    return std::hash<Ref<T>>{}(ref);
  }
};

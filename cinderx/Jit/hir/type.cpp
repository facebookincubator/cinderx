// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/type.h"

#include "cinderx/StaticPython/static_array.h"
#include "cinderx/StaticPython/type_code.h"

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace jit::hir {

static_assert(sizeof(Type) == 16, "Type should fit in two registers");
static_assert(sizeof(intptr_t) == sizeof(int64_t), "Expected 64-bit pointers");

namespace {

// For Types where it makes sense, map them to their corresponding
// PyTypeObject*.
const std::unordered_map<Type, PyTypeObject*>& typeToPyType() {
  static auto const map = [] {
    const std::unordered_map<Type, PyTypeObject*> result_map{
        {TObject, &PyBaseObject_Type},
        {TBool, &PyBool_Type},
        {TBytes, &PyBytes_Type},
        {TCell, &PyCell_Type},
        {TCode, &PyCode_Type},
        {TDict, &PyDict_Type},
        {TBaseException, reinterpret_cast<PyTypeObject*>(PyExc_BaseException)},
        {TFloat, &PyFloat_Type},
        {TFrame, &PyFrame_Type},
        {TFunc, &PyFunction_Type},
        {TGen, &PyGen_Type},
        {TList, &PyList_Type},
        {TLong, &PyLong_Type},
        {TSet, &PySet_Type},
        {TSlice, &PySlice_Type},
        {TTuple, &PyTuple_Type},
        {TType, &PyType_Type},
        {TUnicode, &PyUnicode_Type},
#if PY_VERSION_HEX < 0x030C0000
        {TWaitHandle, &Ci_PyWaitHandle_Type},
#endif
        {TNoneType, Py_TYPE(Py_None)},
    };

    // After construction, verify that all appropriate types have an entry in
    // this table. Except for TWaitHandle, which hasn't been ported to 3.12 yet
    // and TArray which is a heap type so can't be included in this static
    // table.
#define CHECK_TY(name, bits, lifetime, flags)        \
  JIT_CHECK(                                         \
      T##name <= TArray || T##name <= TWaitHandle || \
          ((flags) & kTypeHasUniquePyType) == 0 ||   \
          result_map.contains(T##name),              \
      "Type {} missing entry in typeToPyType()",     \
      T##name);
    HIR_TYPES(CHECK_TY)
#undef CHECK_TY

    return result_map;
  }();

  return map;
}

// Like typeToPyType(), but including Exact types in the key set (e.g., mapping
// TListExact -> PyList_Type).
const std::unordered_map<Type, PyTypeObject*>& typeToPyTypeWithExact() {
  static auto const map = [] {
    auto result_map = typeToPyType();
    for (auto& pair : typeToPyType()) {
      if (pair.first == TObject) {
        result_map.emplace(TObjectExact, &PyBaseObject_Type);
      } else if (pair.first == TLong) {
        result_map.emplace(TLongExact, &PyLong_Type);
      } else {
        result_map.emplace(pair.first & TBuiltinExact, pair.second);
      }
    }
    return result_map;
  }();

  return map;
}

// The inverse of typeToPyType().
const std::unordered_map<PyTypeObject*, Type>& pyTypeToType() {
  static auto const map = [] {
    std::unordered_map<PyTypeObject*, Type> result_map;
    for (auto& pair : typeToPyType()) {
      bool inserted = result_map.emplace(pair.second, pair.first).second;
      JIT_CHECK(inserted, "Duplicate key type: {}", pair.second->tp_name);
    }
    return result_map;
  }();

  return map;
}

// Like pyTypeToType(), but for Type::fromTypeExact(). It wants only the
// components of a type that can represent an exact type: the builtin exact
// type, or user-defined subtypes for exact specialization. These can be
// selected for most types by intersecting with TBuiltinExact or TUser,
// respectively.
//
// The only exceptions that we have to adjust for in this map are predefined
// Types that have other predefined Types as subtypes: TObject (where we leave
// out all other types) and TLong (where we leave out TBool).
const std::unordered_map<PyTypeObject*, Type>& pyTypeToTypeForExact() {
  static auto const map = [] {
    auto result_map = pyTypeToType();
    result_map.at(&PyBaseObject_Type) = TObjectExact | TObjectUser;
    result_map.at(&PyLong_Type) = TLongExact | TLongUser;
    return result_map;
  }();

  return map;
}

static std::string
truncatedStr(const char* data, std::size_t size, char delim) {
  const Py_ssize_t kMaxStrChars = 20;
  if (size <= kMaxStrChars) {
    return fmt::format("{}{}{}", delim, fmt::string_view{data, size}, delim);
  }
  return fmt::format(
      "{}{}{}...", delim, fmt::string_view{data, kMaxStrChars}, delim);
}

} // namespace

std::string Type::specString() const {
  if (hasIntSpec()) {
    if (*this <= TCBool) {
      return int_ ? "true" : "false";
    }
    if (*this <= TCPtr) {
      return fmt::format("{}", getStablePointer(ptr_));
    }
    JIT_DCHECK(
        *this <= TCInt8 || *this <= TCInt16 || *this <= TCInt32 ||
            *this <= TCInt64 || *this <= TCUInt8 || *this <= TCUInt16 ||
            *this <= TCUInt32 || *this <= TCUInt64,
        "Invalid specialization");
    return fmt::format("{}", int_);
  }

  if (hasDoubleSpec()) {
    return fmt::format("{}", double_);
  }

  if (!hasObjectSpec()) {
    if (hasTypeExactSpec()) {
      return fmt::format("{}:Exact", typeSpec()->tp_name);
    }
    return typeSpec()->tp_name;
  }

  if (*this <= TUnicode) {
    Py_ssize_t size;
    auto utf8 = PyUnicode_AsUTF8AndSize(objectSpec(), &size);
    if (utf8 == nullptr) {
      PyErr_Clear();
      return "encoding error";
    }
    return truncatedStr(utf8, size, '"');
  }

  if (typeSpec() == &PyCFunction_Type) {
    PyCFunctionObject* func =
        reinterpret_cast<PyCFunctionObject*>(objectSpec());
    const char* func_name = func->m_ml->ml_name;
    return fmt::format(
        "{}:{}:{}",
        typeSpec()->tp_name,
        func_name,
        getStablePointer(objectSpec()));
  }

  if (*this <= TType) {
    return fmt::format(
        "{}:obj", reinterpret_cast<PyTypeObject*>(objectSpec())->tp_name);
  }

  if (*this <= TBytes) {
    char* buffer;
    Py_ssize_t size;
    if (PyBytes_AsStringAndSize(objectSpec(), &buffer, &size) < 0) {
      PyErr_Clear();
      return "unknown error";
    }
    return truncatedStr(buffer, size, '\'');
  }

  if (*this <= TBool) {
    return objectSpec() == Py_True ? "True" : "False";
  }

  if (*this <= TLong) {
    int overflow = 0;
    auto value = PyLong_AsLongLongAndOverflow(objectSpec(), &overflow);
    if (value == -1) {
      if (overflow == -1) {
        return "underflow";
      }
      if (overflow == 1) {
        return "overflow";
      }
      if (PyErr_Occurred()) {
        PyErr_Clear();
        return "error";
      }
    }
    return fmt::format("{}", value);
  }

  if (*this <= TFloat) {
    auto value = PyFloat_AsDouble(objectSpec());
    if (value == -1.0 && PyErr_Occurred()) {
      return "error";
    }
    return fmt::format("{}", value);
  }

  if (*this <= TCode) {
    auto name = reinterpret_cast<PyCodeObject*>(objectSpec())->co_name;
    if (name != nullptr && PyUnicode_Check(name)) {
      return fmt::format("\"{}\"", PyUnicode_AsUTF8(name));
    }
  }

  // We want to avoid invoking arbitrary Python during compilation, so don't
  // call PyObject_Repr() or anything similar.
  return fmt::format(
      "{}:{}", typeSpec()->tp_name, getStablePointer(objectSpec()));
}

static auto typeToName() {
  std::unordered_map<Type, std::string> map{
#define TY(name, ...) {T##name, #name},
      HIR_TYPES(TY)
#undef TY
  };
  return map;
}

// Return a list of pairs of predefined type bit patterns and their name, used
// to create string representations of nontrivial union types.
static auto makeSortedBits() {
  std::vector<std::pair<Type::bits_t, std::string>> vec;

  // Exclude predefined types with nontrivial mortality, since their 'bits'
  // component is the same as the version with kLifetime{Top,Bottom}.
  //
  // Also exclude any strict supertype of Nullptr, to give strings like
  // {List|Dict|Nullptr} rather than {OptList|Dict}.
  auto include_bits = [](Type::bits_t bits, size_t flags, const char* name) {
    if ((flags & kTypeHasTrivialMortality) == 0 ||
        (((Type::kNullptr & bits) == Type::kNullptr) &&
         bits != Type::kNullptr)) {
      return false;
    }

    JIT_CHECK(
        (bits & Type::kObject) == bits || (bits & Type::kPrimitive) == bits,
        "Bits for {} should be subset of kObject or kPrimitive",
        name);
    return true;
  };
#define TY(name, bits, lifetime, flags)            \
  if (include_bits(Type::k##name, flags, #name)) { \
    vec.emplace_back(Type::k##name, #name);        \
  }
  HIR_TYPES(TY)
#undef TY

  // Sort the vector so types with the most bits set show up first.
  auto pred = [](auto& a, auto& b) {
    return popcount(a.first) > popcount(b.first);
  };
  std::sort(vec.begin(), vec.end(), pred);
  JIT_CHECK(
      vec.back().first == Type::kBottom, "Bottom should be at end of vec");
  vec.pop_back();
  return vec;
}

static std::string joinParts(std::vector<std::string>& parts) {
  if (parts.size() == 1) {
    return parts.front();
  }

  // Always show the parts in alphabetical order, regardless of which has the
  // most bits.
  std::sort(parts.begin(), parts.end());
  return fmt::format("{{{}}}", fmt::join(parts, "|"));
}

std::string Type::toString() const {
  std::string base;

  static auto const type_names = typeToName();
  auto it = type_names.find(unspecialized());
  if (it != type_names.end()) {
    base = it->second;
  } else {
    // Search the list of predefined type names, starting with the ones
    // containing the most bits.
    static auto const sorted_bits = makeSortedBits();
    bits_t bits_left = bits_;
    std::vector<std::string> parts, obj_parts;
    for (auto& pair : sorted_bits) {
      auto bits = pair.first;
      if ((bits_left & bits) == bits) {
        if (bits & kObject) {
          obj_parts.emplace_back(pair.second);
        } else {
          parts.emplace_back(pair.second);
        }
        bits_left ^= bits;
        if (bits_left == 0) {
          break;
        }
      }
    }
    JIT_CHECK(bits_left == 0, "Type contains invalid bits");

    // If we have a nontrivial lifetime component, turn obj_parts into one part
    // with that prepended, then combine that with parts.
    if (lifetime_ != kLifetimeTop && lifetime_ != kLifetimeBottom) {
      const char* mortal = lifetime_ == kLifetimeMortal ? "Mortal" : "Immortal";
      parts.emplace_back(fmt::format("{}{}", mortal, joinParts(obj_parts)));
    } else {
      parts.insert(parts.end(), obj_parts.begin(), obj_parts.end());
    }
    base = joinParts(parts);
  }

  return hasSpec() ? fmt::format("{}[{}]", base, specString()) : base;
}

std::string Type::toStringSafe() const {
  switch (spec_kind_) {
    case kSpecTop:
      return "Top";
    case kSpecType:
      return std::string("Type(") + (pytype_ ? pytype_->tp_name : "nullptr") +
          ")";
    case kSpecTypeExact:
      return std::string("TypeExact(") +
          (pytype_ ? pytype_->tp_name : "nullptr") + ")";
    case kSpecObject:
      return std::string("Object(") +
          (pyobject_ ? (pyobject_->ob_type ? pyobject_->ob_type->tp_name
                                           : "unknown_type")
                     : "nullptr") +
          ")";
    case kSpecInt:
      return "Int";
    case kSpecDouble:
      return "Double";
    case kSpecBottom:
      return "Bottom";
    default:
      return "Unknown";
  }
}

Type Type::fromTypeImpl(PyTypeObject* type, bool exact) {
  auto& type_map = exact ? pyTypeToTypeForExact() : pyTypeToType();

  auto it = type_map.find(type);
  if (it != type_map.end()) {
    return exact ? it->second & TBuiltinExact : it->second;
  }

  // Heap types that we're aware of, not statically known
  if (PyType_IsSubtype(type, PyStaticArray_Type)) {
    return TArray;
  }

  {
    ThreadedCompileSerialize guard;
    if (type->tp_mro == nullptr && !(type->tp_flags & Py_TPFLAGS_READY)) {
      PyType_Ready(type);
    }
  }
  JIT_CHECK(
      type->tp_mro != nullptr,
      "Type {}({}) has a null mro",
      type->tp_name,
      reinterpret_cast<void*>(type));

  PyObject* mro = type->tp_mro;
  for (ssize_t i = 0; i < PyTuple_GET_SIZE(mro); ++i) {
    auto ty = reinterpret_cast<PyTypeObject*>(PyTuple_GET_ITEM(mro, i));
    auto it_2 = type_map.find(ty);
    if (it_2 != type_map.end()) {
      auto bits = it_2->second.bits_;
      return Type{bits & kUser, kLifetimeTop, type, exact};
    }
  }
  JIT_ABORT(
      "Type {}({}) doesn't have object in its mro",
      type->tp_name,
      reinterpret_cast<void*>(type));
}

Type Type::fromType(PyTypeObject* type) {
  return fromTypeImpl(type, false);
}

Type Type::fromTypeExact(PyTypeObject* type) {
  return fromTypeImpl(type, true);
}

Type Type::fromObject(PyObject* obj) {
  if (obj == Py_None) {
    // There's only one value of type NoneType, so we don't need the result to
    // be specialized and it's always immortal.
    if constexpr (PY_VERSION_HEX >= 0x030C0000) {
      return TImmortalNoneType;
    }
    return TNoneType;
  }

  bits_t lifetime = [&]() {
    // Serialize to silence TSAN errors about accessing the reference count of
    // which can change during compliation. However, this is really a false
    // positive as the mortality of an object should not change during
    // compilation.
    ThreadedCompileSerialize guard;
    return _Py_IsImmortal(obj) ? kLifetimeImmortal : kLifetimeMortal;
  }();
  return Type{fromTypeExact(Py_TYPE(obj)).bits_, lifetime, obj};
}

PyTypeObject* Type::uniquePyType() const {
  if (hasObjectSpec()) {
    return nullptr;
  }
  if (hasTypeSpec()) {
    return typeSpec();
  }
  auto& type_map = typeToPyTypeWithExact();
  auto it = type_map.find(dropMortality());
  if (it != type_map.end()) {
    return it->second;
  }
  // Heap types that we're aware of, not statically known
  if (dropMortality() == TArray) {
    return PyStaticArray_Type;
  }
  return nullptr;
}

PyTypeObject* Type::runtimePyType() const {
  if (!isExact()) {
    return nullptr;
  }
  return hasTypeSpec() ? typeSpec() : uniquePyType();
}

std::optional<destructor> Type::runtimePyTypeDestructor() const {
  // If we do not have a runtime type that we can determine from this type, then
  // we cannot reliably determine the destructor.
  auto type = runtimePyType();
  if (type == nullptr) {
    return std::nullopt;
  }

  // If the type is the none type (which we can statically determine), then we
  // should not return the destructor. It's technically harmless to call it in
  // 3.11+, but in 3.10 it will crash.
  if (type == Py_TYPE(Py_None)) {
    return std::nullopt;
  }

  // Since we now have a destructor function that we can return, we can make it
  // into an optional and return it.
  return std::make_optional(type->tp_dealloc);
}

PyObject* Type::asObject() const {
  if (*this <= TNoneType) {
    return Py_None;
  }
  if (hasObjectSpec()) {
    return objectSpec();
  }
  return nullptr;
}

bool Type::isSingleValue() const {
  return *this <= TNoneType || *this <= TNullptr || hasObjectSpec() ||
      hasIntSpec() || hasDoubleSpec();
}

bool Type::operator<=(Type other) const {
  return (bits_ & other.bits_) == bits_ &&
      (lifetime_ & other.lifetime_) == lifetime_ && specSubtype(other);
}

bool Type::specSubtype(Type other) const {
  if (other.specKind() == kSpecTop || specKind() == kSpecBottom) {
    // Top is a supertype of everything, and Bottom is a subtype of everything.
    return true;
  }
  if (!hasSpec()) {
    // The only unspecialized Type that is a subtype of any specialized type is
    // TBottom, which is covered by the previous case.
    return false;
  }
  if ((hasIntSpec() || other.hasIntSpec()) ||
      (hasDoubleSpec() || other.hasDoubleSpec())) {
    // Primitive specializations don't support subtypes other than exact
    // equality.
    return *this == other;
  }

  // Check other's specialization type in decreasing order of specificity.
  if (other.hasObjectSpec()) {
    return hasObjectSpec() && objectSpec() == other.objectSpec();
  }
  if (other.hasTypeExactSpec()) {
    return hasTypeExactSpec() && typeSpec() == other.typeSpec();
  }
  return PyType_IsSubtype(typeSpec(), other.typeSpec());
}

Type Type::operator|(Type other) const {
  // Check trivial, specialization-preserving cases first.
  if (*this <= other) {
    return other;
  }
  if (other <= *this) {
    return *this;
  }

  bits_t bits = bits_ | other.bits_;
  bits_t lifetime = lifetime_ | other.lifetime_;

  Type no_spec{bits, lifetime};
  if (!hasTypeSpec() || !other.hasTypeSpec()) {
    // If either type doesn't have a specialization with a PyTypeObject*, the
    // result is only specialized if we hit one of the trivial cases up above.
    return no_spec;
  }

  if (hasObjectSpec() && other.hasObjectSpec() &&
      objectSpec() == other.objectSpec()) {
    JIT_DCHECK(
        *this == other,
        "Types with identical object specializations aren't equal");
    return *this;
  }

  PyTypeObject* type_a = typeSpec();
  PyTypeObject* type_b = other.typeSpec();
  PyTypeObject* supertype;
  // This logic will need to be more complicated if we want to more precisely
  // unify type specializations with a common supertype that isn't one of the
  // two.
  if (PyType_IsSubtype(type_a, type_b)) {
    supertype = type_b;
  } else if (PyType_IsSubtype(type_b, type_a)) {
    supertype = type_a;
  } else {
    return no_spec;
  }
  if (pyTypeToType().contains(supertype)) {
    // If the resolved supertype is a builtin type, the result doesn't need to
    // be specialized; the bits uniquely describe it already.
    return no_spec;
  }

  // The resulting specialization can only be exact if the two types are the
  // same exact type.
  bool is_exact =
      hasTypeExactSpec() && other.hasTypeExactSpec() && type_a == type_b;
  return Type{bits, lifetime, supertype, is_exact};
}

Type Type::operator&(Type other) const {
  bits_t bits = bits_ & other.bits_;
  bits_t lifetime = lifetime_ & other.lifetime_;

  // The kObject part of 'bits' and all of 'lifetime' are only meaningful if
  // both are non-zero. If one has gone to zero, clear the other as well. This
  // prevents creating types like "MortalBottom" or "LifetimeBottomList", both
  // of which we canonicalize to Bottom.
  if ((bits & kObject) == 0) {
    lifetime = kLifetimeBottom;
  } else if (lifetime == kLifetimeBottom) {
    bits &= ~kObject;
  }

  if (bits == kBottom) {
    return TBottom;
  }
  if (specSubtype(other)) {
    return Type{bits, lifetime, specKind(), int_};
  }
  if (other.specSubtype(*this)) {
    return Type{bits, lifetime, other.specKind(), other.int_};
  }

  // Two different, non-exact type specializations can still have a non-empty
  // intersection thanks to multiple inheritance. We can't represent the
  // intersection of two arbitrary classes, and we want to avoid returning a
  // type that's wider than either input type.
  //
  // Returning either the lhs or rhs would be correct within our constraints,
  // so keep this operation commutative by returning the type with the name
  // that's alphabetically first. Fall back to pointer comparison if they have
  // the same name.
  if (specKind() == kSpecType && other.specKind() == kSpecType) {
    auto type_a = typeSpec();
    auto type_b = other.typeSpec();
    auto cmp = std::strcmp(type_a->tp_name, type_b->tp_name);
    if (cmp < 0 || (cmp == 0 && type_a < type_b)) {
      return Type{bits, lifetime, type_a, false};
    }
    return Type{bits, lifetime, type_b, false};
  }

  return TBottom;
}

Type Type::operator-(Type rhs) const {
  if (*this <= rhs) {
    return TBottom;
  }
  if (!specSubtype(rhs)) {
    return *this;
  }

  bits_t bits = bits_ & ~(rhs.bits_ & kPrimitive);
  bits_t lifetime = lifetime_;
  auto bits_subset = [](bits_t a, bits_t b) { return (a & b) == a; };

  // We only want to remove the kObject parts of 'bits', or any part of
  // 'lifetime', when the corresponding parts of the other component are
  // subsumed by rhs's part.
  if (bits_subset(lifetime_, rhs.lifetime_)) {
    bits &= ~(rhs.bits_ & kObject);
  }
  if (bits_subset(bits_ & kObject, rhs.bits_ & kObject)) {
    lifetime &= ~rhs.lifetime_;
  }

  return Type{bits, lifetime, specKind(), int_};
}

Type Type::asBoxed() const {
  if (*this <= TCBool) {
    return TBool;
  }
  if (*this <= TCInt) {
    return TLong;
  }
  if (*this <= TCDouble) {
    return TFloat;
  }
  JIT_ABORT("{} does not have a boxed equivalent", *this);
}

unsigned int Type::sizeInBytes() const {
  if (*this <= (TCBool | TCInt8 | TCUInt8)) {
    return 1;
  }
  if (*this <= (TCInt16 | TCUInt16)) {
    return 2;
  }
  if (*this <= (TCInt32 | TCUInt32)) {
    return 4;
  }
  if (*this <= (TCInt64 | TCUInt64 | TCPtr | TCDouble | TObject | TNullptr)) {
    return 8;
  }
  JIT_ABORT("Unexpected type {}", *this);
}

Type OwnedType::toHir() const {
  int prim_type = _PyClassLoader_GetTypeCode(type);
  if (prim_type != TYPED_OBJECT) {
    JIT_CHECK(!optional, "primitive types cannot be optional");
    return prim_type_to_type(prim_type);
  }

  Type hir_type = exact ? Type::fromTypeExact(type) : Type::fromType(type);
  if (optional) {
    hir_type |= TNoneType;
  }
  return hir_type;
}

Type prim_type_to_type(int prim_type) {
  switch (prim_type) {
    case TYPED_BOOL:
      return TCBool;
    case TYPED_CHAR:
    case TYPED_INT8:
      return TCInt8;
    case TYPED_INT16:
      return TCInt16;
    case TYPED_INT32:
      return TCInt32;
    case TYPED_INT64:
      return TCInt64;
    case TYPED_UINT8:
      return TCUInt8;
    case TYPED_UINT16:
      return TCUInt16;
    case TYPED_UINT32:
      return TCUInt32;
    case TYPED_UINT64:
      return TCUInt64;
    case TYPED_OBJECT:
      return TOptObject;
    case TYPED_DOUBLE:
      return TCDouble;
    case TYPED_ERROR:
      return TCInt32;
    default:
      JIT_ABORT("Non-primitive or unsupported Python type: {}", prim_type);
  }
}

} // namespace jit::hir

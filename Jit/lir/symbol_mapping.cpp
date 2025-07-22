// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/symbol_mapping.h"

#include "cinderx/python.h"

namespace jit::lir {

const uint64_t* pyFunctionFromName(std::string_view name) {
  static const std::unordered_map<std::string_view, uint64_t> mapping = {
      {"PyType_IsSubtype", reinterpret_cast<uint64_t>(PyType_IsSubtype)},
      {"PyErr_Format", reinterpret_cast<uint64_t>(PyErr_Format)},
      {"PyExc_TypeError", reinterpret_cast<uint64_t>(PyExc_TypeError)},
      {"PyLong_FromLong", reinterpret_cast<uint64_t>(PyLong_FromLong)},
      {"PyLong_FromUnsignedLong",
       reinterpret_cast<uint64_t>(PyLong_FromUnsignedLong)},
      {"PyLong_FromSsize_t", reinterpret_cast<uint64_t>(PyLong_FromSsize_t)},
      {"PyLong_FromSize_t", reinterpret_cast<uint64_t>(PyLong_FromSize_t)},
      {"PyLong_AsSize_t", reinterpret_cast<uint64_t>(PyLong_AsSize_t)},
      {"PyLong_AsSsize_t", reinterpret_cast<uint64_t>(PyLong_AsSsize_t)},
  };

  auto it = mapping.find(name);
  return it != mapping.end() ? &it->second : nullptr;
}

} // namespace jit::lir

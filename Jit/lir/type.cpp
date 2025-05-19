// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/type.h"

#include <ostream>

namespace jit::lir {

size_t bitSize(DataType dt) {
  switch (dt) {
    case DataType::k8bit:
      return 8;
    case DataType::k16bit:
      return 16;
    case DataType::k32bit:
      return 32;
    case DataType::k64bit:
    case DataType::kDouble:
    case DataType::kObject:
      return 64;
    default:
      break;
  }
  throw std::runtime_error{fmt::format("Unrecognized LIR DataType: {}", dt)};
}

std::ostream& operator<<(std::ostream& os, OperandType ty) {
  switch (ty) {
#define OPERAND_TYPE_STRINGIFY(V, ...) \
  case OperandType::k##V:              \
    return os << #V;
    FOREACH_OPERAND_TYPE(OPERAND_TYPE_STRINGIFY)
#undef OPERAND_TYPE_STRINGIFY
    default:
      break;
  }
  return os << "<unknown OperandType " << static_cast<uint8_t>(ty) << ">";
}

std::ostream& operator<<(std::ostream& os, DataType dt) {
  switch (dt) {
#define OPERAND_DATA_TYPE_STRINGIFY(V, ...) \
  case DataType::k##V:                      \
    return os << #V;
    FOREACH_OPERAND_DATA_TYPE(OPERAND_DATA_TYPE_STRINGIFY)
#undef OPERAND_DATA_TYPE_STRINGIFY
    default:
      break;
  }
  return os << "<unknown DataType " << static_cast<uint8_t>(dt) << ">";
}

} // namespace jit::lir

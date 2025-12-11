// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/hir_stats.h"

namespace jit::hir {

void HIRStats::Run(Function& irfunc) {
  for (auto& block : irfunc.cfg.blocks) {
    for (auto& instr : block) {
      stats_.instrs[std::string(instr.opname())]++;
      if (Register* output = instr.output()) {
        Type output_type = output->type();
        std::string output_type_str =
            getThreadedCompileContext().compileRunning()
            ? output_type.toStringSafe()
            : output_type.toString();
        stats_.output_types[output_type_str]++;
      }
    }
  }
}

void HIRStats::Stats::dump(std::string_view function_name) const {
  auto escapeJsonString = [](std::string_view s) {
    std::string result;
    result.reserve(s.size());
    for (unsigned char c : s) {
      switch (c) {
        case '"':
          result += "\\\"";
          break;
        case '\\':
          result += "\\\\";
          break;
        case '\n':
          result += "\\n";
          break;
        case '\r':
          result += "\\r";
          break;
        case '\t':
          result += "\\t";
          break;
        case '\b':
          result += "\\b";
          break;
        case '\f':
          result += "\\f";
          break;
        default:
          // Handle control characters (0x00-0x1F) and non-ASCII bytes
          // (0x80-0xFF)
          if (c < 0x20 || c >= 0x80) {
            result += fmt::format("\\u{:04x}", c);
          } else {
            result += c;
          }
          break;
      }
    }
    return result;
  };

  std::string result = fmt::format(
      "{{\"function\": \"{}\", \"instructions\": {{",
      escapeJsonString(function_name));
  bool first = true;
  for (const auto& [name, count] : instrs) {
    if (!first) {
      result += ", ";
    }
    result += fmt::format("\"{}\": {}", escapeJsonString(name), count);
    first = false;
  }
  result += "}, \"types\": {";
  first = true;
  for (const auto& [name, count] : output_types) {
    if (!first) {
      result += ", ";
    }
    result += fmt::format("\"{}\": {}", escapeJsonString(name), count);
    first = false;
  }
  result += "}}";
  JIT_LOG("Stats for {}: {}", function_name, result);
}

} // namespace jit::hir

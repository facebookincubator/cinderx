// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/mmap_file.h"
#include "cinderx/Jit/symbolizer_iface.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace jit {

class Symbolizer : public ISymbolizer {
 public:
  Symbolizer(const char* exe_path = "/proc/self/exe");

  bool isInitialized() const {
    return file_.isOpen();
  }

  ~Symbolizer() override {
    deinit();
  }

  std::optional<std::string_view> symbolize(const void* func) override;

 private:
  void deinit();

  std::optional<std::string_view> cache(
      const void* func,
      std::optional<std::string> name);

  MmapFile file_;

  // Stored as void* to avoid pulling in ELF structures into this header.  These
  // have type `const ElfW(Shdr)*`.
  const void* symtab_{nullptr};
  const void* strtab_{nullptr};

  // This cache is useful for performance and also critical for correctness.
  // Some of the symbols (for example, to shared objects) do not return owned
  // pointers. We must keep an object in this map for the string_view to point
  // to.
  std::unordered_map<const void*, std::optional<std::string>> cache_;
};

std::optional<std::string> demangle(const std::string& mangled_name);

// Symbolize and demangle the given function.
std::optional<std::string> symbolize(const void* func);

} // namespace jit

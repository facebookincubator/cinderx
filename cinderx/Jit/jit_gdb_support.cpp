// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/jit_gdb_support.h"

#include "cinderx/Common/compiler.h"
#include "cinderx/Common/define.h"
#include "cinderx/Common/log.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/elf/string.h"
#include "cinderx/Jit/elf/symbol.h"
#include "cinderx/Jit/elf/writer.h"
#include "cinderx/Jit/jit_unwind.h"

#include <fcntl.h>
#include <fmt/format.h>
#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>

// These layouts must match gdb/gdb/jit.h, which GDB does not export.
enum JITActions { JIT_NOACTION = 0, JIT_REGISTER_FN, JIT_UNREGISTER_FN };

struct JITCodeEntry {
  JITCodeEntry* next_entry;
  JITCodeEntry* prev_entry;
  const char* symfile_addr;
  uint64_t symfile_size;
};

struct JITDescriptor {
  uint32_t version;
  uint32_t action_flag;
  JITCodeEntry* relevant_entry;
  JITCodeEntry* first_entry;
};

// GDB breaks here to read new symbols. Weak linkage permits other JITs in the
// same process to use the interface too (e.g. LLVM ORC).
CINDERX_NOINLINE [[gnu::weak]] void __jit_debug_register_code() {
  __asm("");
}

// Registration is not synchronized; callers must serialize access.
[[gnu::weak]] JITDescriptor __jit_debug_descriptor = {
    1,
    JIT_NOACTION,
    nullptr,
    nullptr};

namespace cinderx::jit {

namespace {

// The DWARF v2 programs follow the original LuaJIT-derived GDB implementation.
class DwarfBuffer {
 public:
  template <class T>
  void write(T value) {
    const size_t offset = data_.size();
    data_.resize(offset + sizeof(T));
    std::memcpy(data_.data() + offset, &value, sizeof(T));
  }

  void writeString(std::string_view value) {
    const size_t offset = data_.size();
    data_.resize(offset + value.size() + 1);
    std::memcpy(data_.data() + offset, value.data(), value.size());
  }

  void writeUleb128(uint32_t value) {
    while (value >= 0x80) {
      write<uint8_t>((value & 0x7f) | 0x80);
      value >>= 7;
    }
    write<uint8_t>(value);
  }

  void writeSleb128(int64_t value) {
    while (value < -64 || value >= 64) {
      write<uint8_t>((value & 0x7f) | 0x80);
      value >>= 7;
    }
    write<uint8_t>(value & 0x7f);
  }

  size_t beginSection() {
    const size_t offset = data_.size();
    write<uint32_t>(0);
    return offset;
  }

  void endSection(size_t offset) {
    const size_t size = data_.size() - offset - sizeof(uint32_t);
    JIT_THROW_IF(size >= 0xfffffff0, "DWARF32 section is too large");
    const uint32_t encoded_size = size;
    std::memcpy(data_.data() + offset, &encoded_size, sizeof(encoded_size));
  }

  std::span<const std::byte> bytes() const {
    return data_;
  }

 private:
  std::vector<std::byte> data_;
};

DwarfBuffer buildDebugInfo(
    std::string_view filename,
    uintptr_t code_addr,
    uint32_t code_size) {
  DwarfBuffer info;
  const size_t section = info.beginSection();
  info.write<uint16_t>(2); // DWARF version.
  info.write<uint32_t>(0); // Abbreviation offset.
  info.write<uint8_t>(sizeof(uintptr_t));
  info.writeUleb128(1); // Compile unit abbreviation.
  info.writeString(filename);
  info.write<uintptr_t>(code_addr);
  info.write<uintptr_t>(code_addr + code_size);
  info.write<uint32_t>(0); // Statement list offset.
  info.endSection(section);
  return info;
}

DwarfBuffer buildDebugLine(
    std::string_view filename,
    int lineno,
    uintptr_t code_addr,
    uint32_t code_size) {
  DwarfBuffer line;
  const size_t section = line.beginSection();
  line.write<uint16_t>(2);
  const size_t header = line.beginSection();
  line.write<uint8_t>(1); // Minimum instruction length.
  line.write<uint8_t>(1); // Default is_stmt.
  line.write<int8_t>(0); // Line base.
  line.write<uint8_t>(2); // Line range.
  line.write<uint8_t>(4); // First special opcode.
  line.write<uint8_t>(0); // DW_LNS_copy has no operands.
  line.write<uint8_t>(1); // DW_LNS_advance_pc has one operand.
  line.write<uint8_t>(1); // DW_LNS_advance_line has one operand.
  line.write<uint8_t>(0); // End directory table.
  line.writeString(filename);
  line.writeUleb128(0); // Directory index.
  line.writeUleb128(0); // Modification time.
  line.writeUleb128(0); // File size.
  line.write<uint8_t>(0); // End file table.
  line.endSection(header);

  line.write<uint8_t>(0); // Extended opcode.
  line.writeUleb128(1 + sizeof(uintptr_t));
  line.write<uint8_t>(2); // DW_LNE_set_address.
  line.write<uintptr_t>(code_addr);
  if (lineno != 0) {
    line.write<uint8_t>(3); // DW_LNS_advance_line.
    line.writeSleb128(int64_t{lineno} - 1);
  }
  line.write<uint8_t>(1); // DW_LNS_copy.
  line.write<uint8_t>(2); // DW_LNS_advance_pc.
  line.writeUleb128(code_size);
  line.write<uint8_t>(0);
  line.writeUleb128(1);
  line.write<uint8_t>(1); // DW_LNE_end_sequence.
  line.endSection(section);
  return line;
}

bool writeElfObject(int fd, std::span<const std::byte> bytes) {
  while (!bytes.empty()) {
    const int64_t written =
        write(fd, bytes.data(), std::min(bytes.size(), size_t{INT_MAX}));
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    bytes = bytes.subspan(written);
  }
  return true;
}

int registerDebugElf(
    std::span<const std::byte> object,
    const char* type,
    void* code_addr) {
  JIT_THROW_IF(
      object.size() > SIZE_MAX - sizeof(JITCodeEntry),
      "ELF allocation overflow");
  char* raw =
      static_cast<char*>(calloc(1, sizeof(JITCodeEntry) + object.size()));
  if (raw == nullptr) {
    JIT_DLOG("Failed to allocate space for JITCodeEntry + ELFObject");
    return 0;
  }
  char* elf_object = raw + sizeof(JITCodeEntry);
  std::memcpy(elf_object, object.data(), object.size());

  if (getConfig().gdb.write_elf_objects) {
    const std::string filename =
        fmt::format("/tmp/cinder_{}_{}_elf", type, code_addr);
#ifdef WIN32
    constexpr int kBinaryFlag = _O_BINARY;
#else
    constexpr int kBinaryFlag = 0;
#endif
    if (int fd = open(
            filename.c_str(), O_CREAT | O_RDWR | O_TRUNC | kBinaryFlag, 0600);
        fd >= 0) {
      if (!writeElfObject(fd, object)) {
        JIT_DLOG("Failed to write to {}", filename);
      }
      if (close(fd) < 0) {
        JIT_DLOG("Failed to close {}", filename);
      }
    } else {
      JIT_DLOG("Failed to open {}", filename);
    }
  }

  auto* entry = reinterpret_cast<JITCodeEntry*>(raw);
  entry->symfile_addr = elf_object;
  entry->symfile_size = object.size();
  JITCodeEntry* next = __jit_debug_descriptor.first_entry;
  entry->next_entry = next;
  if (next != nullptr) {
    next->prev_entry = entry;
  }
  __jit_debug_descriptor.first_entry = entry;
  __jit_debug_descriptor.relevant_entry = entry;
  __jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
  __jit_debug_register_code();
  return 1;
}

} // namespace

std::vector<std::byte> buildDebugElf(
    std::string_view function_name,
    std::string_view filename,
    int lineno,
    const void* code,
    size_t code_size) {
  JIT_THROW_IF(code_size > UINT32_MAX, "Code range is too large for DWARF v2");
  const uintptr_t code_addr = reinterpret_cast<uintptr_t>(code);
  const uint64_t frame_offset = (uint64_t{code_size} + 7) & ~uint64_t{7};
  JIT_THROW_IF(
      frame_offset > UINTPTR_MAX - code_addr, "Debug code address overflow");
  if (filename.empty()) {
    filename = "<unknown>";
  }
  if (function_name.empty()) {
    function_name = "<unknown>";
  }

  elf::StringTable strings;
  elf::SymbolTable symbols;
  symbols.insert(
      elf::Symbol{
          .name_offset = strings.insert("cinderjit"),
          .info = elf::kFile,
          .section_index = elf::kAbsoluteSection});
  symbols.insert(
      elf::Symbol{
          .name_offset = strings.insert(function_name),
          .info = elf::kGlobal | elf::kFunc,
          .section_index = 1,
          .size = code_size});

  const DwarfBuffer info = buildDebugInfo(filename, code_addr, code_size);
  const DwarfBuffer line =
      buildDebugLine(filename, lineno, code_addr, code_size);
  // DW_TAG_compile_unit; DW_AT_name/string, low_pc/addr, high_pc/addr,
  // stmt_list/data4, then the attribute and abbreviation terminators.
  constexpr uint8_t abbrev[]{
      1, 0x11, 0, 3, 8, 0x11, 1, 0x12, 1, 0x10, 6, 0, 0, 0};
  std::vector<uint8_t> frame(jitUnwindEhFrameSize());
  frame.resize(
      buildJitUnwindEhFrame(frame.data(), frame.size(), code, code_size));

  const std::array<elf::Section, 7> sections{{
      {".text",
       {.type = elf::kNoBits,
        .flags = elf::kSectionAlloc | elf::kSectionExecutable,
        .address = code_addr,
        .size = code_size,
        .align = 16},
       {}},
      {".eh_frame",
       {.type = elf::kProgram,
        .flags = elf::kSectionAlloc,
        .address = code_addr + frame_offset,
        .align = sizeof(uintptr_t)},
       std::as_bytes(std::span{frame})},
      {".strtab", {.type = elf::kStringTable, .align = 1}, strings.bytes()},
      {".symtab",
       {.type = elf::kSymbolTable,
        .link = 3,
        .info = 2,
        .align = sizeof(uintptr_t),
        .entry_size = sizeof(elf::Symbol)},
       symbols.bytes()},
      {".debug_info", {.type = elf::kProgram, .align = 1}, info.bytes()},
      {".debug_abbrev",
       {.type = elf::kProgram, .align = 1},
       std::as_bytes(std::span{abbrev})},
      {".debug_line", {.type = elf::kProgram, .align = 1}, line.bytes()},
  }};
  elf::FileHeader header;
  header.machine = kBuildArch == Arch::kAarch64 ? 183 : 62;
  return elf::writeSections(header, sections);
}

int register_raw_debug_symbol(
    const char* function_name,
    const char* filename,
    int lineno,
    void* code_addr,
    size_t code_size,
    size_t /* stack_size */) {
  if (!getConfig().gdb.supported) {
    return 1;
  }
  const std::vector<std::byte> object =
      buildDebugElf(function_name, filename, lineno, code_addr, code_size);
  if (!registerDebugElf(object, function_name, code_addr)) {
    return 0;
  }
  JIT_DLOG(
      "Registered debug symbol at {} ({} bytes) for {} at {} ({} bytes)",
      static_cast<const void*>(object.data()),
      object.size(),
      function_name,
      code_addr,
      code_size);
  return 1;
}

int register_pycode_debug_symbol(
    PyCodeObject* codeobj,
    const char* fullname,
    jit::CompiledFunctionData& compiled_func) {
  if (!getConfig().gdb.supported) {
    return 1;
  }
  const size_t code_size = compiled_func.code.size();
  const void* code = compiled_func.code.data();
  if (code_size < 1 || compiled_func.stack_size == -1) {
    JIT_DLOG("Not registering invalid debug symbol at {}", code);
    return 0;
  }
  const char* filename = "<unknown>";
  PyObject* sourcefile = codeobj->co_filename;
  if (sourcefile != nullptr && PyUnicode_Check(sourcefile)) {
    filename = PyUnicode_AsUTF8(sourcefile);
    if (filename == nullptr) {
      filename = "<filename failed to encode to UTF8>";
      JIT_DLOG("Failed to encode filename for ELF debug object");
    }
  }
  const std::vector<std::byte> object = buildDebugElf(
      fullname, filename, codeobj->co_firstlineno, code, code_size);
  if (!registerDebugElf(object, "PyFunctionObject", const_cast<void*>(code))) {
    return 0;
  }
  JIT_DLOG(
      "Registered debug symbol at {} ({} bytes) for a function at {} ({} "
      "bytes)",
      static_cast<const void*>(object.data()),
      object.size(),
      code,
      code_size);
  return 1;
}

} // namespace cinderx::jit

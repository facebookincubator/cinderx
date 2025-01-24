// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/elf/reader.h"
#include "cinderx/Jit/elf/writer.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <sstream>

using namespace jit;

using ElfTest = RuntimeTest;

namespace {

bool sectionExists(std::span<const std::byte> bytes, std::string_view name) {
  // Will be non-nullptr if it exists, but can still be empty.
  return elf::findSection(bytes, name).data() != nullptr;
}

void verifyElf(std::span<const std::byte> bytes) {
  // Verify the magic ELF bytes at the start.
  ASSERT_EQ(static_cast<uint8_t>(bytes[0]), 0x7f);
  ASSERT_EQ(static_cast<char>(bytes[1]), 'E');
  ASSERT_EQ(static_cast<char>(bytes[2]), 'L');
  ASSERT_EQ(static_cast<char>(bytes[3]), 'F');

  // Standard sections are all there.
  ASSERT_TRUE(sectionExists(bytes, ".text"));
  ASSERT_TRUE(sectionExists(bytes, ".dynsym"));
  ASSERT_TRUE(sectionExists(bytes, ".dynstr"));
  ASSERT_TRUE(sectionExists(bytes, ".dynamic"));
  ASSERT_TRUE(sectionExists(bytes, ".hash"));
  ASSERT_TRUE(sectionExists(bytes, ".shstrtab"));

  // Custom sections are there too.
  ASSERT_TRUE(sectionExists(bytes, elf::kFuncNoteSectionName));
}

} // namespace

TEST_F(ElfTest, Junk) {
  std::vector<uint8_t> elf;
  elf.push_back(0x7f);
  elf.push_back('E');
  elf.push_back('L');
  elf.push_back('F');
  for (uint8_t i = 1; i < 255; ++i) {
    elf.push_back(i);
  }
  auto bytes = std::as_bytes(std::span{elf});
  ASSERT_THROW(elf::findSection(bytes, ".text"), std::runtime_error);
}

TEST_F(ElfTest, EmptyEntries) {
  std::stringstream ss;
  elf::writeEntries(ss, {});
  std::string result = ss.str();

  verifyElf(std::as_bytes(std::span{result}));
}

TEST_F(ElfTest, OneEntry) {
  constexpr const char* source = R"(
def func(x):
  return x + 1
)";
  Ref<PyObject> func_obj{compileAndGet(source, "func")};
  ASSERT_TRUE(func_obj != nullptr);

  BorrowedRef<PyFunctionObject> func{func_obj};
  BorrowedRef<PyCodeObject> code{func->func_code};
  std::unique_ptr<CompiledFunction> compiled_func = Compiler().Compile(func);

  std::stringstream ss;

  elf::CodeEntry entry;
  entry.code = code;
  entry.compiled_code = compiled_func->codeBuffer();
  entry.normal_entry =
      reinterpret_cast<void*>(compiled_func->vectorcallEntry());
  entry.static_entry = compiled_func->staticEntry();
  entry.func_name = "func";
  entry.file_name = "spaghetti.exe";
  entry.lineno = 15;

  elf::writeEntries(ss, {entry});
  std::string result = ss.str();
  auto result_bytes = std::as_bytes(std::span{result});

  verifyElf(result_bytes);

  std::span<const std::byte> func_note_section =
      elf::findSection(result_bytes, elf::kFuncNoteSectionName);

  elf::NoteArray notes = elf::readNoteSection(func_note_section);
  ASSERT_EQ(notes.notes().size(), 1);
  ASSERT_EQ(notes.notes()[0].name, entry.func_name);

  elf::CodeNoteData note_data = elf::parseCodeNote(notes.notes()[0]);
  ASSERT_EQ(note_data.file_name, entry.file_name);
  ASSERT_EQ(note_data.lineno, entry.lineno);
  ASSERT_GT(note_data.size, 0);
  ASSERT_LT(note_data.size, 10000);
  ASSERT_GT(note_data.normal_entry_offset, 0);
  ASSERT_LT(note_data.normal_entry_offset, 10000);
  ASSERT_EQ(note_data.static_entry_offset, std::nullopt);
}

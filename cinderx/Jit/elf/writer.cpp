// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/elf/writer.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"

#include <ostream>
#include <sstream>

namespace jit::elf {

namespace {

constexpr uint64_t kPageSize = 0x1000;

constexpr uint64_t kTextStartAddress = kPageSize;

constexpr bool isAligned(uint64_t n, uint64_t align) {
  return n == roundUp(n, align);
}

uint64_t alignOffset(Object& elf, uint64_t align) {
  uint64_t new_offset = roundUp(elf.section_offset, align);
  uint64_t delta = new_offset - elf.section_offset;
  elf.section_offset = new_offset;
  return delta;
}

void checkAlignedSection(const SectionHeader& header, std::string_view name) {
  JIT_CHECK(
      isAligned(header.offset, header.align),
      "{} section has offset {:#x} which doesn't match alignment of {:#x}",
      name,
      header.offset,
      header.align);
}

void checkAlignedSegment(const SegmentHeader& header) {
  JIT_CHECK(
      isAligned(header.address - header.offset, header.align),
      "Segment with address {:#x} and offset {:#x} doesn't match alignment of "
      "{:#x}",
      header.address,
      header.offset,
      header.align);
}

void initFileHeader(Object& elf) {
  FileHeader& header = elf.file_header;
  header.segment_header_offset = offsetof(Object, segment_headers);
  header.segment_header_count = raw(SegmentIdx::kTotal);
  header.section_header_offset = offsetof(Object, section_headers);
  header.section_header_count = raw(SectionIdx::kTotal);
  header.section_name_index = raw(SectionIdx::kShstrtab);
}

void initTextSection(Object& elf, uint64_t text_size) {
  // Program bits. Occupies memory and is executable.  Text follows the section
  // header table after some padding.

  SectionHeader& header = elf.getSectionHeader(SectionIdx::kText);
  header.name_offset = elf.shstrtab.insert(".text");
  header.type = kProgram;
  header.flags = kSectionAlloc | kSectionExecutable;
  header.address = elf.section_offset;
  header.offset = elf.section_offset;
  header.size = text_size;
  header.align = 0x10;

  checkAlignedSection(header, ".text");

  elf.section_offset += header.size;
}

void initDynsymSection(Object& elf) {
  SectionHeader& header = elf.getSectionHeader(SectionIdx::kDynsym);
  header.name_offset = elf.shstrtab.insert(".dynsym");
  header.type = kSymbolTable;
  header.flags = kSectionAlloc | kSectionInfoLink;
  header.address = elf.section_offset;
  header.offset = elf.section_offset;
  header.size = elf.dynsym.bytes().size();
  header.link = raw(SectionIdx::kDynstr);
  // This is the index of the first global symbol, i.e. the first symbol after
  // the null symbol.
  header.info = 1;
  header.align = 0x8;
  header.entry_size = sizeof(Symbol);

  checkAlignedSection(header, ".dynsym");

  elf.section_offset += header.size;
}

void initDynstrSection(Object& elf) {
  SectionHeader& header = elf.getSectionHeader(SectionIdx::kDynstr);
  header.name_offset = elf.shstrtab.insert(".dynstr");
  header.type = kStringTable;
  header.flags = kSectionAlloc;
  header.address = elf.section_offset;
  header.offset = elf.section_offset;
  header.size = elf.dynstr.bytes().size();
  header.align = 0x1;

  checkAlignedSection(header, ".dynstr");

  elf.section_offset += header.size;
}

void initHashSection(Object& elf) {
  JIT_CHECK(
      isAligned(elf.section_offset, 0x8),
      "Hash section starts at unaligned address {:#x}",
      elf.section_offset);

  SectionHeader& header = elf.getSectionHeader(SectionIdx::kHash);
  header.name_offset = elf.shstrtab.insert(".hash");
  header.type = kHash;
  header.flags = kSectionAlloc;
  header.address = elf.section_offset;
  header.offset = elf.section_offset;
  header.size = elf.hash.size_bytes();
  header.link = raw(SectionIdx::kDynsym);
  header.align = 0x8;

  elf.section_offset += header.size;
}

void initFuncNoteSection(Object& elf) {
  JIT_CHECK(
      isAligned(elf.section_offset, 0x4),
      "Function note section starts at unaligned address {:#x}",
      elf.section_offset);

  SectionHeader& header = elf.getSectionHeader(SectionIdx::kFuncNote);
  header.name_offset = elf.shstrtab.insert(kFuncNoteSectionName);
  header.type = kNote;
  header.flags = kSectionAlloc;
  header.address = elf.section_offset;
  header.offset = elf.section_offset;
  header.size = elf.func_notes.size_bytes();
  header.align = 0x4;

  elf.section_offset += header.size;
}

void initDynamicSection(Object& elf) {
  JIT_CHECK(
      isAligned(elf.section_offset, kPageSize),
      "Dynamic section starts at unaligned address {:#x}",
      elf.section_offset);

  SectionHeader& header = elf.getSectionHeader(SectionIdx::kDynamic);
  header.name_offset = elf.shstrtab.insert(".dynamic");
  header.type = kDynamic;
  header.flags = kSectionAlloc | kSectionWritable;
  header.address = elf.section_offset;
  header.offset = elf.section_offset;
  header.size = elf.dynamic.bytes().size();
  header.link = raw(SectionIdx::kDynstr);
  header.entry_size = sizeof(Dyn);
  header.align = 0x8;

  elf.section_offset += header.size;
}

void initShstrtabSection(Object& elf) {
  SectionHeader& header = elf.getSectionHeader(SectionIdx::kShstrtab);
  header.name_offset = elf.shstrtab.insert(".shstrtab");
  header.type = kStringTable;
  header.offset = elf.section_offset;
  header.size = elf.shstrtab.bytes().size();
  header.align = 0x1;

  checkAlignedSection(header, ".shstrtab");

  elf.section_offset += header.size;
}

void initTextSegment(Object& elf) {
  SectionHeader& section = elf.getSectionHeader(SectionIdx::kText);

  // The .text section immediately follows all the ELF headers.
  SegmentHeader& header = elf.getSegmentHeader(SegmentIdx::kText);
  header.type = kSegmentLoadable;
  header.flags = kSegmentExecutable | kSegmentReadable;
  header.offset = section.offset;
  header.address = section.address;
  header.file_size = section.size;
  header.mem_size = header.file_size;
  header.align = 0x1000;

  checkAlignedSegment(header);
}

void initReadonlySegment(Object& elf) {
  // Starts at .dynsym and ends at .dynamic.
  SectionHeader& dynsym = elf.getSectionHeader(SectionIdx::kDynsym);
  SectionHeader& dynamic = elf.getSectionHeader(SectionIdx::kDynamic);
  JIT_CHECK(
      dynsym.address < dynamic.address,
      "Expecting sections to be in a specific order");

  SegmentHeader& header = elf.getSegmentHeader(SegmentIdx::kReadonly);
  header.type = kSegmentLoadable;
  header.flags = kSegmentReadable;
  header.offset = dynsym.offset;
  header.address = dynsym.address;
  header.file_size = dynamic.offset - dynsym.offset;
  header.mem_size = header.file_size;
  header.align = 0x1000;

  checkAlignedSegment(header);
}

void initReadwriteSegment(Object& elf) {
  SectionHeader& dynamic = elf.getSectionHeader(SectionIdx::kDynamic);

  SegmentHeader& header = elf.getSegmentHeader(SegmentIdx::kReadwrite);
  header.type = kSegmentLoadable;
  header.flags = kSegmentReadable | kSegmentWritable;
  header.offset = dynamic.offset;
  header.address = dynamic.address;
  header.file_size = dynamic.size;
  header.mem_size = header.file_size;
  header.align = 0x1000;

  checkAlignedSegment(header);
}

void initFuncNoteSegment(Object& elf) {
  SectionHeader& note = elf.getSectionHeader(SectionIdx::kFuncNote);

  SegmentHeader& header = elf.getSegmentHeader(SegmentIdx::kFuncNote);
  header.type = kSegmentNote;
  header.flags = kSegmentReadable;
  header.offset = note.offset;
  header.address = note.address;
  header.file_size = note.size;
  header.mem_size = header.file_size;
  header.align = note.align;
}

void initDynamicSegment(Object& elf) {
  SectionHeader& dynamic = elf.getSectionHeader(SectionIdx::kDynamic);

  SegmentHeader& header = elf.getSegmentHeader(SegmentIdx::kDynamic);
  header.type = kSegmentDynamic;
  header.flags = kSegmentReadable | kSegmentWritable;
  header.offset = dynamic.offset;
  header.address = dynamic.address;
  header.file_size = dynamic.size;
  header.mem_size = header.file_size;
  header.align = 0x1000;
}

void initDynamics(Object& elf) {
  // Has to be run after .dynsym, .dynstr, and .hash are mapped out.
  SectionHeader& dynsym = elf.getSectionHeader(SectionIdx::kDynsym);
  SectionHeader& dynstr = elf.getSectionHeader(SectionIdx::kDynstr);
  SectionHeader& hash = elf.getSectionHeader(SectionIdx::kHash);

  // TASK(T183002717): kNeeded for _cinderx.so.
  elf.dynamic.insert(DynTag::kNeeded, elf.libpython_name);

  elf.dynamic.insert(DynTag::kHash, hash.address);
  elf.dynamic.insert(DynTag::kStrtab, dynstr.address);
  elf.dynamic.insert(DynTag::kStrSz, dynstr.size);
  elf.dynamic.insert(DynTag::kSymtab, dynsym.address);
  elf.dynamic.insert(DynTag::kSymEnt, sizeof(Symbol));
}

template <class T>
void write(std::ostream& os, T* data, size_t size) {
  os.write(reinterpret_cast<const char*>(data), size);
  JIT_CHECK(!os.bad(), "Failed to write {} bytes of ELF output", size);
}

void write(std::ostream& os, std::span<const std::byte> bytes) {
  write(os, bytes.data(), bytes.size());
}

template <class T, class = std::enable_if_t<std::is_integral_v<T>>>
void write(std::ostream& os, T n) {
  write(os, &n, sizeof(n));
}

void pad(std::ostream& os, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    os.put(0);
  }
}

void writeFuncNote(std::ostream& os, const CodeEntry& entry) {
  auto code_start = reinterpret_cast<uintptr_t>(entry.compiled_code.data());

  write(os, static_cast<uint32_t>(entry.file_name.size()));
  os << entry.file_name;
  write(os, static_cast<uint32_t>(entry.lineno));

  write(os, hashBytecode(entry.code));
  write(os, static_cast<uint32_t>(entry.compiled_code.size()));

  auto normal_entry = reinterpret_cast<uintptr_t>(entry.normal_entry);
  auto static_entry = reinterpret_cast<uintptr_t>(entry.static_entry);

  // Entry points are encoded as offsets from the start of the code buffer
  // because we don't know the exact address the function will be linked into.
  auto normal_offset = normal_entry - code_start;
  auto static_offset =
      static_entry != 0 ? (static_entry - code_start) : kInvalidStaticOffset;

  write(os, static_cast<uint32_t>(normal_offset));
  write(os, static_cast<uint32_t>(static_offset));
}

void initFuncNotes(Object& elf, const std::vector<CodeEntry>& entries) {
  for (const CodeEntry& entry : entries) {
    std::stringstream ss;
    writeFuncNote(ss, entry);

    Note note;
    note.name = entry.func_name;
    note.desc = ss.str();
    // The version of Cinder at the time this was written :).  The number means
    // nothing, but it's good to have a unique number so disassembly tools don't
    // try to map it to other conventional uses.
    note.type = 0x30a05f0;

    elf.func_notes.insert(std::move(note));
  }
}

void writeHash(std::ostream& os, const HashTable& hash) {
  uint32_t nbuckets = hash.buckets().size();
  uint32_t nchains = hash.chains().size();

  write(os, &nbuckets, sizeof(nbuckets));
  write(os, &nchains, sizeof(nchains));
  write(os, std::as_bytes(hash.buckets()));
  write(os, std::as_bytes(hash.chains()));
}

void writeNote(std::ostream& os, const Note& note) {
  // Both the size and the serialized string include the NUL terminator.
  auto name_size = static_cast<uint32_t>(note.name.size() + 1);
  auto desc_size = static_cast<uint32_t>(note.desc.size() + 1);

  write(os, &name_size, sizeof(name_size));
  write(os, &desc_size, sizeof(desc_size));
  write(os, &note.type, sizeof(note.type));

  os << note.name << '\0';
  pad(os, roundUp(name_size, 4) - name_size);

  if (desc_size > 0) {
    os << note.desc << '\0';
    pad(os, roundUp(desc_size, 4) - desc_size);
  }
}

void writeNotes(std::ostream& os, const NoteArray& notes) {
  for (const Note& note : notes.notes()) {
    writeNote(os, note);
  }
}

void writeElf(
    std::ostream& os,
    const Object& elf,
    const std::vector<CodeEntry>& entries) {
  // Write out all the headers.
  write(os, &elf.file_header, sizeof(elf.file_header));
  write(os, &elf.section_headers, sizeof(elf.section_headers));
  write(os, &elf.segment_headers, sizeof(elf.segment_headers));
  pad(os, elf.header_padding);

  // Write out the actual sections themselves.
  for (const CodeEntry& entry : entries) {
    write(os, entry.compiled_code);
  }
  pad(os, elf.text_padding);

  write(os, elf.dynsym.bytes());
  write(os, elf.dynstr.bytes());
  pad(os, elf.dynsym_padding);

  writeHash(os, elf.hash);
  pad(os, elf.hash_padding);

  writeNotes(os, elf.func_notes);
  pad(os, elf.func_notes_padding);

  write(os, elf.dynamic.bytes());
  pad(os, elf.dynamic_padding);

  write(os, elf.shstrtab.bytes());
}

} // namespace

void writeEntries(std::ostream& os, const std::vector<CodeEntry>& entries) {
  Object elf;
  initFileHeader(elf);

  // Initialize symbols before any of the sections.
  uint64_t text_end_address = kTextStartAddress;
  for (const CodeEntry& entry : entries) {
    Symbol sym;
    sym.name_offset = elf.dynstr.insert(entry.func_name);
    sym.info = kGlobal | kFunc;
    sym.section_index = raw(SectionIdx::kText);
    sym.address = text_end_address;
    sym.size = entry.compiled_code.size();
    elf.dynsym.insert(std::move(sym));

    // TASK(T176630885): Not writing the filename or lineno yet.

    text_end_address += entry.compiled_code.size();
  }
  uint64_t text_size = text_end_address - kTextStartAddress;

  elf.libpython_name = elf.dynstr.insert("libpython3.10.so");

  // The headers are all limited to the zeroth page, sections begin on the next
  // page.
  elf.section_offset = offsetof(Object, header_padding);
  elf.header_padding = alignOffset(elf, kPageSize);
  JIT_CHECK(
      elf.section_offset == kTextStartAddress,
      "ELF headers were too big and went past the zeroth page: {:#x}",
      elf.section_offset);

  // Null section needs no extra initialization.

  initTextSection(elf, text_size);
  elf.text_padding = alignOffset(elf, kPageSize);

  initDynsymSection(elf);
  initDynstrSection(elf);
  elf.dynsym_padding = alignOffset(elf, 0x8);

  elf.hash.build(elf.dynsym, elf.dynstr);
  initHashSection(elf);
  elf.hash_padding = alignOffset(elf, 4);

  initFuncNotes(elf, entries);
  initFuncNoteSection(elf);
  elf.func_notes_padding = alignOffset(elf, kPageSize);

  initDynamics(elf);

  initDynamicSection(elf);
  elf.dynamic_padding = alignOffset(elf, 0x8);

  initShstrtabSection(elf);

  initTextSegment(elf);
  initReadonlySegment(elf);
  initReadwriteSegment(elf);
  initFuncNoteSegment(elf);
  initDynamicSegment(elf);

  writeElf(os, elf, entries);
}

} // namespace jit::elf

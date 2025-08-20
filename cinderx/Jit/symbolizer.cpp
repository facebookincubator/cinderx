// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/symbolizer.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/module_state.h"

#include <cxxabi.h>
#include <dlfcn.h>

#ifdef ENABLE_SYMBOLIZER
#include <elf.h>
#include <link.h> // for ElfW
#endif

#include <cstdio>
#include <cstdlib>

namespace jit {

namespace {

#ifdef ENABLE_SYMBOLIZER

struct SymbolResult {
  const void* func;
  std::optional<std::string> name;
};

bool hasELFMagic(const void* addr) {
  auto elf_hdr = reinterpret_cast<const ElfW(Ehdr)*>(addr);
  return (elf_hdr->e_ident[0] == ELFMAG0) && (elf_hdr->e_ident[1] == ELFMAG1) &&
      (elf_hdr->e_ident[2] == ELFMAG2) && (elf_hdr->e_ident[3] == ELFMAG3);
}

// Return 0 to continue iteration and non-zero to stop.
int findSymbolIn(struct dl_phdr_info* info, size_t, void* data) {
  // Continue until the first dynamic library is found. Looks like a bunch of
  // platforms put the main executable as the first entry, which has an empty
  // name. Skip it.
  if (info->dlpi_name == nullptr || info->dlpi_name[0] == 0) {
    return 0;
  }
  // Ignore linux-vdso.so.1 since it does not have an actual file attached.
  std::string_view name{info->dlpi_name};
  if (name.find("linux-vdso") != name.npos) {
    return 0;
  }
  if (info->dlpi_addr == 0) {
    JIT_LOG("Invalid ELF object '{}'", info->dlpi_name);
    return 0;
  }
  if (!hasELFMagic(reinterpret_cast<void*>(info->dlpi_addr))) {
    JIT_LOG(
        "Bad ELF magic at {} in {}",
        reinterpret_cast<void*>(info->dlpi_addr),
        info->dlpi_name);
    return 0;
  }
  int fd = ::open(info->dlpi_name, O_RDONLY);
  if (fd < 0) {
    JIT_LOG("Failed opening {}: {}", info->dlpi_name, ::strerror(errno));
    return 0;
  }
  SCOPE_EXIT(::close(fd));
  struct stat statbuf;
  if (::fstat(fd, &statbuf) < 0) {
    JIT_LOG("Failed stat: {}", ::strerror(errno));
    return 0;
  }
  void* mapping =
      ::mmap(nullptr, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapping == MAP_FAILED) {
    JIT_LOG("Failed mmap: {}", ::strerror(errno));
    return 0;
  }
  SCOPE_EXIT(::munmap(mapping, statbuf.st_size));
  uint8_t* elf_obj = static_cast<uint8_t*>(mapping);
  auto elf_hdr = reinterpret_cast<ElfW(Ehdr)*>(elf_obj);
  if (elf_hdr->e_shoff == 0) {
    JIT_LOG("No section header table in {}", info->dlpi_name);
    return 0;
  }
  // Get the number of entries in the section header table (`e_shnum`). If this
  // value is zero, the number of entries is in the `sh_size` field of the
  // first entry in the section header table.
  auto sec_hdrs = reinterpret_cast<ElfW(Shdr)*>(elf_obj + elf_hdr->e_shoff);
  uint64_t num_sec_hdrs = elf_hdr->e_shnum;
  if (num_sec_hdrs == 0) {
    num_sec_hdrs = sec_hdrs[0].sh_size;
  }
  // Iterate through the symbol tables and search for symbols with the given
  // function address.
  for (uint64_t i = 0; i < num_sec_hdrs; i++) {
    ElfW(Shdr)* sec_hdr = &sec_hdrs[i];
    if (sec_hdr->sh_type != SHT_SYMTAB) {
      continue;
    }
    ElfW(Shdr)* sym_tab_hdr = &sec_hdrs[i];
    ElfW(Shdr)* str_tab_hdr = &sec_hdrs[sym_tab_hdr->sh_link];
    uint64_t nsyms = sym_tab_hdr->sh_size / sym_tab_hdr->sh_entsize;
    auto symtab_start =
        reinterpret_cast<ElfW(Sym)*>(elf_obj + sym_tab_hdr->sh_offset);
    ElfW(Sym)* symtab_end = symtab_start + nsyms;
    auto strtab =
        reinterpret_cast<const char*>(elf_obj + str_tab_hdr->sh_offset);
    for (ElfW(Sym)* sym = symtab_start; sym != symtab_end; sym++) {
      if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC) {
        // We only care about symbols associated with executable code
        continue;
      }
      auto addr = reinterpret_cast<void*>(info->dlpi_addr + sym->st_value);
      auto result = reinterpret_cast<SymbolResult*>(data);
      if (addr == result->func) {
        // Convert the result to std::string here; the lifetime of the strtab
        // ends at the end of this function.
        result->name = &strtab[sym->st_name];
        return 1;
      }
    }
  }
  return 0;
}

#endif

} // namespace

Symbolizer::Symbolizer(const char* exe_path) {
  try {
    file_.open(exe_path);
  } catch (const std::exception& exn) {
    JIT_LOG("{}", exn.what());
    return;
  }

#ifdef ENABLE_SYMBOLIZER
  const std::byte* exe = file_.data().data();

  auto elf = reinterpret_cast<const ElfW(Ehdr)*>(exe);
  auto shdr = reinterpret_cast<const ElfW(Shdr)*>(exe + elf->e_shoff);
  auto str =
      reinterpret_cast<const char*>(exe + shdr[elf->e_shstrndx].sh_offset);
  for (int i = 0; i < elf->e_shnum; i++) {
    if (shdr[i].sh_size) {
      if (std::strcmp(&str[shdr[i].sh_name], ".symtab") == 0) {
        symtab_ = &shdr[i];
      } else if (std::strcmp(&str[shdr[i].sh_name], ".strtab") == 0) {
        strtab_ = &shdr[i];
      }
    }
  }
  if (symtab_ == nullptr) {
    JIT_LOG("could not find symtab");
    deinit();
    return;
  }
  if (strtab_ == nullptr) {
    JIT_LOG("could not find strtab");
    deinit();
    return;
  }
#endif
}

std::optional<std::string_view> Symbolizer::cache(
    const void* func,
    std::optional<std::string> name) {
  auto pair = cache_.emplace(func, std::move(name));
  JIT_CHECK(pair.second, "{} already exists in cache", func);
  return pair.first->second;
}

std::optional<std::string_view> Symbolizer::symbolize(const void* func) {
#ifdef ENABLE_SYMBOLIZER
  // Try the cache first. We might have looked it up before.
  auto cached = cache_.find(func);
  if (cached != cache_.end()) {
    return cached->second;
  }
  // Then try dladdr. It might be able to find the symbol.
  Dl_info info;
  if (::dladdr(func, &info) != 0 && info.dli_sname != nullptr) {
    return cache(func, info.dli_sname);
  }
  if (!isInitialized()) {
    return std::nullopt;
  }
  // Fall back to reading our own ELF header.
  const std::byte* exe = file_.data().data();

  auto symtab = reinterpret_cast<const ElfW(Shdr)*>(symtab_);
  auto strtab = reinterpret_cast<const ElfW(Shdr)*>(strtab_);

  auto sym = reinterpret_cast<const ElfW(Sym)*>(exe + symtab->sh_offset);
  auto str = reinterpret_cast<const char*>(exe + strtab->sh_offset);
  for (size_t i = 0; i < symtab->sh_size / sizeof(ElfW(Sym)); i++) {
    if (reinterpret_cast<void*>(sym[i].st_value) == func) {
      return cache(func, str + sym[i].st_name);
    }
  }
  // Fall back to reading dynamic symbols.
  SymbolResult result = {func, std::nullopt};
  int found = ::dl_iterate_phdr(findSymbolIn, &result);
  JIT_CHECK(
      (found > 0) == result.name.has_value(),
      "result.name should match return value of dl_iterate_phdr");

  // We intentionally cache negative lookup results to help performance. This
  // means we'll miss out on addresses that are mapped to a symbol after our
  // first attempt at symbolizing them.
  return cache(func, result.name);
#else
  return std::nullopt;
#endif
}

void Symbolizer::deinit() {
  try {
    file_.close();
  } catch (const std::exception& exn) {
    JIT_LOG("{}", exn.what());
  }

  symtab_ = nullptr;
  strtab_ = nullptr;
  cache_.clear();
}

std::optional<std::string> demangle(const std::string& mangled_name) {
  int status;
  char* demangled_name =
      abi::__cxa_demangle(mangled_name.c_str(), nullptr, nullptr, &status);
  if (demangled_name == nullptr) {
    if (status == -1) {
      JIT_DLOG("Could not allocate memory for demangled name");
    } else if (status == -2) {
      JIT_DLOG("Mangled name '{}' is not valid", mangled_name);
      // Couldn't demangle. Oh well. Probably better to have some name than
      // none at all.
      return mangled_name;
    } else if (status == -3) {
      JIT_DLOG("Invalid input to __cxa_demangle");
    }
    return std::nullopt;
  }
  std::string result{demangled_name};
  std::free(demangled_name);
  return result;
}

std::optional<std::string> symbolize(const void* func) {
#ifdef ENABLE_SYMBOLIZER
  if (!getConfig().log.symbolize_funcs) {
    return std::nullopt;
  }
  auto module_state = cinderx::getModuleState();
  if (module_state == nullptr || module_state->symbolizer() == nullptr) {
    return std::nullopt;
  }

  std::optional<std::string_view> mangled_name =
      module_state->symbolizer()->symbolize(func);
  if (!mangled_name.has_value()) {
    return std::nullopt;
  }
  return demangle(std::string{*mangled_name});
#else
  return std::nullopt;
#endif
}

} // namespace jit

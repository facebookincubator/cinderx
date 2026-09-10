// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/perf_jitdump.h"

#include "cinderx/python.h"

#include "internal/pycore_ceval.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/compilation_lock.h"
#include "cinderx/Jit/config.h"

#ifndef WIN32

#include <fmt/format.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef __x86_64__

// Needed for __rdtsc on GCC.
#include <x86intrin.h>

// Use the cheaper rdtsc by default. If you disable this for some reason, or
// run on a non-x86_64 architecture, you need to add '-k1' to your 'perf
// record' command.
#define PERF_USE_RDTSC

#endif

// From elf.h, intentionally not included here to avoid having it as a
// dependency.
#define EM_X86_64 62
#define EM_AARCH64 183

#endif

namespace cinderx::jit::perf {

#ifndef WIN32

namespace {

// Size used for the JIT dump mmap() call.  Just needs to be consistent across
// mmap()/munmap(), perf uses the mmap event rather than the mapping itself.
constexpr size_t kJitdumpMmapSize = 1;

// An entry in a perf map file.  Consists of a memory range and a symbol name.
struct PerfMapEntry {
  const void* addr{};
  uint32_t size{};
  const char* name{};
};

struct FileInfo {
  std::string filename;
  std::string filename_format;
  std::FILE* file{nullptr};
};

FileInfo g_pid_map;

FileInfo g_jitdump_file;
void* g_jitdump_mmap_addr = nullptr;

// C++-friendly wrapper around strerror_r().
std::string string_error(int errnum) {
  char buf[1024];
  // There's two forms of strerror_r(), one that returns a string and one that
  // returns an int.
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  return strerror_r(errnum, buf, sizeof(buf));
#else
  strerror_r(errnum, buf, sizeof(buf));
  return std::string{buf};
#endif
}

class FileLock {
 public:
  FileLock(std::FILE* file, bool exclusive) : fd_{fileno(file)} {
    auto operation = exclusive ? LOCK_EX : LOCK_SH;
    while (true) {
      auto ret = ::flock(fd_, operation);
      if (ret == 0) {
        return;
      }
      if (ret == -1 && errno == EINTR) {
        continue;
      }
      JIT_ABORT(
          "flock({}, {}) failed: {}", fd_, operation, string_error(errno));
    }
  }

  ~FileLock() {
    auto ret = ::flock(fd_, LOCK_UN);
    JIT_CHECK(
        ret == 0, "flock({}, LOCK_UN) failed: {}", fd_, string_error(errno));
  }

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

 private:
  int fd_;
};

class ExclusiveFileLock : public FileLock {
 public:
  explicit ExclusiveFileLock(std::FILE* file) : FileLock{file, true} {}
};

// This file writes out perf jitdump files, to be used by 'perf inject' and
// 'perf report'. The format is documented here:
// https://raw.githubusercontent.com/torvalds/linux/master/tools/perf/Documentation/jitdump-specification.txt.

enum Flags {
  JITDUMP_FLAGS_ARCH_TIMESTAMP = 1,
};

struct FileHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t total_size;
  uint32_t elf_mach;
  uint32_t pad1;
  uint32_t pid;
  uint64_t timestamp;
  uint64_t flags;
};

enum RecordType {
  JIT_CODE_LOAD = 0,
  JIT_CODE_MOVE = 1,
  JIT_CODE_DEBUG_INFO = 2,
  JIT_CODE_CLOSE = 3,
  JIT_CODE_UNWINDING_INFO = 4,
};

struct RecordHeader {
  uint32_t type;
  uint32_t total_size;
  uint64_t timestamp;
};

struct CodeLoadRecord : RecordHeader {
  uint32_t pid;
  uint32_t tid;
  uint64_t vma;
  uint64_t code_addr;
  uint64_t code_size;
  uint64_t code_index;
};

// The gettid() syscall doesn't have a C wrapper.
pid_t gettid() {
  return syscall(SYS_gettid);
}

// Get a timestamp for the current event.
uint64_t getTimestamp() {
#ifdef PERF_USE_RDTSC
  return __rdtsc();
#else
  static const uint64_t kNanosPerSecond = 1000000000;
  struct timespec tm;
  int ret = clock_gettime(CLOCK_MONOTONIC, &tm);
  if (ret < 0) {
    return -1;
  }
  return tm.tv_sec * kNanosPerSecond + tm.tv_nsec;
#endif
}

void* mmapJitDump(int fd) {
  return mmap(nullptr, kJitdumpMmapSize, PROT_EXEC, MAP_PRIVATE, fd, 0);
}

bool isPerfMapFilename(std::string_view s) {
  // TODO: Consider also checking for only decimal chars in between.
  return s.starts_with("/tmp/perf-") && s.ends_with(".map");
}

FileInfo openFileInfo(std::string filename_format) {
  auto filename = fmt::format(fmt::runtime(filename_format), getpid());
  auto file = std::fopen(filename.c_str(), "w+");
  if (file == nullptr) {
    JIT_LOG("Couldn't open {} for writing ({})", filename, string_error(errno));
    return {};
  }
  return {filename, filename_format, file};
}

FileInfo openPidMap() {
  if (!getConfig().perf_map.enabled) {
    return {};
  }

  FileInfo perf_map = openFileInfo("/tmp/perf-{}.map");
  JIT_DLOG("Opened JIT perf-map file: {}", perf_map.filename);
  return perf_map;
}

// If enabled, open the jitdump file, and write out its header.
FileInfo openJitdumpFile() {
  auto const& jit_dump_dir = getConfig().perf_map.jit_dump_dir;
  if (jit_dump_dir.empty()) {
    return {};
  }

  auto info = openFileInfo(fmt::format("{}/jit-{{}}.dump", jit_dump_dir));
  if (info.file == nullptr) {
    return {};
  }
  auto fd = fileno(info.file);

  // mmap() the jitdump file so perf inject can find it.
  g_jitdump_mmap_addr = mmapJitDump(fd);
  JIT_CHECK(
      g_jitdump_mmap_addr != MAP_FAILED,
      "Marker mmap of jitdump file failed: {}",
      string_error(errno));

  // Write out the file header.
  FileHeader header;
  header.magic = 0x4a695444;
  header.version = 1;
  header.total_size = sizeof(header);
#ifdef __x86_64__
  header.elf_mach = EM_X86_64;
#elif defined(__aarch64__)
  header.elf_mach = EM_AARCH64;
#else
#error Please provide the ELF e_machine value for your architecture.
#endif
  header.pad1 = 0;
  header.pid = getpid();
  header.timestamp = getTimestamp();
#ifdef PERF_USE_RDTSC
  header.flags = JITDUMP_FLAGS_ARCH_TIMESTAMP;
#else
  header.flags = 0;
#endif

  std::fwrite(&header, sizeof(header), 1, info.file);
  std::fflush(info.file);
  return info;
}

void initFiles() {
  static bool inited = false;
  if (inited) {
    return;
  }
  g_pid_map = openPidMap();
  g_jitdump_file = openJitdumpFile();
  inited = true;
}

// Parses a JIT entry and returns a tuple containing the
// code address, code size, and entry name. An example of an entry is:
// 7fa873c00148 360 __CINDER_JIT:__main__:foo2
PerfMapEntry parsePerfMapEntry(std::string_view entry) {
  size_t space_pos_1 = entry.find(' ');

  // Extract the hexadecimal code address
  const char* code_addr_str = entry.substr(0, space_pos_1).data();
  unsigned long long code_addr_val = 0;
  std::from_chars(
      code_addr_str, code_addr_str + space_pos_1, code_addr_val, 16);
  const void* code_addr = reinterpret_cast<const void*>(code_addr_val);

  // Find the second space character
  size_t space_pos_2 = entry.find(' ', space_pos_1 + 1);

  // Extract the hexadecimal code size
  const char* code_size_str = entry.substr(space_pos_1 + 1, space_pos_2).data();
  uint32_t code_size = 0;
  std::from_chars(
      code_size_str,
      code_size_str + (space_pos_2 - space_pos_1 - 1),
      code_size,
      16);

  // Extract the entry name
  const char* entry_name = entry.substr(space_pos_2 + 1).data();

  return PerfMapEntry{code_addr, code_size, entry_name};
}

// Copy the contents of from_name to to_name. Returns a std::FILE* at the end
// of to_name on success, or nullptr on failure.
std::FILE* copyFile(const std::string& from_name, const std::string& to_name) {
  auto from = std::fopen(from_name.c_str(), "r");
  if (from == nullptr) {
    JIT_LOG(
        "Couldn't open {} for reading ({})", from_name, string_error(errno));
    return nullptr;
  }
  auto to = std::fopen(to_name.c_str(), "w+");
  if (to == nullptr) {
    std::fclose(from);
    JIT_LOG("Couldn't open {} for writing ({})", to_name, string_error(errno));
    return nullptr;
  }

  char buf[4096];
  while (true) {
    auto bytes_read = std::fread(&buf, 1, sizeof(buf), from);
    auto bytes_written = std::fwrite(&buf, 1, bytes_read, to);
    if (bytes_read < sizeof(buf) && std::feof(from)) {
      // We finished successfully.
      std::fflush(to);
      std::fclose(from);
      return to;
    }
    if (bytes_read == 0 || bytes_written < bytes_read) {
      JIT_LOG("Error copying {} to {}", from_name, to_name);
      std::fclose(from);
      std::fclose(to);
      return nullptr;
    }
  }
}

// Copy the contents of another perf map file to our process's perf map file.
// Can optionally filter out any entries that aren't from the CinderX JIT.
bool copyPerfMapEntries(const std::string& filename, bool filter_cinder) {
  std::ifstream file{filename};
  if (!file) {
    JIT_DLOG(
        "Couldn't open perf map file {} for reading ({})",
        filename,
        string_error(errno));
    return false;
  }

  std::string line;
  while (std::getline(file, line)) {
    if (filter_cinder && line.find("__CINDER_") == std::string::npos) {
      continue;
    }

    auto entry = parsePerfMapEntry(line);
    int res = PyUnstable_WritePerfMapEntry(entry.addr, entry.size, entry.name);
    if (res != 0) {
      JIT_DLOG(
          "Failed to copy perf map line from {} ({})",
          filename,
          string_error(errno));
    }
  }

  if (!file.eof()) {
    JIT_DLOG("Failed to copy all perf map entries from {}", filename);
    return false;
  }

  return true;
}

bool isPerfTrampolineActive() {
  PyThreadState* tstate = PyThreadState_GET();
  return tstate->interp->eval_frame &&
      tstate->interp->eval_frame != _PyEval_EvalFrameDefault;
}

// Copy the perf pid map from the parent process into a new file for this child
// process.
void copyFileInfo(FileInfo& info) {
  if (info.file == nullptr) {
    return;
  }

  std::fclose(info.file);
  auto parent_filename = info.filename;
  auto child_filename =
      fmt::format(fmt::runtime(info.filename_format), getpid());
  info = {};

  if (isPerfMapFilename(parent_filename)) {
    if (isPreforkCompilationEnabled()) {
      JIT_LOG(
          "File {} has already been copied to {} by the perf trampoline, "
          "skipping copy.",
          parent_filename,
          child_filename);
      return;
    } else if (isPerfTrampolineActive()) {
      if (!copyPerfMapEntries(parent_filename, true /* filter_cinder */)) {
        JIT_LOG(
            "Failed to copy JIT entries from {} to {}",
            parent_filename,
            child_filename);
      }
      return;
    } else if (isJitUsable()) {
      // The JIT is still enabled: copy the file to allow for more compilation
      // in this process.
      if (!copyPerfMapEntries(parent_filename, false /* filter_cinder */)) {
        JIT_LOG(
            "Failed to copy perf map file from {} to {}",
            parent_filename,
            child_filename);
      }
      return;
    }

    // Fall through to the hard link case otherwise.
  }

  unlink(child_filename.c_str());
  if (isJitUsable()) {
    // The JIT is still enabled: copy the file to allow for more compilation
    // in this process.
    if (auto new_pid_map = copyFile(parent_filename, child_filename)) {
      info.filename = child_filename;
      info.file = new_pid_map;
    }
  } else {
    // The JIT has been disabled: hard link the file to save disk space. Don't
    // open it in this process, to avoid messing with the parent's file.
    if (::link(parent_filename.c_str(), child_filename.c_str()) != 0) {
      JIT_LOG(
          "Failed to link {} to {}: {}",
          child_filename,
          parent_filename,
          string_error(errno));
    } else {
      // Poke the file's atime to keep tmpwatch at bay.
      std::FILE* file = std::fopen(parent_filename.c_str(), "r");
      if (file != nullptr) {
        std::fclose(file);
      }
    }
  }
}

void copyParentPidMap() {
  copyFileInfo(g_pid_map);
}

void copyJitdumpFile() {
  auto ret = munmap(g_jitdump_mmap_addr, kJitdumpMmapSize);
  JIT_CHECK(
      ret == 0, "Marker unmap of jitdump file failed: {}", string_error(errno));

  copyFileInfo(g_jitdump_file);
  if (g_jitdump_file.file == nullptr) {
    return;
  }

  g_jitdump_mmap_addr = mmapJitDump(fileno(g_jitdump_file.file));
}

} // namespace

#endif

bool isPreforkCompilationEnabled() {
  return kOS != OS::kWindows && getConfig().compile_perf_trampoline_prefork;
}

void registerFunction(
    const std::vector<std::pair<void*, std::size_t>>& code_sections,
    std::string_view name,
    std::string_view prefix) {
#ifndef WIN32
  JITCompilationLock lock;

  initFiles();

  for (auto& section_and_size : code_sections) {
    void* code = section_and_size.first;
    std::size_t size = section_and_size.second;
    auto jit_entry = fmt::format("{}:{}", prefix, name);
    PyUnstable_WritePerfMapEntry(
        static_cast<const void*>(code), size, jit_entry.c_str());
  }

  if (auto file = g_jitdump_file.file) {
    // Make sure no parent or child process writes concurrently.
    ExclusiveFileLock write_lock(file);

    static uint64_t code_index = 0;
    for (auto& section_and_size : code_sections) {
      auto const prefixed_name = fmt::format("{}:{}", prefix, name);

      void* code = section_and_size.first;
      std::size_t size = section_and_size.second;
      CodeLoadRecord record;
      record.type = JIT_CODE_LOAD;

      record.total_size = sizeof(record) + prefixed_name.size() + 1 + size;
      record.timestamp = getTimestamp();
      record.pid = getpid();
      record.tid = gettid();
      record.vma = record.code_addr = reinterpret_cast<uint64_t>(code);
      record.code_size = size;
      record.code_index = code_index++;

      std::fwrite(&record, sizeof(record), 1, file);
      std::fwrite(prefixed_name.data(), 1, prefixed_name.size() + 1, file);
      std::fwrite(code, 1, size, file);
    }
    std::fflush(file);
  }
#endif
}

void afterForkChild() {
#ifndef WIN32
  // Make sure the parent processes map is closed before copying into it,
  // otherwise init is a nop.
  PyUnstable_PerfMapState_Fini();
  copyParentPidMap();
  copyJitdumpFile();
#endif
}

} // namespace cinderx::jit::perf

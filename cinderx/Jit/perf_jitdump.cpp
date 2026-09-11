// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/perf_jitdump.h"

#include "cinderx/python.h"

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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

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

struct JitDumpFile {
  std::string path;
  std::FILE* handle{nullptr};
  void* mmap_addr{nullptr};
};

// The perf map file handle is stored inside of CPython, the only state CinderX
// stores is the file path.
std::string s_perf_map_path;

JitDumpFile s_jit_dump_file;

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

std::string perfMapPath() {
  return fmt::format("/tmp/perf-{}.map", getpid());
}

std::string jitDumpPath() {
  return fmt::format(
      "{}/jit-{}.dump", getConfig().perf_map.jit_dump_dir, getpid());
}

// If enabled, open the jitdump file, and write out its header.
JitDumpFile openJitdumpFile() {
  JitDumpFile jit_dump_file;

  jit_dump_file.path = jitDumpPath();
  jit_dump_file.handle = std::fopen(jit_dump_file.path.c_str(), "w+");
  if (jit_dump_file.handle == nullptr) {
    JIT_DLOG(
        "Failed to open JIT dump file {}, {}",
        jit_dump_file.path,
        string_error(errno));
    return {};
  }
  auto fd = fileno(jit_dump_file.handle);

  // mmap() the jitdump file so perf inject can find it.
  jit_dump_file.mmap_addr = mmapJitDump(fd);
  if (jit_dump_file.mmap_addr == MAP_FAILED) {
    JIT_DLOG(
        "Failed to mmap jit dump file {}, {}",
        jit_dump_file.path,
        string_error(errno));
    std::fclose(jit_dump_file.handle);
    return {};
  }

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

  std::fwrite(&header, sizeof(header), 1, jit_dump_file.handle);
  std::fflush(jit_dump_file.handle);
  return jit_dump_file;
}

void initFiles() {
  static bool inited = false;
  if (inited) {
    return;
  }

  if (getConfig().perf_map.enabled) {
    auto perf_map_path = perfMapPath();
    // CPython will open the file in append mode.  We want to empty it out first
    // so that we don't make use of stale entries from previous processes.
    //
    // This runs the risk of blowing away entries that are added by this process
    // before CinderX is initialized, but we don't have a good solution for that
    // today.
    std::error_code ignored_ec;
    fs::remove(perf_map_path, ignored_ec);

    int result = PyUnstable_PerfMapState_Init();
    if (result != 0) {
      JIT_DLOG(
          "Failed to initialize perf map file (cpython: {}) (errno: {})",
          result,
          string_error(errno));
    } else {
      s_perf_map_path = std::move(perf_map_path);
      JIT_DLOG("Opened JIT perf-map file: {}", s_perf_map_path);
    }
  }

  if (!getConfig().perf_map.jit_dump_dir.empty()) {
    s_jit_dump_file = openJitdumpFile();
  }

  inited = true;
}

std::error_code copyFile(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  return ec;
}

void copyPerfMap() {
  if (s_perf_map_path.empty()) {
    return;
  }

  std::string old_path = std::move(s_perf_map_path);
  std::string new_path = perfMapPath();

  // Check if CPython has copied over the perf map file over for us.
  if (isPreforkCompilationEnabled()) {
    s_perf_map_path = std::move(new_path);
    return;
  }

  std::error_code ec = copyFile(old_path, new_path);
  if (ec) {
    JIT_DLOG(
        "Failed to copy perf map file from {} to {}, {}",
        old_path,
        new_path,
        ec.message());
    return;
  }

  int result = PyUnstable_PerfMapState_Init();
  if (result != 0) {
    JIT_DLOG(
        "Failed to initialize perf map file (cpython: {}) (errno: {})",
        result,
        string_error(errno));
    return;
  }

  s_perf_map_path = std::move(new_path);
}

void copyJitDumpFile() {
  if (s_jit_dump_file.path.empty()) {
    return;
  }

  std::string old_path = std::move(s_jit_dump_file.path);
  std::string new_path = jitDumpPath();

  std::fclose(s_jit_dump_file.handle);
  s_jit_dump_file.handle = nullptr;

  auto ret = munmap(s_jit_dump_file.mmap_addr, kJitdumpMmapSize);
  if (ret != 0) {
    JIT_DLOG("Marker unmap of jitdump file failed: {}", string_error(errno));
  }
  s_jit_dump_file.mmap_addr = nullptr;

  std::error_code ec = copyFile(old_path, new_path);
  if (ec) {
    JIT_DLOG(
        "Failed to copy JIT dump file from {} to {}, {}",
        old_path,
        new_path,
        ec.message());
    return;
  }

  std::FILE* handle = std::fopen(new_path.c_str(), "a+");
  if (handle == nullptr) {
    JIT_DLOG(
        "Failed to open JIT dump file in {} after copying it, {}",
        new_path,
        string_error(errno));
    return;
  }

  auto mmap_addr = mmapJitDump(fileno(handle));
  if (mmap_addr == MAP_FAILED) {
    JIT_DLOG(
        "Failed to mmap jit dump file {} after fork, {}",
        new_path,
        string_error(errno));
    std::fclose(handle);
    return;
  }

  s_jit_dump_file.path = new_path;
  s_jit_dump_file.handle = handle;
  s_jit_dump_file.mmap_addr = mmap_addr;
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

  if (std::FILE* file = s_jit_dump_file.handle) {
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
  // Make sure the perf map file handle we inherited from the parent is closed.
  PyUnstable_PerfMapState_Fini();

  copyPerfMap();
  copyJitDumpFile();
#endif
}

} // namespace cinderx::jit::perf

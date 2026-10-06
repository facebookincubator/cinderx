// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/perf_jitdump.h"

#include "cinderx/python.h"

#include "cinderx/Common/define.h"
#include "cinderx/Common/log.h"

#ifndef _WIN32

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

#ifdef CINDER_X86_64

// Needed for __rdtsc on GCC.
#include <x86intrin.h>

// Use the cheaper rdtsc by default. If you disable this for some reason, or
// run on a non-x86_64 architecture, you need to add '-k1' to your 'perf
// record' command.
#define PERF_USE_RDTSC

#endif

// From elf.h, intentionally not included here to avoid having it as a
// dependency.
constexpr int EM_X86_64 = 62;
constexpr int EM_AARCH64 = 183;

#endif

namespace cinderx::jit::perf {

namespace {

#ifndef _WIN32

// Size used for the JIT dump mmap() call.  Just needs to be consistent across
// mmap()/munmap(), perf uses the mmap event rather than the mapping itself.
constexpr size_t kJitdumpMmapSize = 1;

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

// Following definitions are for the JIT dump file format.

constexpr int JITDUMP_FLAGS_ARCH_TIMESTAMP = 1;

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

int munmapJitDump(void* addr) {
  return munmap(addr, kJitdumpMmapSize);
}

std::string perfMapPath() {
  return fmt::format("/tmp/perf-{}.map", getpid());
}

std::string jitDumpPath(std::string_view dir) {
  return fmt::format("{}/jit-{}.dump", dir, getpid());
}

std::error_code copyFile(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  return ec;
}

struct PerfMapWriter : Writer {
  explicit PerfMapWriter(bool prefork_enabled)
      : prefork_enabled_{prefork_enabled} {}

  ~PerfMapWriter() override {
    // CPython will take care of closing the perf map state.
  }

  PerfMapWriter(const PerfMapWriter& o) = delete;
  PerfMapWriter& operator=(const PerfMapWriter& rhs) = delete;

  PerfMapWriter(PerfMapWriter&& o) = delete;
  PerfMapWriter& operator=(PerfMapWriter&& rhs) = delete;

  void writeEntry(const Entry& entry) override {
    if (!init_) {
      init_ = true;
      init();
    }

    if (path_.empty()) {
      return;
    }

    // Avoiding integer conversion warnings.
    auto entry_size = static_cast<unsigned>(entry.size);

    int result =
        PyUnstable_WritePerfMapEntry(entry.addr, entry_size, entry.name);
    if (result != 0) {
      JIT_DLOG(
          "Failed to write perf map entry for {} to file {} (cpython: {}) "
          "(errno: {})",
          entry.name,
          path_,
          result,
          string_error(errno));
    }
  }

  void flush() override {
    // Nothing to do.
  }

  void afterForkChild() override {
    if (path_.empty()) {
      return;
    }

    std::string old_path = std::move(path_);
    std::string new_path = perfMapPath();

    PyUnstable_PerfMapState_Fini();

    // Check if CPython has already copied over the perf map file over for us.
    if (prefork_enabled_) {
      path_ = std::move(new_path);
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

    path_ = std::move(new_path);
  }

 private:
  void init() {
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
      return;
    }

    path_ = std::move(perf_map_path);
    JIT_DLOG("Opened JIT perf-map file: {}", path_);
  }

  // The perf map file handle is stored inside of CPython, the only state
  // CinderX stores is the file path.
  std::string path_;
  bool init_{false};
  bool prefork_enabled_{false};
};

struct JitDumpWriter : Writer {
  explicit JitDumpWriter(std::string_view jit_dump_dir) : dir_{jit_dump_dir} {}

  ~JitDumpWriter() override {
    if (handle_ == nullptr) {
      return;
    }
    std::fclose(handle_);
    if (mmap_addr_ != nullptr && munmapJitDump(mmap_addr_) != 0) {
      JIT_DLOG("Marker unmap of jitdump file failed: {}", string_error(errno));
    }
  }

  JitDumpWriter(const JitDumpWriter& o) = delete;
  JitDumpWriter& operator=(const JitDumpWriter& rhs) = delete;

  JitDumpWriter(JitDumpWriter&& o) = delete;
  JitDumpWriter& operator=(JitDumpWriter&& rhs) = delete;

  void writeEntry(const Entry& entry) override {
    if (!init_) {
      init_ = true;
      init();
    }

    if (handle_ == nullptr) {
      return;
    }

    size_t name_len = std::strlen(entry.name);
    CodeLoadRecord record{};
    record.type = JIT_CODE_LOAD;
    // Total size is 32-bit even though it's defined by the code size which is
    // 64-bit.
    auto total_size =
        static_cast<uint32_t>(sizeof(record) + name_len + 1 + entry.size);
    record.total_size = total_size;
    record.timestamp = getTimestamp();
    record.pid = getpid();
    record.tid = gettid();
    record.vma = record.code_addr = reinterpret_cast<uint64_t>(entry.addr);
    record.code_size = entry.size;
    record.code_index = code_index_++;

    std::fwrite(&record, sizeof(record), 1, handle_);
    std::fwrite(entry.name, 1, name_len + 1, handle_);
    std::fwrite(entry.addr, 1, entry.size, handle_);
  }

  void flush() override {
    if (handle_ == nullptr) {
      return;
    }
    if (std::fflush(handle_) != 0) {
      JIT_DLOG(
          "Failed to flush jit dump file {}, {}", path_, string_error(errno));
    }
  }

  void afterForkChild() override {
    if (handle_ == nullptr) {
      return;
    }

    std::string old_path = std::move(path_);
    std::string new_path = jitDumpPath(dir_);

    // Destroy previous state, ignore failures.
    std::fclose(handle_);
    if (mmap_addr_ != nullptr) {
      munmapJitDump(mmap_addr_);
    }
    handle_ = nullptr;
    mmap_addr_ = nullptr;

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

    // The mmap() is needed for `perf inject`, which is Linux-only.  It will
    // fail on macOS.
    if (kOS == OS::kLinux) {
      auto mmap_addr = mmapJitDump(fileno(handle));
      if (mmap_addr == MAP_FAILED) {
        JIT_DLOG(
            "Failed to mmap jit dump file {} after fork, {}",
            new_path,
            string_error(errno));
        std::fclose(handle);
        return;
      }
      mmap_addr_ = mmap_addr;
    }

    path_ = std::move(new_path);
    handle_ = handle;
  }

 private:
  void init() {
    // First create the file header.  Done first to handle the architecture
    // check before opening the file.
    FileHeader header{};
    header.magic = 0x4a695444;
    header.version = 1;
    header.total_size = sizeof(header);
    if constexpr (kBuildArch == Arch::kX86_64) {
      header.elf_mach = EM_X86_64;
    } else if constexpr (kBuildArch == Arch::kAarch64) {
      header.elf_mach = EM_AARCH64;
    } else {
      JIT_THROW("Please provide the ELF e_machine value for your architecture");
    }
    header.pad1 = 0;
    header.pid = getpid();
    header.timestamp = getTimestamp();
#ifdef PERF_USE_RDTSC
    header.flags = JITDUMP_FLAGS_ARCH_TIMESTAMP;
#else
    header.flags = 0;
#endif

    // Open the JIT dump file.
    auto path = jitDumpPath(dir_);
    auto handle = std::fopen(path.c_str(), "w+");
    if (handle == nullptr) {
      JIT_DLOG(
          "Failed to open JIT dump file {}, {}", path, string_error(errno));
      return;
    }
    [[maybe_unused]] int fd = fileno(handle);

    // mmap() the jitdump file so perf inject can find it.  This is Linux-only
    // and fails on macOS, where the dump is still usable without the marker
    // mapping.
    if (kOS == OS::kLinux) {
      auto mmap_addr = mmapJitDump(fd);
      if (mmap_addr == MAP_FAILED) {
        JIT_DLOG(
            "Failed to mmap jit dump file {}, {}", path, string_error(errno));
        std::fclose(handle);
        return;
      }
      mmap_addr_ = mmap_addr;
    }

    path_ = std::move(path);
    handle_ = handle;

    std::fwrite(&header, sizeof(header), 1, handle_);
  }

  std::string dir_;
  std::string path_;
  // Uses std::FILE* as the mmap() call needs the raw file descriptor, which
  // std::ostream doesn't expose.
  std::FILE* handle_{};
  void* mmap_addr_{};
  uint64_t code_index_{0};
  bool init_{false};
};

#else // !define(_WIN32)

struct NopWriter : Writer {
  ~NopWriter() override = default;

  void writeEntry(const Entry&) override {}
  void flush() override {}
  void afterForkChild() override {}
};

#endif

} // namespace

std::unique_ptr<Writer> makePerfMapWriter(
    [[maybe_unused]] bool prefork_enabled) {
#ifndef _WIN32
  return std::make_unique<PerfMapWriter>(prefork_enabled);
#else
  return std::make_unique<NopWriter>();
#endif
}

std::unique_ptr<Writer> makeJitDumpWriter(
    [[maybe_unused]] std::string_view jit_dump_dir) {
#ifndef _WIN32
  return std::make_unique<JitDumpWriter>(jit_dump_dir);
#else
  return std::make_unique<NopWriter>();
#endif
}

} // namespace cinderx::jit::perf

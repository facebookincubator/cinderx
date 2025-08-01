// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/mmap_file.h"

#include "cinderx/Common/util.h"

#include <fcntl.h>
#include <fmt/format.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <system_error>

namespace jit {

std::string strerrorSafe(int errnum) {
  return std::system_category().message(errnum);
}

MmapFile::~MmapFile() {
  try {
    close();
  } catch (const std::exception& exn) {
    JIT_LOG("{}", exn.what());
  }
}

void MmapFile::open(const char* filename) {
  if (isOpen()) {
    throw std::logic_error{fmt::format(
        "Trying to mmap {} on top of an existing file object", filename)};
  }

  int fd = ::open(filename, O_RDONLY);
  if (fd == -1) {
    throw std::runtime_error{
        fmt::format("Could not open {}: {}", filename, strerrorSafe(errno))};
  }

  // Close the file descriptor. We don't need to keep it around for the mapping
  // to be valid and if we leave it lying around then some CPython tests fail
  // because they rely on specific file descriptor numbers.
  SCOPE_EXIT(::close(fd));

  struct stat statbuf;
  int stat_result = ::fstat(fd, &statbuf);
  if (stat_result == -1) {
    throw std::runtime_error{
        fmt::format("Could not stat {}: {}", filename, strerrorSafe(errno))};
  }
  off_t signed_size = statbuf.st_size;
  if (signed_size < 0) {
    throw std::runtime_error{
        fmt::format("Stat'd a size of {} for file {}", signed_size, filename)};
  }

  auto size = static_cast<size_t>(signed_size);
  void* data = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);

  if (data == MAP_FAILED) {
    throw std::runtime_error{fmt::format(
        "Failed to mmap file {}: {}", filename, strerrorSafe(errno))};
  }

  data_ = reinterpret_cast<const std::byte*>(data);
  size_ = size;
}

void MmapFile::close() {
  if (!isOpen()) {
    return;
  }

  int result =
      ::munmap(const_cast<void*>(reinterpret_cast<const void*>(data_)), size_);
  if (result != 0) {
    throw std::runtime_error{
        fmt::format("Failed to munmap file: {}", strerrorSafe(errno))};
  }

  data_ = nullptr;
  size_ = 0;
}

bool MmapFile::isOpen() const {
  return data_ != nullptr;
}

std::span<const std::byte> MmapFile::data() {
  return std::span{data_, size_};
}

} // namespace jit

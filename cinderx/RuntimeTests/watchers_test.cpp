// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/define.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Common/watchers.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_state.h"

#include <cstring>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace cinderx {

using WatchersTest = RuntimeTest;

namespace {

WatcherState& watcherState() {
  return getModuleState()->watcher_state;
}

} // namespace

TEST_F(WatchersTest, WatchTypeAssignsVersionTagAfterModification) {
  Ref<PyTypeObject> type{compileAndGet("class C: pass", "C")};
  ASSERT_NE(type, nullptr);

  ASSERT_EQ(watcherState().watchType(type), 0);
  ASSERT_TRUE(Ci_Type_HasValidVersionTag(type.get()));
  unsigned int version = type->tp_version_tag;
  ASSERT_EQ(watcherState().watchType(type), 0);
  EXPECT_EQ(type->tp_version_tag, version);

  // Modifying the type drops its version tag but leaves it watched. Watching
  // it again must assign a new tag, or later modifications go unreported.
  PyType_Modified(type);
  ASSERT_NE(type->tp_watched, 0);
  ASSERT_FALSE(Ci_Type_HasValidVersionTag(type.get()));
  ASSERT_EQ(watcherState().watchType(type), 0);
  EXPECT_TRUE(Ci_Type_HasValidVersionTag(type.get()));

  ASSERT_EQ(watcherState().unwatchType(type), 0);
}

#if defined(__linux__)

// Copies an object's fixed-size part into its own read-only page, so any store
// to the copy faults.
class ReadOnlyCopy {
 public:
  explicit ReadOnlyCopy(PyObject* obj) {
    auto size = static_cast<size_t>(Py_TYPE(obj)->tp_basicsize);
    JIT_CHECK(size <= kPageSize, "Object too large: {} bytes", size);
    page_ = mmap(
        nullptr,
        kPageSize,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    JIT_CHECK(page_ != MAP_FAILED, "mmap failed");
    std::memcpy(page_, obj, size);
    JIT_CHECK(mprotect(page_, kPageSize, PROT_READ) == 0, "mprotect failed");
  }

  ~ReadOnlyCopy() {
    munmap(page_, kPageSize);
  }

  ReadOnlyCopy(const ReadOnlyCopy&) = delete;
  ReadOnlyCopy& operator=(const ReadOnlyCopy&) = delete;

  template <typename T>
  T* get() const {
    return static_cast<T*>(page_);
  }

 private:
  void* page_;
};

TEST_F(WatchersTest, RewatchTypeDoesNotWrite) {
  if constexpr (!kPreforkModel) {
    GTEST_SKIP() << "Only prefork builds skip re-watching";
  }
  Ref<PyTypeObject> type{compileAndGet("class C: pass", "C")};
  ASSERT_NE(type, nullptr);
  ASSERT_EQ(watcherState().watchType(type), 0);

  {
    ReadOnlyCopy copy{type.getObj()};
    EXPECT_EQ(watcherState().watchType(copy.get<PyTypeObject>()), 0);
  }

  ASSERT_EQ(watcherState().unwatchType(type), 0);
}

#endif

} // namespace cinderx

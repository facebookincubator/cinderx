// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <mutex>
#include <shared_mutex>

namespace cinderx {

// Replace an inherited mutex with a fresh, unlocked mutex in the child,
// including resetting ThreadSanitizer's mutex metadata. locked_by_prepare
// indicates whether the prepare handler acquired it on the forking thread.
void resetMutexAfterFork(std::mutex& mutex, bool locked_by_prepare = true);
void resetMutexAfterFork(std::recursive_mutex& mutex);
void resetMutexAfterFork(std::shared_mutex& mutex);

// Keep TSAN in sync when a locked mutex is rebuilt as part of a larger object.
void destroyMutexMetadataBeforeReinit(std::mutex& mutex);
void createMutexMetadataAfterReinit(std::mutex& mutex);

} // namespace cinderx

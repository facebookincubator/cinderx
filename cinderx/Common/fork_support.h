// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <mutex>
#include <shared_mutex>

namespace cinderx {

// Replace a mutex locked by an atfork prepare handler with a fresh, unlocked
// mutex in the child, including resetting ThreadSanitizer's mutex metadata.
void resetMutexAfterFork(std::mutex& mutex);
void resetMutexAfterFork(std::recursive_mutex& mutex);
void resetMutexAfterFork(std::shared_mutex& mutex);

// Keep TSAN in sync when a locked mutex is rebuilt as part of a larger object.
void destroyMutexMetadataBeforeReinit(std::mutex& mutex);
void createMutexMetadataAfterReinit(std::mutex& mutex);

} // namespace cinderx

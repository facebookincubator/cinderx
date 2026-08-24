// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <mutex>

namespace cinderx {

// Replace a mutex locked by an atfork prepare handler with a fresh, unlocked
// mutex in the child, including resetting ThreadSanitizer's mutex metadata.
void resetMutexAfterFork(std::mutex& mutex);
void resetMutexAfterFork(std::recursive_mutex& mutex);

} // namespace cinderx

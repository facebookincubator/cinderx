// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/thread.h"

#include "cinderx/Common/log.h"

#include <fmt/std.h>

#include <string_view>
#include <thread>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace cinderx {

void setThreadName(const char* name) {
  JIT_DLOG("Naming thread {} as {}", std::this_thread::get_id(), name);

#ifndef _WIN32
  pthread_setname_np(
#ifndef __APPLE__
      pthread_self(),
#endif
      name);
#endif

  if (strlen(name) > kMaxThreadNameLen) {
    JIT_DLOG(
        "Warning: Thread named {} was truncated to {}",
        std::string_view{name},
        std::string_view{name, kMaxThreadNameLen});
  }
}

} // namespace cinderx

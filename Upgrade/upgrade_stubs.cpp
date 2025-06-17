// Copyright (c) Meta Platforms, Inc. and affiliates.
#define __UPGRADE_STUBS_CPP

#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove

#define STUB(ret, func, task, args...)                \
  ret func(args) {                                    \
    UPGRADE_ASSERT(Hit stubbed function : func task); \
  }

extern "C" {} // extern "C"

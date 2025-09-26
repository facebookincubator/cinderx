// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Common/ref.h"

#include <string_view>

namespace jit {

class IJITList {
 public:
  IJITList() = default;
  virtual ~IJITList() = default;

  IJITList(const IJITList&) = delete;
  IJITList& operator=(const IJITList&) = delete;

  virtual bool parseLine(std::string_view line) = 0;
  virtual void parseFile(const char* filename) = 0;

  virtual int lookupFunc(BorrowedRef<PyFunctionObject> function) const = 0;
  virtual int lookupCode(BorrowedRef<PyCodeObject> code) const = 0;
  virtual int lookupName(BorrowedRef<> module_name, BorrowedRef<> qualname)
      const = 0;

  virtual Ref<> getList() const = 0;
};

} // namespace jit

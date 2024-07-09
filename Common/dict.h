// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#if PY_VERSION_HEX < 0x030C0000

#include "Objects/dict-common.h"  // @donotremove
#define DICT_VALUES(dict) dict->ma_values

#else

#define DICT_VALUES(dict) dict->ma_values->values
#include "internal/pycore_dict.h"

#endif
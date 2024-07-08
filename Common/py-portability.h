// Copyright (c) Meta Platforms, Inc. and affiliates.
/*
 * Utilities to smooth out portability between base runtime versions.
 */

#pragma once

#if PY_VERSION_HEX < 0x030C0000
  #define CI_INTERP_IMPORT_FIELD(interp, field) interp->field
#else
  #define CI_INTERP_IMPORT_FIELD(interp, field) interp->imports.field
#endif

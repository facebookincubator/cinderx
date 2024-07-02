// Copyright (c) Meta Platforms, Inc. and affiliates.

/*
 * A place to put extra CinderX-only flags used on existing Python objects.
 */

#pragma once

// Additional PyCodeObject flags (see Include/code.h)
#define CI_CO_STATICALLY_COMPILED 0x4000000

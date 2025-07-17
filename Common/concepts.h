// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <concepts>

template <typename T, typename... U>
concept AnyOf = (std::same_as<T, U> || ...);

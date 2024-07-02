# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from _strictmodule import IStrictModuleLoader, StrictModuleLoaderFactory

from _strictmodule import (
    NONSTRICT_MODULE_KIND,
    STATIC_MODULE_KIND,
    STRICT_MODULE_KIND,
    StrictAnalysisResult,
    StrictModuleLoader,
    STUB_KIND_MASK_ALLOWLIST,
    STUB_KIND_MASK_NONE,
    STUB_KIND_MASK_STRICT,
    STUB_KIND_MASK_TYPING,
)


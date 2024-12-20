# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    # pyre-ignore[21]: _strictmodule is going away.
    from _strictmodule import (  # noqa: F401
        IStrictModuleLoader,
        StrictModuleLoaderFactory,
    )

# pyre-ignore[21]: _strictmodule is going away.
from _strictmodule import (  # noqa: F401
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

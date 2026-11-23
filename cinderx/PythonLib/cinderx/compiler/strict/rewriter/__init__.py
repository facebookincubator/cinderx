# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from .remove_annotations import (
    AnnotationRemover as AnnotationRemover,
    remove_annotations as remove_annotations,
)
from .rewriter import (
    rewrite as rewrite,
    StrictModuleRewriter as StrictModuleRewriter,
)

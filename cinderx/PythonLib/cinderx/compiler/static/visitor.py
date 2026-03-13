# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from __future__ import annotations

from ast import AST, ImportFrom
from contextlib import contextmanager, nullcontext
from typing import ContextManager, Generator, Generic, TYPE_CHECKING, TypeVar

from ..visitor import ASTVisitor

if TYPE_CHECKING:
    from ..errors import ErrorSink
    from .compiler import Compiler
    from .module_table import ModuleTable
    from .types import TypeEnvironment


TVisitRet = TypeVar("TVisitRet", covariant=True)


class GenericVisitor(ASTVisitor, Generic[TVisitRet]):
    def __init__(self, module: ModuleTable) -> None:
        super().__init__()
        self.module = module
        self.module_name: str = module.name
        self.filename: str = module.filename
        self.compiler: Compiler = module.compiler
        self.error_sink: ErrorSink = module.compiler.error_sink
        self.type_env: TypeEnvironment = module.compiler.type_env
        # the qualname that should be the "requester" of types used (for dep tracking)
        self._context_qualname: str = ""
        # if true, all deps tracked in visiting should be considered decl deps
        self.force_decl_deps: bool = False

    @property
    def context_qualname(self) -> str:
        return self._context_qualname

    @contextmanager
    def temporary_context_qualname(
        self, qualname: str | None, force_decl: bool = False
    ) -> Generator[None, None, None]:
        old_qualname = self._context_qualname
        self._context_qualname = qualname or ""
        old_decl = self.force_decl_deps
        self.force_decl_deps = force_decl
        try:
            yield
        finally:
            self._context_qualname = old_qualname
            self.force_decl_deps = old_decl

    def record_dependency(self, source: tuple[str, str]) -> None:
        self.module.record_dependency(
            self.context_qualname, source, force_decl=self.force_decl_deps
        )

    def visit(self, node: AST, *args: object) -> TVisitRet:
        # if we have a sequence of nodes, don't catch TypedSyntaxError here;
        # walk_list will call us back with each individual node in turn and we
        # can catch errors and add node info then.
        ctx = self.error_context(node) if isinstance(node, AST) else nullcontext()
        with ctx:
            if not args:
                # pyre-ignore: ASTVisitor is not generic yet, can't assert the result is
                # TVisitRet.
                return super().visit(node)
            # pyre-ignore: ASTVisitor is not generic yet, can't assert the result is
            # TVisitRet.
            return super().visit(node, *args)

    def syntax_error(self, msg: str, node: AST) -> None:
        return self.error_sink.syntax_error(msg, self.filename, node)

    def perf_warning(self, msg: str, node: AST) -> None:
        return self.error_sink.perf_warning(msg, self.filename, node)

    def error_context(self, node: AST | None) -> ContextManager[None]:
        if node is None:
            return nullcontext()
        return self.error_sink.error_context(self.filename, node)

    def _resolve_relative_import(self, node: ImportFrom) -> str:
        """Resolve a relative import to an absolute module name.

        For __init__ modules, level=1 means "this package" (no stripping).
        For non-__init__ modules, level=1 means "parent package" (strip one).
        Each additional level strips one more package component.
        """
        level = node.level
        parts = self.module_name.split(".")
        # __init__ modules ARE the package, so relative imports start from
        # the package itself rather than its parent.
        is_package = self.filename.endswith("__init__.py")
        strip = level if not is_package else level - 1
        if strip > len(parts):
            self.syntax_error(
                "attempted relative import beyond top-level package", node
            )
            return ""
        # Copy with list() when strip == 0 to avoid mutating `parts` via append below.
        base_parts = parts[: len(parts) - strip] if strip > 0 else list(parts)
        if node.module:
            base_parts.append(node.module)
        result = ".".join(base_parts)
        if not result:
            self.syntax_error(
                "attempted relative import beyond top-level package", node
            )
        return result

    @contextmanager
    def temporary_error_sink(self, sink: ErrorSink) -> Generator[None, None, None]:
        orig_sink = self.error_sink
        self.error_sink = sink
        try:
            yield
        finally:
            self.error_sink = orig_sink

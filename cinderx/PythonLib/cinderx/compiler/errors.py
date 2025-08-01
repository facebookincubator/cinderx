# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import linecache
from contextlib import contextmanager
from dataclasses import dataclass
from typing import TYPE_CHECKING


if TYPE_CHECKING:
    from ast import AST
    from typing import Generator


class TypedSyntaxError(SyntaxError):
    pass


class PerfWarning(Warning):
    def __init__(
        self,
        msg: str,
        filename: str,
        lineno: int,
        offset: int,
        text: str | None,
        end_lineno: int | None = None,
        end_offset: int | None = None,
    ) -> None:
        super().__init__(msg, filename, lineno, offset, text)
        self.msg = msg
        self.filename = filename
        self.lineno = lineno
        self.offset = offset
        self.text = text
        self.end_lineno = end_lineno
        self.end_offset = end_offset


@dataclass
class ErrorLocation:
    filename: str
    node: AST
    lineno: int
    offset: int
    source_line: str | None = None
    end_lineno: int | None = None
    end_offset: int | None = None


def error_location(filename: str, node: AST) -> ErrorLocation:
    # pyre-fixme[16]: `AST` has no attribute `lineno`.
    source_line = linecache.getline(filename, node.lineno)
    # The SyntaxError offset field is 1-indexed:
    # https://docs.python.org/3.10/library/exceptions.html#SyntaxError.offset
    # The AST col_offset field is 0-indexed:
    # https://docs.python.org/3/library/ast.html
    return ErrorLocation(
        filename,
        node,
        node.lineno,
        # pyre-fixme[16]: `AST` has no attribute `col_offset`.
        node.col_offset + 1,
        source_line or None,
        # pyre-fixme[16]: `AST` has no attribute `end_lineno`.
        node.end_lineno + 1 if node.end_lineno is not None else None,
        # pyre-fixme[16]: `AST` has no attribute `end_col_offset`.
        node.end_col_offset + 1 if node.end_col_offset is not None else None,
    )


class ErrorSink:
    throwing = True

    def __init__(self) -> None:
        self.errors: list[TypedSyntaxError] = []
        self.warnings: list[PerfWarning] = []

    def error(self, exception: TypedSyntaxError) -> None:
        raise exception

    def syntax_error(self, msg: str, filename: str, node: AST) -> None:
        errloc = error_location(filename, node)
        self.error(
            TypedSyntaxError(
                msg,
                (
                    errloc.filename,
                    errloc.lineno,
                    errloc.offset,
                    errloc.source_line,
                    errloc.end_lineno,
                    errloc.end_offset,
                ),
            )
        )

    @property
    def has_errors(self) -> bool:
        return len(self.errors) > 0

    @contextmanager
    def error_context(self, filename: str, node: AST) -> Generator[None, None, None]:
        """Add error location context to any TypedSyntaxError raised in with block."""
        try:
            yield
        except TypedSyntaxError as exc:
            if exc.filename is None:
                exc.filename = filename
            if (exc.lineno, exc.offset) == (None, None):
                errloc = error_location(filename, node)
                exc.lineno = errloc.lineno
                exc.offset = errloc.offset
                exc.text = errloc.source_line
                exc.end_lineno = errloc.end_lineno
                exc.end_offset = errloc.end_offset
            self.error(exc)

    def warn(self, warning: PerfWarning) -> None:
        pass

    def perf_warning(self, msg: str, filename: str, node: AST) -> None:
        errloc = error_location(filename, node)
        self.warn(
            PerfWarning(
                msg,
                filename,
                errloc.lineno,
                errloc.offset,
                errloc.source_line,
                errloc.end_lineno,
                errloc.end_offset,
            )
        )

    @property
    def has_warnings(self) -> bool:
        return len(self.warnings) > 0


class CollectingErrorSink(ErrorSink):
    throwing = False

    def error(self, exception: TypedSyntaxError) -> None:
        self.errors.append(exception)

    def warn(self, warning: PerfWarning) -> None:
        self.warnings.append(warning)

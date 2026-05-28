# Copyright (c) Meta Platforms, Inc. and affiliates.

# Type stubs for __static__.enum. The runtime classes use a custom EnumMeta /
# StringEnumMeta metaclass (see enum.py) that is structurally parallel to but
# independent from the stdlib `enum` module. Static type checkers (pyrefly,
# mypy) special-case stdlib `enum.Enum` to apply enum semantics — member
# assignments produce instances of the enclosing class, not the raw value
# type. To get those semantics, the stub declares the classes as subclasses
# of stdlib `enum.Enum` even though the runtime classes are not.

# lint-ignore: UnusedImportsRule: re-exported names
from enum import (
    Enum as Enum,
    EnumMeta as EnumMeta,
    IntEnum as IntEnum,
    unique as unique,
)
from typing import Iterator, Type, TypeVar

_T = TypeVar("_T")

class StringEnumMeta(EnumMeta): ...

class StringEnum(Enum, str, metaclass=StringEnumMeta):
    _value_: str

    @property
    def value(self) -> str: ...

    # When iterating over a StringEnum subclass, type checkers privilege the
    # inherited str.__iter__ over EnumMeta.__iter__, which is wrong and
    # produces "too few arguments for __iter__" errors. Declaring __iter__ as
    # a classmethod overrides str.__iter__ from the checker's perspective so
    # `for x in MyStringEnum` type-checks correctly. Mirrors the workaround
    # in distillery/util/enum.pyi.
    @classmethod
    def __iter__(cls: Type[_T]) -> Iterator[_T]: ...

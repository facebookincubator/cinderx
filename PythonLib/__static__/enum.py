# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from __future__ import annotations

import inspect
from typing import Iterable, Mapping, Sequence, SupportsIndex, Type


def eq_method(self: Enum, other: Enum) -> bool:
    return self.value == other or (
        getattr(other, "value", None) is not None and self.value == other.value
    )


def hash_method(self: Enum) -> int:
    return hash(self.value)


TYPE_NEW: object = type.__new__
SENTINEL = object()


class EnumMeta(type):
    @staticmethod
    def is_hashable(obj: object) -> bool:
        try:
            hash(obj)
            return True
        except TypeError:
            return False

    def __new__(
        metacls,
        classname: str,
        parents: tuple[Type[object], ...],
        dct: Mapping[str, object],
    ) -> type[Enum]:
        attributes = {}
        members = {}

        for name, value in dct.items():
            if (
                name.startswith("_")
                or inspect.isroutine(value)
                or inspect.isdatadescriptor(value)
            ):
                attributes[name] = value
            else:
                members[name] = value

        attributes.update({"__members__": {}, "__reversed_map__": {}})
        if len(members) != len(
            {members[k] for k in members if EnumMeta.is_hashable(members[k])}
        ):
            attributes["__eq__"] = eq_method
            attributes["__hash__"] = hash_method

        klass = super().__new__(metacls, classname, parents, attributes)

        for name, value in members.items():
            option = klass(name=name, value=value)
            # pyre-ignore[16]: __members__ is dynamically defined
            klass.__members__[name] = option
            if EnumMeta.is_hashable(option):
                # pyre-ignore[16]: __reversed_map__ is dynamically defined
                klass.__reversed_map__[option] = option
            if EnumMeta.is_hashable(value):
                klass.__reversed_map__[value] = option
            setattr(klass, name, option)
        return klass

    def __len__(cls) -> int:
        # pyre-ignore[16]: __members__ is dynamically defined
        return len(cls.__members__)

    def __getitem__(cls, attribute: str) -> Enum:
        # pyre-ignore[16]: __members__ is dynamically defined
        return cls.__members__[attribute]

    def __iter__(cls) -> Iterable[Enum]:
        # pyre-ignore[16]: __members__ is dynamically defined
        return iter(cls.__members__.values())

    def __call__(cls, *args: object, **kwargs: object) -> Enum:
        if len(args) == 1:
            attribute = args[0]
            # pyre-ignore[6]: expected str, got object
            return cls._get_by_value(attribute)

        name = kwargs["name"]
        value = kwargs["value"]
        # pyre-ignore[20]: __new__ expects dct
        # pyre-ignore[9]:declared to have type Enum but is used as Type[Enum]
        instance: Enum = cls.__new__(cls, value)
        instance.name = name
        instance.value = value
        return instance

    def _get_by_value(cls, value: str) -> Enum:
        # pyre-ignore[16]: __reversed_map__ is dynamically defined
        res = cls.__reversed_map__.get(value, SENTINEL)
        if res is not SENTINEL:
            return res

        raise ValueError(
            f"Enum type {cls.__name__} has no attribute with value {value!r}"
        )


class Enum(metaclass=EnumMeta):
    def __init__(self, value: object) -> None:
        self.value = value
        self.name: object = None

    def __dir__(self) -> Sequence[str]:
        return ["name", "value"]

    def __str__(self) -> str:
        return f"{type(self).__name__}.{self.name}"

    def __repr__(self) -> str:
        return f"<{type(self).__name__}.{self.name}: {self.value}>"

    def __reduce_ex__(self, proto: SupportsIndex) -> tuple[type[object], tuple[object]]:
        return self.__class__, (self.value,)


class IntEnum(Enum, int):
    pass


class StringEnumMeta(EnumMeta):
    """Like the regular EnumMeta, but parses string/binary inputs to __call__
    as text (to match text literals used in StringEnum)."""

    def _get_by_value(cls, value: str | bytes) -> StringEnum:
        # pyre-ignore[7]: Expected StringEnum, got Enum
        return super()._get_by_value(
            value.decode("utf-8") if isinstance(value, bytes) else value
        )


class StringEnum(Enum, str, metaclass=StringEnumMeta):
    def __str__(self) -> str:
        return f"{self.value}"


def unique(enumeration: type[Enum]) -> type[Enum]:
    """
    Class decorator for enumerations ensuring unique member values
    """
    duplicates = []
    # pyre-ignore[16]: __members__ is dynamically defined
    for name, member in enumeration.__members__.items():
        if name != member.name:
            duplicates.append((name, member.name))
    if duplicates:
        raise ValueError(f"duplicate values found in {enumeration!r}")
    return enumeration

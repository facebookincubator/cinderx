# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest
from typing import Any

from cinderx import async_cached_property, cached_property


class Holder:
    __slots__ = ("slot", "__dict__")


def func(self) -> int:
    return 1


class UninitializedCachedPropertyTest(unittest.TestCase):
    """`T.__new__(T)` skips __init__, which is what sets the fields."""

    def descriptor_types(self) -> list[Any]:
        # The *_with_descr variants are reachable only through the slot form.
        slot = Holder.__dict__["slot"]
        types = [
            cached_property,
            async_cached_property,
            type(cached_property(func, slot)),
            type(async_cached_property(func, slot)),
        ]
        self.assertEqual(len({t.__name__ for t in types}), 4)
        return types

    def test_every_attribute_raises(self) -> None:
        """No attribute or method of an uninitialized descriptor may crash."""
        for prop_type in self.descriptor_types():
            for attr in ("name", "slot", "__name__"):
                with self.subTest(prop_type=prop_type.__name__, attr=attr):
                    prop = prop_type.__new__(prop_type)
                    with self.assertRaises((TypeError, AttributeError)):
                        getattr(prop, attr)

    def test_every_call_shape_raises(self) -> None:
        for prop_type in self.descriptor_types():
            with self.subTest(prop_type=prop_type.__name__):
                holder = Holder()
                prop = prop_type.__new__(prop_type)
                msg = "is not initialized"

                with self.assertRaisesRegex(TypeError, msg):
                    prop.__get__(holder, Holder)
                # Only the sync types expose clear()/has_value()/__set_name__.
                if hasattr(prop, "clear"):
                    with self.assertRaisesRegex(TypeError, msg):
                        prop.clear(holder)
                    with self.assertRaisesRegex(TypeError, msg):
                        prop.has_value(holder)
                    with self.assertRaisesRegex(TypeError, msg):
                        prop.__set_name__(Holder, "p")

                # Only the *_with_descr variants have a setter and a deleter.
                if hasattr(prop, "__set__"):
                    with self.assertRaisesRegex(TypeError, msg):
                        prop.__set__(holder, 1)
                if hasattr(prop, "__delete__"):
                    with self.assertRaisesRegex(TypeError, msg):
                        prop.__delete__(holder)

    def test_class_access_still_returns_the_descriptor(self) -> None:
        for prop_type in self.descriptor_types():
            with self.subTest(prop_type=prop_type.__name__):
                prop = prop_type.__new__(prop_type)
                self.assertIs(prop.__get__(None, Holder), prop)

    def test_initialized_properties_are_unaffected(self) -> None:
        class C:
            @cached_property
            def prop(self) -> int:
                return 42

        c = C()
        self.assertEqual(c.prop, 42)
        prop: Any = C.prop
        self.assertEqual(prop.name, "prop")
        self.assertIsNone(prop.slot)

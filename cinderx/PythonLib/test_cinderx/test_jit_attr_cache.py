# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import unittest
from textwrap import dedent

import cinderx
import cinderx.jit
import cinderx.test_support as cinder_support
from cinderx.test_support import passIf, passUnless, skip_if_ft, skip_module_if_oss

skip_module_if_oss()

from .common import failUnlessHasOpcodes

if cinderx.is_initialized():
    from .test_compiler.test_strict.test_loader import base_sandbox, sandbox


def nothing():
    return 0


@cinder_support.failUnlessJITCompiled
def get_meaning_of_life(obj):
    return obj.meaning_of_life()


class LoadMethodCacheTests(unittest.TestCase):
    def test_type_modified(self):
        class Oracle:
            def meaning_of_life(self):
                return 42

        obj = Oracle()
        # Uncached
        self.assertEqual(get_meaning_of_life(obj), 42)
        # Cached
        self.assertEqual(get_meaning_of_life(obj), 42)

        # Invalidate cache
        def new_meaning_of_life(x):
            return 0

        # pyrefly: ignore [bad-assignment]
        Oracle.meaning_of_life = new_meaning_of_life

        self.assertEqual(get_meaning_of_life(obj), 0)

    def test_base_type_modified(self):
        class Base:
            def meaning_of_life(self):
                return 42

        class Derived(Base):
            pass

        obj = Derived()
        # Uncached
        self.assertEqual(get_meaning_of_life(obj), 42)
        # Cached
        self.assertEqual(get_meaning_of_life(obj), 42)

        # Mutate Base. Should propagate to Derived and invalidate the cache.
        def new_meaning_of_life(x):
            return 0

        # pyrefly: ignore [bad-assignment]
        Base.meaning_of_life = new_meaning_of_life

        self.assertEqual(get_meaning_of_life(obj), 0)

    def test_second_base_type_modified(self):
        class Base1:
            pass

        class Base2:
            def meaning_of_life(self):
                return 42

        class Derived(Base1, Base2):
            pass

        obj = Derived()
        # Uncached
        self.assertEqual(get_meaning_of_life(obj), 42)
        # Cached
        self.assertEqual(get_meaning_of_life(obj), 42)

        # Mutate first base. Should propagate to Derived and invalidate the cache.
        def new_meaning_of_life(x):
            return 0

        Base1.meaning_of_life = new_meaning_of_life

        self.assertEqual(get_meaning_of_life(obj), 0)

    def test_type_dunder_bases_reassigned(self):
        class Base1:
            pass

        class Derived(Base1):
            pass

        # No shadowing happens between obj{1,2} and Derived, thus the now
        # shadowing flag should be set
        obj1 = Derived()
        obj2 = Derived()
        # pyrefly: ignore [missing-attribute]
        obj2.meaning_of_life = nothing

        # Now obj2.meaning_of_life shadows Base.meaning_of_life
        class Base2:
            def meaning_of_life(self):
                return 42

        Derived.__bases__ = (Base2,)

        # Attempt to prime the cache
        self.assertEqual(get_meaning_of_life(obj1), 42)
        self.assertEqual(get_meaning_of_life(obj1), 42)
        # If flag is not correctly cleared when Derived.__bases__ is
        # assigned we will end up returning 42
        self.assertEqual(get_meaning_of_life(obj2), 0)

    def _make_obj(self):
        class Oracle:
            def meaning_of_life(self):
                return 42

        obj = Oracle()
        # Uncached
        self.assertEqual(get_meaning_of_life(obj), 42)
        # Cached
        self.assertEqual(get_meaning_of_life(obj), 42)
        return obj

    def test_instance_assignment(self):
        obj = self._make_obj()
        obj.meaning_of_life = nothing
        self.assertEqual(get_meaning_of_life(obj), 0)

    def test_instance_dict_assignment(self):
        obj = self._make_obj()
        obj.__dict__["meaning_of_life"] = nothing
        self.assertEqual(get_meaning_of_life(obj), 0)

    def test_instance_dict_replacement(self):
        obj = self._make_obj()
        obj.__dict__ = {"meaning_of_life": nothing}
        self.assertEqual(get_meaning_of_life(obj), 0)

    def test_instance_dunder_class_assignment(self):
        obj = self._make_obj()

        class Other:
            pass

        other = Other()
        # pyrefly: ignore [missing-attribute]
        other.meaning_of_life = nothing
        other.__class__ = obj.__class__
        self.assertEqual(get_meaning_of_life(other), 0)

    def test_shadowcode_setattr(self):
        """sets attribute via shadow byte code, it should update the
        type bit for instance shadowing"""
        obj = self._make_obj()
        obj.foo = 42
        obj1 = type(obj)()
        obj1.other = 100

        def f(obj, set):
            if set:
                obj.meaning_of_life = nothing
            yield 42

        for _ in range(100):
            list(f(obj, False))
        list(f(obj, True))

        self.assertEqual(get_meaning_of_life(obj), 0)

    def test_shadowcode_setattr_split(self):
        """sets attribute via shadow byte code on a split dict,
        it should update the type bit for instance shadowing"""
        obj = self._make_obj()

        def f(obj, set):
            if set:
                obj.meaning_of_life = nothing
            yield 42

        for _ in range(100):
            list(f(obj, False))
        list(f(obj, True))

        self.assertEqual(get_meaning_of_life(obj), 0)

    def _index_long(self):
        return (6).__index__()

    def test_call_wrapper_descriptor(self):
        self.assertEqual(self._index_long(), 6)


@passUnless(cinderx.jit.is_enabled(), "Test uses the JIT")
class LoadModuleMethodCacheTests(unittest.TestCase):
    @skip_if_ft("T250369692: LoadModuleAttrCached not supported with free-threading")
    def test_load_method_from_module(self):
        with cinder_support.temp_sys_path() as tmp:
            (tmp / "tmp_a.py").write_text(
                dedent(
                    """
                    a = 1
                    def get_a():
                        return 1+2
                    """
                ),
                encoding="utf8",
            )
            (tmp / "tmp_b.py").write_text(
                dedent(
                    """
                    import tmp_a

                    def test():
                        return tmp_a.get_a()
                    """
                ),
                encoding="utf8",
            )

            # pyre-ignore[21]: Dynamically generated as part of this test.
            import tmp_b

            cinderx.jit.force_compile(tmp_b.test)

            self.assertEqual(tmp_b.test(), 3)
            self.assertTrue(cinderx.jit.is_jit_compiled(tmp_b.test))
            self.assertIn(
                "LoadModuleAttrCached",
                # pyrefly: ignore [bad-argument-type]
                cinderx.jit.get_function_hir_opcode_counts(tmp_b.test),
            )

            # pyre-ignore[21]: Dynamically generated as part of this test.
            import tmp_a

            tmp_a.get_a = lambda: 10
            self.assertEqual(tmp_b.test(), 10)
            delattr(tmp_a, "get_a")
            with self.assertRaises(AttributeError):
                tmp_b.test()

    @skip_if_ft("T250369692: LoadModuleAttrCached not supported with free-threading")
    @passUnless(
        cinderx.is_initialized(),
        "Strict Module test code doesn't exist outside of CinderX",
    )
    def test_load_method_from_strict_module(self):
        strict_sandbox = base_sandbox.use_cm(sandbox, self)
        code_str = """
        import __strict__
        a = 1
        def get_a():
            return 1+2
        """
        strict_sandbox.write_file("tmp_a.py", code_str)
        code_str = """
        import __strict__
        import tmp_a

        def test():
            return tmp_a.get_a()
        """
        strict_sandbox.write_file("tmp_b.py", code_str)
        tmp_b = strict_sandbox.strict_import("tmp_b")
        # pyrefly: ignore [missing-attribute]
        cinderx.jit.force_compile(tmp_b.test)
        # pyrefly: ignore [missing-attribute]
        self.assertTrue(cinderx.jit.is_jit_compiled(tmp_b.test))
        self.assertIn(
            "LoadModuleAttrCached",
            # pyrefly: ignore [bad-argument-type, missing-attribute]
            cinderx.jit.get_function_hir_opcode_counts(tmp_b.test),
        )
        # prime the cache
        # pyrefly: ignore [missing-attribute]
        self.assertEqual(tmp_b.test(), 3)
        # pyrefly: ignore [missing-attribute]
        self.assertEqual(tmp_b.test(), 3)


@cinder_support.failUnlessJITCompiled
@failUnlessHasOpcodes("LOAD_ATTR")
def get_foo(obj):
    return obj.foo


class LoadAttrCacheTests(unittest.TestCase):
    def test_dict_reassigned(self):
        class Base:  # noqa: B903
            def __init__(self, x):
                self.foo = x

        obj1 = Base(100)
        obj2 = Base(200)
        # uncached
        self.assertEqual(get_foo(obj1), 100)
        # cached
        self.assertEqual(get_foo(obj1), 100)
        self.assertEqual(get_foo(obj2), 200)
        obj1.__dict__ = {"foo": 200}
        self.assertEqual(get_foo(obj1), 200)
        self.assertEqual(get_foo(obj2), 200)

    def test_dict_mutated(self):
        class Base:  # noqa: B903
            def __init__(self, foo):
                self.foo = foo

        obj = Base(100)
        # uncached
        self.assertEqual(get_foo(obj), 100)
        # cached
        self.assertEqual(get_foo(obj), 100)
        obj.__dict__["foo"] = 200
        self.assertEqual(get_foo(obj), 200)

    def test_dict_resplit(self):
        # This causes one resize of the instance dictionary, which should cause
        # it to go from split -> combined -> split.
        class Base:
            def __init__(self):
                self.foo, self.a, self.b = 100, 200, 300
                self.c, self.d, self.e = 400, 500, 600

        obj = Base()
        # uncached
        self.assertEqual(get_foo(obj), 100)
        # cached
        self.assertEqual(get_foo(obj), 100)
        obj.foo = 800
        self.assertEqual(get_foo(obj), 800)

    def test_dict_combined(self):
        class Base:  # noqa: B903
            def __init__(self, foo):
                self.foo = foo

        obj1 = Base(100)
        # uncached
        self.assertEqual(get_foo(obj1), 100)
        # cached
        self.assertEqual(get_foo(obj1), 100)
        obj2 = Base(200)
        # pyrefly: ignore [missing-attribute]
        obj2.bar = 300
        # At this point the dictionary should still be split
        obj3 = Base(400)
        # pyrefly: ignore [missing-attribute]
        obj3.baz = 500
        # Assigning 'baz' should clear the cached key object for Base and leave
        # existing instance dicts in the following states:
        #
        # obj1.__dict__ - Split
        # obj2.__dict__ - Split
        # obj3.__dict__ - Combined
        obj4 = Base(600)
        self.assertEqual(get_foo(obj1), 100)
        self.assertEqual(get_foo(obj2), 200)
        self.assertEqual(get_foo(obj3), 400)
        self.assertEqual(get_foo(obj4), 600)

    def test_descr_type_mutated(self):
        class Descr:
            def __get__(self, obj, ty):
                return "I'm a getter!"

            def __set__(self, obj, ty):
                raise RuntimeError("unimplemented")

        class C:
            foo = Descr()

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.foo

        c = C()
        self.assertEqual(get_attr(c), "I'm a getter!")
        c.__dict__["foo"] = "I'm an attribute!"
        self.assertEqual(get_attr(c), "I'm a getter!")
        descr_set = Descr.__set__
        del Descr.__set__
        self.assertEqual(get_attr(c), "I'm an attribute!")
        Descr.__set__ = descr_set
        self.assertEqual(get_attr(c), "I'm a getter!")

    def test_descr_type_changed(self):
        class Descr:
            def __get__(self, obj, ty):
                return "get"

            def __set__(self, obj, ty):
                raise RuntimeError("unimplemented")

        class GetDescr:
            def __get__(self, obj, ty):
                return "only get"

        class C:
            foo = Descr()

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.foo

        c = C()
        self.assertEqual(get_attr(c), "get")
        c.__dict__["foo"] = "attribute"
        self.assertEqual(get_attr(c), "get")
        C.__dict__["foo"].__class__ = GetDescr
        self.assertEqual(get_attr(c), "attribute")
        del c.__dict__["foo"]
        self.assertEqual(get_attr(c), "only get")
        C.__dict__["foo"].__class__ = Descr
        self.assertEqual(get_attr(c), "get")

    def test_shared_descr_type_across_cache_entries(self):
        """Two types share the same descriptor type in the same cache.

        When one type changes and its cache entry is reset, the descriptor
        type watch must survive for the remaining entry. Otherwise mutating
        the descriptor type (e.g. removing __set__) won't invalidate the
        surviving entry.
        """

        class Descr:
            def __get__(self, obj, ty):
                return "descr"

            def __set__(self, obj, val):
                raise RuntimeError("unimplemented")

        class T1:
            foo = Descr()

        class T2:
            foo = Descr()

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.foo

        t1 = T1()
        t2 = T2()

        # Prime and cache both entries (same descriptor type Descr).
        self.assertEqual(get_attr(t1), "descr")
        self.assertEqual(get_attr(t1), "descr")
        self.assertEqual(get_attr(t2), "descr")
        self.assertEqual(get_attr(t2), "descr")

        # Both t1 and t2 have instance dict entries shadowed by
        # the data descriptor.
        t1.__dict__["foo"] = "t1 attr"
        t2.__dict__["foo"] = "t2 attr"
        self.assertEqual(get_attr(t1), "descr")
        self.assertEqual(get_attr(t2), "descr")

        # Invalidate T1's cache entry by mutating T1. This must NOT
        # unwatch Descr from the descriptor watcher since T2's entry
        # still depends on it.
        T1.bar = 1

        # Now mutate the descriptor type: removing __set__ makes Descr
        # no longer a data descriptor. T2's entry must be invalidated
        # so the instance attribute is returned instead.
        del Descr.__set__
        self.assertEqual(get_attr(t2), "t2 attr")

    def test_non_data_descr_becomes_data_descr(self):
        """When __set__ is added to a non-data descriptor type, it becomes a
        data descriptor and should take priority over instance dict entries."""

        class NonDataDescr:
            def __get__(self, obj, ty):
                return "from_descr"

        class C:
            foo = NonDataDescr()

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.foo

        c = C()
        # Non-data descriptor: instance dict takes priority
        c.__dict__["foo"] = "from_dict"
        self.assertEqual(get_attr(c), "from_dict")
        self.assertEqual(get_attr(c), "from_dict")

        # Add __set__ to make it a data descriptor: descriptor now takes
        # priority over instance dict
        NonDataDescr.__set__ = lambda self, obj, val: None
        self.assertEqual(get_attr(c), "from_descr")
        self.assertEqual(get_attr(c), "from_descr")

        # Remove __set__ again: back to non-data descriptor
        del NonDataDescr.__set__
        self.assertEqual(get_attr(c), "from_dict")

    @passIf(
        cinderx.jit.is_enabled(),
        "T214641462: Not clear why this is failing, but it is",
    )
    def test_type_destroyed(self):
        class A:
            pass

        class Attr:
            foo = "in Attr"

        # When C is first created with A as a base, return a normal MRO. When
        # __bases__ is reassigned later to use Attr as a base, return an MRO
        # without C in it. This gives us a type with no reference cycles in it,
        # which can be destroyed without running the GC (which calls
        # type_clear() and hides the bug).
        class NoSelfMRO(type):
            def mro(cls):
                if cls.__bases__ == (A,):
                    return (cls, A, object)
                return (cls.__bases__[0], object)

        class C(A, metaclass=NoSelfMRO):
            __slots__ = ()

        C.__bases__ = (Attr,)

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.foo

        c = C()
        self.assertEqual(get_attr(c), "in Attr")

        del c
        del C

        class D:
            foo = "in D"

        d = D()
        self.assertEqual(get_attr(d), "in D")

    def test_property_raises_attr_error_with_getattr_fallback(self):
        """When a property raises AttributeError on a type with __getattr__,
        __getattr__ should be invoked as a fallback, matching CPython's
        _Py_slot_tp_getattr_hook behavior."""

        class C:
            @property
            def foo(self):
                raise AttributeError("nope")

            def __getattr__(self, name):
                return f"fallback:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.foo

        c = C()
        # Uncached
        self.assertEqual(get_attr(c), "fallback:foo")
        # Cached
        self.assertEqual(get_attr(c), "fallback:foo")

    def test_data_descriptor_raises_attr_error_with_getattr_fallback(self):
        """When a data descriptor's __get__ raises AttributeError on a type
        with __getattr__, __getattr__ should be invoked as a fallback."""

        class RaisingDescr:
            def __get__(self, obj, cls):
                raise AttributeError("custom descr error")

            def __set__(self, obj, val):
                pass

        class C:
            x = RaisingDescr()

            def __getattr__(self, name):
                return f"fallback:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.x

        c = C()
        # Uncached
        self.assertEqual(get_attr(c), "fallback:x")
        # Cached
        self.assertEqual(get_attr(c), "fallback:x")

    def test_non_data_descriptor_raises_attr_error_with_getattr_fallback(self):
        """When a non-data descriptor's __get__ raises AttributeError on a type
        with __getattr__, __getattr__ should be invoked as a fallback."""

        class RaisingDescr:
            def __get__(self, obj, cls):
                raise AttributeError("non-data descr error")

        class C:
            x = RaisingDescr()

            def __getattr__(self, name):
                return f"fallback:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.x

        c = C()
        # Uncached
        self.assertEqual(get_attr(c), "fallback:x")
        # Cached
        self.assertEqual(get_attr(c), "fallback:x")

    def test_property_no_error_with_getattr(self):
        """When a property succeeds on a type with __getattr__, the property
        value should be returned normally (not __getattr__)."""

        class C:
            @property
            def foo(self):
                return "from_property"

            def __getattr__(self, name):
                return f"fallback:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_attr(o):
            return o.foo

        c = C()
        # Uncached
        self.assertEqual(get_attr(c), "from_property")
        # Cached
        self.assertEqual(get_attr(c), "from_property")

    def test_instance_dict_miss_with_getattr_fallback(self):
        """When an attribute is not in the instance dict on a type with
        __getattr__, __getattr__ should be invoked."""

        class C:
            def __init__(self):
                self.existing = 42

            def __getattr__(self, name):
                return f"fallback:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_existing(o):
            return o.existing

        @cinder_support.failUnlessJITCompiled
        def get_missing(o):
            return o.missing

        c = C()
        # Instance dict hit should work normally
        self.assertEqual(get_existing(c), 42)
        self.assertEqual(get_existing(c), 42)
        # Instance dict miss should fall through to __getattr__
        self.assertEqual(get_missing(c), "fallback:missing")
        self.assertEqual(get_missing(c), "fallback:missing")


@cinder_support.failUnlessJITCompiled
@failUnlessHasOpcodes("STORE_ATTR")
def set_foo(x, val):
    x.foo = val


class DataDescr:
    def __init__(self, val):
        self.val = val
        self.invoked = False

    def __get__(self, obj, typ):
        return self.val

    def __set__(self, obj, val):
        self.invoked = True


class StoreAttrCacheTests(unittest.TestCase):
    def test_data_descr_attached(self):
        class Base:  # noqa: B903
            def __init__(self, x):
                self.foo = x

        obj = Base(100)

        # Uncached
        set_foo(obj, 200)
        # Cached
        set_foo(obj, 200)
        self.assertEqual(obj.foo, 200)

        # Attaching a data descriptor to the type should invalidate the cache
        # and prevent future caching
        descr = DataDescr(300)
        # pyrefly: ignore [no-access]
        Base.foo = descr
        set_foo(obj, 200)
        self.assertEqual(obj.foo, 300)
        self.assertTrue(descr.invoked)

        descr.invoked = False
        set_foo(obj, 400)
        self.assertEqual(obj.foo, 300)
        self.assertTrue(descr.invoked)

    def test_swap_split_dict_with_combined(self):
        class Base:  # noqa: B903
            def __init__(self, x):
                self.foo = x

        obj = Base(100)

        # Uncached
        set_foo(obj, 200)
        # Cached
        set_foo(obj, 200)
        self.assertEqual(obj.foo, 200)

        # At this point obj should have a split dictionary for attribute
        # storage. We're going to swap it out with a combined dictionary
        # and verify that attribute stores still work as expected.
        d = {"foo": 300}
        obj.__dict__ = d
        set_foo(obj, 400)
        self.assertEqual(obj.foo, 400)
        self.assertEqual(d["foo"], 400)

    def test_swap_combined_dict_with_split(self):
        class Base:  # noqa: B903
            def __init__(self, x):
                self.foo = x

        # Swap out obj's dict with a combined dictionary. Priming the IC
        # for set_foo will result in it expecting a combined dictionary
        # for instances of type Base.
        obj = Base(100)
        obj.__dict__ = {"foo": 100}
        # Uncached
        set_foo(obj, 200)
        # Cached
        set_foo(obj, 200)
        self.assertEqual(obj.foo, 200)

        # obj2 should have a split dictionary used for attribute storage
        # which will result in a cache miss in the IC
        obj2 = Base(300)
        set_foo(obj2, 400)
        self.assertEqual(obj2.foo, 400)

    def test_split_dict_no_slot(self):
        class Base:
            pass

        # obj is a split dict
        obj = Base()
        # pyrefly: ignore [missing-attribute]
        obj.quox = 42

        # obj1 is no longer split, but the assignment
        # didn't go through Cix_PyObjectDict_SetItem, so the type
        # still has a valid CACHED_KEYS
        obj1 = Base()
        obj1.__dict__["other"] = 100

        # now we try setting foo on obj1, do the set on obj1
        # while setting up the cache, but attempt to create a cache
        # with an invalid val_offset because there's no foo
        # entry in the cached keys.
        set_foo(obj1, 300)
        # pyrefly: ignore [missing-attribute]
        self.assertEqual(obj1.foo, 300)

        set_foo(obj, 400)
        self.assertEqual(obj1.foo, 300)


class MetaclassGetAttrCacheTests(unittest.TestCase):
    """Tests for inline cache behavior with metaclass __getattr__.

    Regression tests for a bug where the JIT inline cache for LOAD_ATTR
    incorrectly cached a __getattr__ dispatch for class objects whose metaclass
    defines __getattr__, skipping MRO lookup for inherited class attributes.

    The correct behavior is:
    1. Inherited attributes should be found via MRO (type_getattro), NOT via
       metaclass __getattr__.
    2. Truly missing attributes should call metaclass __getattr__.
    """

    def test_metaclass_getattr_inherited_attr(self):
        """Inherited class attributes must be found via MRO, not metaclass
        __getattr__, even after the IC is populated."""

        class MyMeta(type):
            def __getattr__(cls, item):
                raise AttributeError(item)

        class Base(metaclass=MyMeta):
            inherited_attr = False

        class Child(Base):
            pass

        @cinder_support.failUnlessJITCompiled
        def get_inherited(cls):
            return cls.inherited_attr

        # Uncached - should find via MRO
        self.assertFalse(get_inherited(Child))
        # Cached - should still find via MRO, not call MyMeta.__getattr__
        self.assertFalse(get_inherited(Child))
        # Run enough iterations to thoroughly exercise the IC
        for _ in range(100):
            self.assertFalse(get_inherited(Child))

    def test_metaclass_getattr_missing_attr(self):
        """Truly missing attributes should call metaclass __getattr__."""

        class MyMeta(type):
            def __getattr__(cls, item):
                return f"meta_fallback:{item}"

        class Base(metaclass=MyMeta):
            existing = True

        class Child(Base):
            pass

        @cinder_support.failUnlessJITCompiled
        def get_missing(cls):
            return cls.nonexistent

        # Should call MyMeta.__getattr__
        self.assertEqual(get_missing(Child), "meta_fallback:nonexistent")
        self.assertEqual(get_missing(Child), "meta_fallback:nonexistent")
        for _ in range(100):
            self.assertEqual(get_missing(Child), "meta_fallback:nonexistent")

    def test_metaclass_getattr_pydantic_pattern(self):
        """Reproduces the Pydantic pattern that triggered the production
        outage: metaclass with __getattr__ + inherited boolean class attribute.

        This mimics Pydantic's ModelMetaclass and BaseModel pattern where
        __pydantic_root_model__ is defined on BaseModel and inherited by
        child models."""

        class ModelMeta(type):
            def __getattr__(cls, item):
                raise AttributeError(item)

        class BaseModel(metaclass=ModelMeta):
            __pydantic_root_model__ = False

        class UserModel(BaseModel):
            pass

        class RootModel(BaseModel):
            __pydantic_root_model__ = True

        @cinder_support.failUnlessJITCompiled
        def is_root_model(cls):
            return cls.__pydantic_root_model__

        # UserModel inherits __pydantic_root_model__ = False from BaseModel
        self.assertFalse(is_root_model(UserModel))
        self.assertFalse(is_root_model(UserModel))

        # RootModel overrides it to True
        self.assertTrue(is_root_model(RootModel))
        self.assertTrue(is_root_model(RootModel))

        # After IC is populated for one type, the other should still work
        for _ in range(100):
            self.assertFalse(is_root_model(UserModel))
            self.assertTrue(is_root_model(RootModel))

    def test_metaclass_getattr_mixed_access(self):
        """Test that the IC correctly handles a mix of inherited and missing
        attributes when a metaclass defines __getattr__."""

        class MyMeta(type):
            def __getattr__(cls, item):
                return f"meta:{item}"

        class Base(metaclass=MyMeta):
            class_attr = 42

        class Child(Base):
            own_attr = 100

        @cinder_support.failUnlessJITCompiled
        def get_class_attr(cls):
            return cls.class_attr

        @cinder_support.failUnlessJITCompiled
        def get_own_attr(cls):
            return cls.own_attr

        @cinder_support.failUnlessJITCompiled
        def get_missing_attr(cls):
            return cls.totally_missing

        # Inherited attribute via MRO
        for _ in range(100):
            self.assertEqual(get_class_attr(Child), 42)

        # Own attribute
        for _ in range(100):
            self.assertEqual(get_own_attr(Child), 100)

        # Missing attribute -> metaclass __getattr__
        for _ in range(100):
            self.assertEqual(get_missing_attr(Child), "meta:totally_missing")

    def test_metaclass_getattr_does_not_affect_instances(self):
        """Metaclass __getattr__ should not interfere with instance attribute
        access on instances of the class."""

        class MyMeta(type):
            def __getattr__(cls, item):
                return f"meta:{item}"

        class MyClass(metaclass=MyMeta):
            class_val = "from_class"

            def __init__(self):
                self.inst_val = "from_instance"

        @cinder_support.failUnlessJITCompiled
        def get_inst_val(obj):
            return obj.inst_val

        @cinder_support.failUnlessJITCompiled
        def get_class_val(obj):
            return obj.class_val

        obj = MyClass()
        # Instance attribute lookup should work normally
        for _ in range(100):
            self.assertEqual(get_inst_val(obj), "from_instance")
            self.assertEqual(get_class_val(obj), "from_class")


class GetAttrMutationTests(unittest.TestCase):
    """Tests that the IC correctly handles mutations to __getattr__ after
    the cache has been populated.

    The type watcher system should invalidate IC entries when __getattr__
    is added or removed, ensuring correct behavior even after mutation.
    """

    def test_add_getattr_after_ic_populated(self):
        """Adding __getattr__ to a class after the IC has cached attribute
        lookups should not break anything. The IC should be invalidated and
        subsequent lookups should use __getattr__ for missing attributes."""

        class C:
            def __init__(self):
                self.x = 42

        @cinder_support.failUnlessJITCompiled
        def get_x(o):
            return o.x

        @cinder_support.failUnlessJITCompiled
        def get_missing(o):
            return o.missing

        c = C()
        # Populate IC
        self.assertEqual(get_x(c), 42)
        self.assertEqual(get_x(c), 42)
        # Missing attr raises AttributeError
        with self.assertRaises(AttributeError):
            get_missing(c)

        # Now add __getattr__ to the class
        C.__getattr__ = lambda self, name: f"fallback:{name}"

        # Existing attribute should still work
        self.assertEqual(get_x(c), 42)
        # Missing attribute should now use __getattr__
        self.assertEqual(get_missing(c), "fallback:missing")

    def test_remove_getattr_after_ic_populated(self):
        """Removing __getattr__ from a class after the IC has cached lookups
        that used __getattr__ should invalidate the cache. Subsequent lookups
        for missing attributes should raise AttributeError."""

        class C:
            def __init__(self):
                self.x = 42

            def __getattr__(self, name):
                return f"fallback:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_x(o):
            return o.x

        @cinder_support.failUnlessJITCompiled
        def get_missing(o):
            return o.missing

        c = C()
        # Populate IC with __getattr__ active
        self.assertEqual(get_x(c), 42)
        self.assertEqual(get_x(c), 42)
        self.assertEqual(get_missing(c), "fallback:missing")
        self.assertEqual(get_missing(c), "fallback:missing")

        # Remove __getattr__
        del C.__getattr__

        # Existing attribute should still work
        self.assertEqual(get_x(c), 42)
        # Missing attribute should now raise
        with self.assertRaises(AttributeError):
            get_missing(c)

    def test_replace_getattr_after_ic_populated(self):
        """Replacing __getattr__ with a different implementation after the IC
        has cached should use the new implementation."""

        class C:
            def __init__(self):
                self.x = 42

            def __getattr__(self, name):
                return f"old:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_missing(o):
            return o.missing

        c = C()
        # Populate IC
        self.assertEqual(get_missing(c), "old:missing")
        self.assertEqual(get_missing(c), "old:missing")

        # Replace __getattr__
        C.__getattr__ = lambda self, name: f"new:{name}"

        self.assertEqual(get_missing(c), "new:missing")

    def test_add_getattr_to_base_after_ic_populated(self):
        """Adding __getattr__ to a base class after the IC has cached lookups
        on a derived class should invalidate the cache."""

        class Base:
            pass

        class Derived(Base):
            def __init__(self):
                self.x = 42

        @cinder_support.failUnlessJITCompiled
        def get_x(o):
            return o.x

        @cinder_support.failUnlessJITCompiled
        def get_missing(o):
            return o.missing

        d = Derived()
        # Populate IC
        self.assertEqual(get_x(d), 42)
        self.assertEqual(get_x(d), 42)
        with self.assertRaises(AttributeError):
            get_missing(d)

        # Add __getattr__ on the base class
        Base.__getattr__ = lambda self, name: f"base_fallback:{name}"

        # Existing attribute should still work
        self.assertEqual(get_x(d), 42)
        # Missing attribute should now use Base.__getattr__
        self.assertEqual(get_missing(d), "base_fallback:missing")

    def test_add_getattr_with_data_descriptor(self):
        """Adding __getattr__ to a class that has a data descriptor cached
        in the IC should work correctly. If the descriptor raises
        AttributeError, __getattr__ should be used as fallback."""

        class RaisingDescr:
            def __get__(self, obj, cls):
                raise AttributeError("descr error")

            def __set__(self, obj, val):
                pass

        class C:
            x = RaisingDescr()

        @cinder_support.failUnlessJITCompiled
        def get_x(o):
            return o.x

        c = C()
        # Without __getattr__, descriptor error propagates
        with self.assertRaises(AttributeError):
            get_x(c)

        # Add __getattr__
        C.__getattr__ = lambda self, name: f"fallback:{name}"

        # Now the descriptor error should be caught and __getattr__ called
        self.assertEqual(get_x(c), "fallback:x")
        self.assertEqual(get_x(c), "fallback:x")

    def test_remove_getattr_with_data_descriptor(self):
        """Removing __getattr__ from a class that has a data descriptor that
        raises AttributeError. After removal, the error should propagate."""

        class RaisingDescr:
            def __get__(self, obj, cls):
                raise AttributeError("descr error")

            def __set__(self, obj, val):
                pass

        class C:
            x = RaisingDescr()

            def __getattr__(self, name):
                return f"fallback:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_x(o):
            return o.x

        c = C()
        # With __getattr__, descriptor error is caught
        self.assertEqual(get_x(c), "fallback:x")
        self.assertEqual(get_x(c), "fallback:x")

        # Remove __getattr__
        del C.__getattr__

        # Now the descriptor error should propagate
        with self.assertRaises(AttributeError):
            get_x(c)

    def test_add_getattr_with_instance_dict_miss(self):
        """Adding __getattr__ after the IC has cached split dict lookups
        where the attribute may not be in the instance dict."""

        class C:
            pass

        @cinder_support.failUnlessJITCompiled
        def get_foo(o):
            return o.foo

        c = C()
        # Populate IC - attribute is missing, raises
        with self.assertRaises(AttributeError):
            get_foo(c)

        # Add the attribute to the instance
        # pyrefly: ignore  no foo
        c.foo = 100
        self.assertEqual(get_foo(c), 100)

        # Remove instance attribute and add __getattr__
        # pyrefly: ignore  no foo
        del c.foo
        C.__getattr__ = lambda self, name: f"dynamic:{name}"

        self.assertEqual(get_foo(c), "dynamic:foo")

    def test_add_getattribute_invalidates_getattr_ic(self):
        """Adding a custom __getattribute__ to a class that has __getattr__
        cached in the IC should invalidate the cache. The hookUsesGenericGetAttr
        check at fill time should reject re-caching with the custom
        __getattribute__."""

        class C:
            def __init__(self):
                self.x = 42

            def __getattr__(self, name):
                return f"fallback:{name}"

        @cinder_support.failUnlessJITCompiled
        def get_x(o):
            return o.x

        @cinder_support.failUnlessJITCompiled
        def get_missing(o):
            return o.missing

        c = C()
        # Populate IC
        self.assertEqual(get_x(c), 42)
        self.assertEqual(get_missing(c), "fallback:missing")

        # Add custom __getattribute__ - this changes the semantics
        def custom_getattribute(self, name):
            return f"custom:{name}"

        C.__getattribute__ = custom_getattribute

        # Now all attribute access should go through custom __getattribute__
        self.assertEqual(get_x(c), "custom:x")
        self.assertEqual(get_missing(c), "custom:missing")

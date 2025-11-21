# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import sys
import unittest

from textwrap import dedent

import cinderx
import cinderx.jit
import cinderx.test_support as cinder_support
from cinderx.test_support import passIf, passUnless

from .common import failUnlessHasOpcodes

if cinderx.is_initialized():
    from .test_compiler.test_strict.test_loader import base_sandbox, sandbox

AT_LEAST_312: bool = sys.version_info[:2] >= (3, 12)


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
                "LoadModuleAttrCached" if AT_LEAST_312 else "LoadModuleMethodCached",
                cinderx.jit.get_function_hir_opcode_counts(tmp_b.test),
            )

            # pyre-ignore[21]: Dynamically generated as part of this test.
            import tmp_a

            tmp_a.get_a = lambda: 10
            self.assertEqual(tmp_b.test(), 10)
            delattr(tmp_a, "get_a")
            with self.assertRaises(AttributeError):
                tmp_b.test()

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
        cinderx.jit.force_compile(tmp_b.test)
        self.assertTrue(cinderx.jit.is_jit_compiled(tmp_b.test))
        self.assertIn(
            "LoadModuleAttrCached" if AT_LEAST_312 else "LoadModuleMethodCached",
            cinderx.jit.get_function_hir_opcode_counts(tmp_b.test),
        )
        # prime the cache
        self.assertEqual(tmp_b.test(), 3)
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
        obj2.bar = 300
        # At this point the dictionary should still be split
        obj3 = Base(400)
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

    @passIf(
        cinderx.jit.is_enabled() and AT_LEAST_312,
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
        self.assertEqual(obj1.foo, 300)

        set_foo(obj, 400)
        self.assertEqual(obj1.foo, 300)

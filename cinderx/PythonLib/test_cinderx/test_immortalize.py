# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe

import gc
import os
import sys
import unittest

# pyre-ignore
# this must be in globals for JIT to see
from re._constants import BIGCHARSET

import cinderx
from cinderx.test_support import passUnless, run_in_subprocess, skip_if_ft

_PY_DEBUG_BUILD = hasattr(sys, "gettotalrefcount")
_PY312_BUILD = sys.version_info[:2] == (3, 12)


@skip_if_ft("T251571267: Free threading doesn't support heap immortalization")
@passUnless(cinderx.is_initialized(), "Tests immortalization APIs in CinderX")
class ImmortalizeTests(unittest.TestCase):
    def test_default_not_immortal(self) -> None:
        obj = []
        self.assertFalse(cinderx.is_immortal(obj))

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_is_immortal(self) -> None:
        obj = []
        cinderx.immortalize_heap()
        self.assertTrue(cinderx.is_immortal(obj))

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_post_immortalize(self) -> None:
        cinderx.immortalize_heap()
        obj = []
        self.assertFalse(cinderx.is_immortal(obj))

    @unittest.skipIf(
        _PY_DEBUG_BUILD,
        "Python 3.12 debug builds only allow interned immortal unicode",
    )
    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_exact_dict_unicode_keys(self) -> None:
        key = "".join(("qe2_", "param"))
        value = object()
        mapping = {key: value}
        holder = [mapping]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(mapping))
        self.assertTrue(cinderx.is_immortal(key))
        self.assertTrue(cinderx.is_immortal(value))

    @unittest.skipIf(
        _PY_DEBUG_BUILD,
        "Python 3.12 debug builds only allow interned immortal unicode",
    )
    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_gc_collected_exact_dict_entries(self) -> None:
        key = "".join(("gc_collected_", "param"))
        value = object()
        mapping = {key: value}
        gc.collect()
        self.assertEqual(not _PY312_BUILD, gc.is_tracked(mapping))

        holder = [mapping]
        self.assertEqual(not _PY312_BUILD, gc.is_tracked(mapping))
        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(mapping))
        self.assertTrue(cinderx.is_immortal(key))
        self.assertTrue(cinderx.is_immortal(value))

    @unittest.skipIf(
        _PY_DEBUG_BUILD,
        "Python 3.12 debug builds only allow interned immortal unicode",
    )
    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_nested_exact_dict_entries(self) -> None:
        outer_key = "".join(("outer_", "param"))
        inner_key = "".join(("inner_", "param"))
        inner_value = object()
        inner = {inner_key: inner_value}
        outer = {outer_key: inner}
        holder = [outer]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(outer))
        self.assertTrue(cinderx.is_immortal(outer_key))
        self.assertTrue(cinderx.is_immortal(inner))
        self.assertTrue(cinderx.is_immortal(inner_key))
        self.assertTrue(cinderx.is_immortal(inner_value))

    @unittest.skipIf(
        _PY_DEBUG_BUILD,
        "Python 3.12 debug builds only allow interned immortal unicode",
    )
    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_self_referential_exact_dict_entries(self) -> None:
        key = "".join(("self_", "param"))
        value = object()
        self_key = "".join(("self_", "ref"))
        mapping = {key: value}
        mapping[self_key] = mapping
        holder = [mapping]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(mapping))
        self.assertTrue(cinderx.is_immortal(key))
        self.assertTrue(cinderx.is_immortal(value))
        self.assertTrue(cinderx.is_immortal(self_key))
        self.assertIs(mapping[self_key], mapping)

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_code_consts_entries(self) -> None:
        def target() -> None:
            return None

        const = object()
        consts = (const,)
        target.__code__ = target.__code__.replace(co_consts=consts)
        holder = [target]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(target.__code__))
        self.assertTrue(cinderx.is_immortal(consts))
        self.assertTrue(cinderx.is_immortal(const))

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_code_consts_tuple_entry(self) -> None:
        def target() -> None:
            return None

        const = object()
        tuple_const = (const,)
        consts = (tuple_const,)
        target.__code__ = target.__code__.replace(co_consts=consts)
        holder = [target]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(target.__code__))
        self.assertTrue(cinderx.is_immortal(consts))
        self.assertTrue(cinderx.is_immortal(tuple_const))

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_code_consts_nested_exact_dict_entries(self) -> None:
        def target() -> None:
            return None

        key = object()
        value = object()
        mapping = {key: value}
        consts = (mapping,)
        target.__code__ = target.__code__.replace(co_consts=consts)
        holder = [target]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(target.__code__))
        self.assertTrue(cinderx.is_immortal(consts))
        self.assertTrue(cinderx.is_immortal(mapping))
        self.assertTrue(cinderx.is_immortal(key))
        self.assertTrue(cinderx.is_immortal(value))

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_nested_code_object_consts(self) -> None:
        def inner() -> None:
            return None

        def outer() -> None:
            return None

        const = object()
        inner_code = inner.__code__.replace(co_consts=(const,))
        outer.__code__ = outer.__code__.replace(co_consts=(inner_code,))
        holder = [outer]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(outer.__code__))
        self.assertTrue(cinderx.is_immortal(inner_code))
        self.assertTrue(cinderx.is_immortal(inner_code.co_consts))
        self.assertTrue(cinderx.is_immortal(const))

    @unittest.skipIf(
        _PY_DEBUG_BUILD,
        "Python 3.12 debug builds only allow interned immortal unicode",
    )
    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_code_name_tuple_entries(self) -> None:
        def target() -> None:
            return None

        name = "".join(("dynamic_", "name"))
        local_name = "".join(("dynamic_", "local"))
        target.__code__ = target.__code__.replace(
            co_names=(name,),
            co_nlocals=1,
            co_varnames=(local_name,),
        )
        holder = [target]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(target.__code__))
        self.assertTrue(cinderx.is_immortal(target.__code__.co_names))
        self.assertTrue(cinderx.is_immortal(name))
        self.assertTrue(cinderx.is_immortal(local_name))

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_code_exceptiontable(self) -> None:
        def target() -> None:
            return None

        exceptiontable = bytes(bytearray((1, 2, 3)))
        target.__code__ = target.__code__.replace(
            co_exceptiontable=exceptiontable,
        )
        holder = [target]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(target.__code__))
        self.assertTrue(cinderx.is_immortal(exceptiontable))

    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_refcount(self) -> None:
        from cinderx import jit

        print(sys.getrefcount(BIGCHARSET))

        def f():
            x = BIGCHARSET
            return x

        jit.force_compile(f)
        cinderx.immortalize_heap()

        rc = sys.getrefcount(BIGCHARSET)
        x = f()
        self.assertEqual(rc, sys.getrefcount(BIGCHARSET))
        self.assertTrue(cinderx.is_immortal(x))

    @unittest.skipIf(
        _PY_DEBUG_BUILD,
        "Python 3.12 debug builds only allow interned immortal unicode",
    )
    @unittest.skipUnless(hasattr(os, "fork"), "fork not available on Windows")
    @run_in_subprocess
    def test_immortalize_code_qualname(self) -> None:
        def target() -> None:
            return None

        qualname = "".join(("dynamic_", "qualname"))
        target.__code__ = target.__code__.replace(co_qualname=qualname)
        holder = [target]

        cinderx.immortalize_heap()

        self.assertTrue(cinderx.is_immortal(holder))
        self.assertTrue(cinderx.is_immortal(target.__code__))
        self.assertTrue(cinderx.is_immortal(qualname))

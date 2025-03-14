# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# @pyre-unsafe

import builtins
import unittest
from textwrap import dedent

from cinderx.jit import (
    force_compile,
    is_enabled as is_jit_enabled,
    is_jit_compiled,
)
import cinderx.test_support as cinder_support
from cinderx.test_support import skip_unless_lazy_imports
from .common import failUnlessHasOpcodes, with_globals

try:
    # pyre-ignore[21]: Pyre doesn't know about cinderjit.
    import cinderjit
except ImportError:
    cinderjit = None


class LoadGlobalCacheTests(unittest.TestCase):
    def setUp(self):
        global license, a_global
        try:
            del license
        except NameError:
            pass
        try:
            del a_global
        except NameError:
            pass

    @staticmethod
    def set_global(value):
        global a_global
        a_global = value

    @staticmethod
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def get_global():
        return a_global

    @staticmethod
    def del_global():
        global a_global
        del a_global

    @staticmethod
    def set_license(value):
        global license
        license = value

    @staticmethod
    def del_license():
        global license
        del license

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def test_simple(self):
        global a_global
        self.set_global(123)
        self.assertEqual(a_global, 123)
        self.set_global(456)
        self.assertEqual(a_global, 456)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def test_shadow_builtin(self):
        self.assertIs(license, builtins.license)
        self.set_license(0xDEADBEEF)
        self.assertIs(license, 0xDEADBEEF)
        self.del_license()
        self.assertIs(license, builtins.license)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def test_shadow_fake_builtin(self):
        self.assertRaises(NameError, self.get_global)
        builtins.a_global = "poke"
        self.assertEqual(a_global, "poke")
        self.set_global("override poke")
        self.assertEqual(a_global, "override poke")
        self.del_global()
        self.assertEqual(a_global, "poke")
        # We don't support DELETE_ATTR yet.
        delattr(builtins, "a_global")
        self.assertRaises(NameError, self.get_global)

    class prefix_str(str):
        def __new__(ty, prefix, value):
            s = super().__new__(ty, value)
            s.prefix = prefix
            return s

        def __hash__(self):
            return hash(self.prefix + self)

        def __eq__(self, other):
            return (self.prefix + self) == other

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def test_weird_key_in_globals(self):
        global a_global
        self.assertRaises(NameError, self.get_global)
        globals()[self.prefix_str("a_glo", "bal")] = "a value"
        self.assertEqual(a_global, "a value")
        self.assertEqual(self.get_global(), "a value")

    class MyGlobals(dict):
        def __getitem__(self, key):
            if key == "knock_knock":
                return "who's there?"
            return super().__getitem__(key)

    @with_globals(MyGlobals())
    def return_knock_knock(self):
        return knock_knock

    def test_dict_subclass_globals(self):
        self.assertEqual(self.return_knock_knock(), "who's there?")

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def _test_unwatch_builtins(self):
        self.set_global("hey")
        self.assertEqual(self.get_global(), "hey")
        builtins.__dict__[42] = 42

    @cinder_support.runInSubprocess
    def test_unwatch_builtins(self):
        try:
            self._test_unwatch_builtins()
        finally:
            del builtins.__dict__[42]

    @skip_unless_lazy_imports
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    @cinder_support.runInSubprocess
    def test_preload_side_effect_modifies_globals(self):
        with cinder_support.temp_sys_path() as tmp:
            (tmp / "tmp_a.py").write_text(
                dedent(
                    """
                    import importlib
                    importlib.set_lazy_imports(True)
                    from tmp_b import B

                    A = 1

                    def get_a():
                        return A + B

                    """
                ),
                encoding="utf8",
            )
            (tmp / "tmp_b.py").write_text(
                dedent(
                    """
                    import tmp_a

                    tmp_a.A = 2

                    B = 3
                    """
                ),
                encoding="utf8",
            )
            if cinderjit:
                cinderjit.clear_runtime_stats()

            # pyre-ignore[21]: This file dynamically generated as part of this test.
            import tmp_a

            # Force the compilation if this is running with AutoJIT.
            force_compile(tmp_a.get_a)

            # What happens on the first call is kinda undefined in principle
            # given lazy imports; somebody could previously have imported B
            # (not in this specific test, but in principle), or not, so the
            # first call might return 4 or 5. With JIT compilation it will
            # always return 5 because compilation will trigger the lazy import
            # and its side effect. Without the JIT it will return 4 in this
            # test, but we consider this an acceptable side effect of JIT
            # compilation because this code can't in general rely on B never
            # having previously been imported.
            tmp_a.get_a()

            # On the second call the result should undoubtedly be 5 in all
            # circumstances. Even if we compile with the wrong value for A, the
            # guard on the LoadGlobalCached will ensure we deopt and return the
            # right result.
            self.assertEqual(tmp_a.get_a(), 5)
            if cinderjit:
                self.assertTrue(is_jit_compiled(tmp_a.get_a))
                # The real test here is what when the value of a global changes
                # during compilation preload (as it does in this test because
                # the preload bytescan of get_a() first hits A, loads the old
                # value, then hits B, triggers the lazy import and imports
                # tmp_b, causing the value of A to change), we still have time
                # to compile with the correct (new) value and avoid compiling
                # code that will inevitably deopt, and so we should.
                stats = cinderjit.get_and_clear_runtime_stats()
                relevant_deopts = [
                    d for d in stats["deopt"] if d["normal"]["func_qualname"] == "get_a"
                ]
                self.assertEqual(relevant_deopts, [])

    @skip_unless_lazy_imports
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    @cinder_support.runInSubprocess
    def test_preload_side_effect_makes_globals_unwatchable(self):
        with cinder_support.temp_sys_path() as tmp:
            (tmp / "tmp_a.py").write_text(
                dedent(
                    """
                    import importlib
                    importlib.set_lazy_imports(True)
                    from tmp_b import B

                    A = 1

                    def get_a():
                        return A + B

                    """
                ),
                encoding="utf8",
            )
            (tmp / "tmp_b.py").write_text(
                dedent(
                    """
                    import tmp_a

                    tmp_a.__dict__[42] = 1
                    tmp_a.A = 2

                    B = 3
                    """
                ),
                encoding="utf8",
            )

            if cinderjit:
                cinderjit.clear_runtime_stats()
            import tmp_a

            # Force the compilation if this is running with AutoJIT.
            force_compile(tmp_a.get_a)

            tmp_a.get_a()
            self.assertEqual(tmp_a.get_a(), 5)
            self.assertTrue(not is_jit_enabled() or is_jit_compiled(tmp_a.get_a))

    @skip_unless_lazy_imports
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    @cinder_support.runInSubprocess
    def test_preload_side_effect_makes_builtins_unwatchable(self):
        with cinder_support.temp_sys_path() as tmp:
            (tmp / "tmp_a.py").write_text(
                dedent(
                    """
                    import importlib
                    importlib.set_lazy_imports(True)
                    from tmp_b import B

                    def get_a():
                        return max(1, 2) + B

                    """
                ),
                encoding="utf8",
            )
            (tmp / "tmp_b.py").write_text(
                dedent(
                    """
                    __builtins__[42] = 2

                    B = 3
                    """
                ),
                encoding="utf8",
            )
            if cinderjit:
                cinderjit.clear_runtime_stats()
            import tmp_a

            # Force the compilation if this is running with AutoJIT.
            force_compile(tmp_a.get_a)

            tmp_a.get_a()
            self.assertEqual(tmp_a.get_a(), 5)
            self.assertTrue(not is_jit_enabled() or is_jit_compiled(tmp_a.get_a))

    @skip_unless_lazy_imports
    @cinder_support.runInSubprocess
    def test_lazy_import_after_global_cached(self):
        with cinder_support.temp_sys_path() as tmp:
            (tmp / "tmp_a.py").write_text(
                dedent(
                    """
                    import importlib
                    importlib.set_lazy_imports(True)
                    from tmp_b import B

                    def f():
                        return B

                    for _ in range(51):
                        f()

                    from tmp_b import B
                    """
                )
            )
            (tmp / "tmp_b.py").write_text(
                dedent(
                    """
                    B = 3
                    """
                )
            )
            import tmp_a

            self.assertEqual(tmp_a.f(), 3)

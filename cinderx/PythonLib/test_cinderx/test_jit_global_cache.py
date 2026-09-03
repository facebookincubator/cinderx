# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# @pyre-unsafe

# pyre-unsafe

import builtins
import sys
import unittest
from textwrap import dedent

import cinderx.jit
import cinderx.test_support as cinder_support
from cinderx.test_support import run_in_subprocess, skip_unless_lazy_imports

from .common import failUnlessHasOpcodes, with_globals


# Initialized at module scope so they have a value when failUnlessJITCompiled
# compiles the readers below at decoration time. One global per test, because
# the guard is chosen from the type seen at compile time.
a_bool_global: bool = False
an_int_global: int = 0
a_retyped_global: int = 0


class LoadGlobalCacheTests(unittest.TestCase):
    def setUp(self):
        for name in (
            "a_global",
            "deleted_after_compile_global",
            "late_bound_builtin",
            "late_bound_global",
            "license",
        ):
            globals().pop(name, None)
        builtins.__dict__.pop("a_global", None)
        builtins.__dict__.pop("late_bound_builtin", None)

    def tearDown(self):
        self.setUp()

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
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def get_late_bound_global():
        return late_bound_global  # noqa: F821

    @staticmethod
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def get_deleted_after_compile_global():
        return deleted_after_compile_global  # noqa: F821

    @staticmethod
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def get_late_bound_builtin():
        return late_bound_builtin  # noqa: F821

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

    def test_global_bound_after_compile(self):
        cinderx.jit.clear_runtime_stats()
        globals()["late_bound_global"] = 123

        self.assertEqual(self.get_late_bound_global(), 123)
        if cinderx.jit.is_enabled():
            self.assertEqual(self.deopt_count(self.get_late_bound_global), 0)

    def test_global_deleted_after_compile(self):
        globals()["deleted_after_compile_global"] = "present"
        self.assertEqual(self.get_deleted_after_compile_global(), "present")
        cinderx.jit.clear_runtime_stats()

        del globals()["deleted_after_compile_global"]
        with self.assertRaises(NameError):
            self.get_deleted_after_compile_global()
        if cinderx.jit.is_enabled():
            self.assertGreater(
                self.deopt_count(self.get_deleted_after_compile_global), 0
            )

    def test_builtin_fallback_and_module_shadow_after_compile(self):
        cinderx.jit.clear_runtime_stats()
        builtins.late_bound_builtin = "builtin"
        self.assertEqual(self.get_late_bound_builtin(), "builtin")

        globals()["late_bound_builtin"] = "module"
        self.assertEqual(self.get_late_bound_builtin(), "module")

        del globals()["late_bound_builtin"]
        self.assertEqual(self.get_late_bound_builtin(), "builtin")
        if cinderx.jit.is_enabled():
            self.assertEqual(self.deopt_count(self.get_late_bound_builtin), 0)

        del builtins.late_bound_builtin
        with self.assertRaises(NameError):
            self.get_late_bound_builtin()
        if cinderx.jit.is_enabled():
            self.assertGreater(self.deopt_count(self.get_late_bound_builtin), 0)

    @staticmethod
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def read_bool_global(n):
        total = 0
        for _ in range(n):
            if a_bool_global:
                total += 1
        return total

    @staticmethod
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def read_int_global(n):
        total = 0
        for _ in range(n):
            total += an_int_global
        return total

    @staticmethod
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def read_retyped_global(n):
        total = 0
        for _ in range(n):
            total += a_retyped_global
        return total

    def deopt_count(self, func):
        stats = cinderx.jit.get_and_clear_runtime_stats()
        return sum(
            d["int"]["count"]
            for d in stats.get("deopt", ())
            if d["normal"]["func_qualname"] == func.__qualname__
        )

    # A LOAD_GLOBAL guards a data global's type rather than pinning its value,
    # so rebinding one is not supposed to invalidate compiled code. Rebinding a
    # module-level flag or counter is ordinary Python.
    def test_rebinding_bool_global_does_not_deopt(self):
        global a_bool_global

        cinderx.jit.clear_runtime_stats()
        for _ in range(50):
            a_bool_global = True
            self.assertEqual(self.read_bool_global(4), 4)
            a_bool_global = False
            self.assertEqual(self.read_bool_global(4), 0)

        self.assertEqual(self.deopt_count(self.read_bool_global), 0)

    def test_rebinding_int_global_does_not_deopt(self):
        global an_int_global

        cinderx.jit.clear_runtime_stats()
        for i in range(50):
            an_int_global = i
            self.assertEqual(self.read_int_global(3), i * 3)

        self.assertEqual(self.deopt_count(self.read_int_global), 0)

    # The type guard is what makes pinning the value unnecessary, so swapping in
    # a value of a different type still has to fall back to the interpreter.
    def test_changing_global_type_deopts(self):
        global a_retyped_global

        try:
            a_retyped_global = 1
            self.assertEqual(self.read_retyped_global(2), 2)
            cinderx.jit.clear_runtime_stats()

            a_retyped_global = 1.5
            self.assertEqual(self.read_retyped_global(2), 3.0)
            if cinderx.jit.is_enabled():
                self.assertGreater(self.deopt_count(self.read_retyped_global), 0)
        finally:
            a_retyped_global = 0

    class prefix_str(str):
        def __new__(cls, prefix, value):
            s = super().__new__(cls, value)
            s.prefix = prefix
            return s

        def __hash__(self):
            return hash(self.prefix + self)

        def __eq__(self, other):
            return (self.prefix + self) == other

    # Runs in a subprocess because the weird key is never removed.  CPython converts a
    # dict's keys to the general kind on a non-exact-str insert and never converts back,
    # so this module's globals stay unwatchable for the rest of the process.  That nulls
    # every global cache pointing at them and deopts every LOAD_GLOBAL here, which
    # breaks the other tests.
    @run_in_subprocess
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def test_weird_key_in_globals(self):
        self.assertRaises(NameError, self.get_global)
        globals()[self.prefix_str("a_glo", "bal")] = "a value"
        self.assertEqual(a_global, "a value")
        self.assertEqual(self.get_global(), "a value")

    @run_in_subprocess
    def test_unwatchable_dict_uses_generic_lookup(self):
        globals()[self.prefix_str("unwatchable_", "key")] = None

        @failUnlessHasOpcodes("LOAD_GLOBAL")
        def get_unwatchable_global():
            return unwatchable_global  # noqa: F821

        if cinderx.jit.is_enabled():
            self.assertTrue(cinderx.jit.force_compile(get_unwatchable_global))

        globals()["unwatchable_global"] = "first"
        self.assertEqual(get_unwatchable_global(), "first")
        globals()["unwatchable_global"] = "second"
        self.assertEqual(get_unwatchable_global(), "second")

    class MyGlobals(dict):
        def __getitem__(self, key):
            if key == "knock_knock":
                return "who's there?"
            return super().__getitem__(key)

    @with_globals(MyGlobals())
    def return_knock_knock(self):
        return knock_knock  # noqa: F821

    def test_dict_subclass_globals(self):
        self.assertEqual(self.return_knock_knock(), "who's there?")

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def _test_unwatch_builtins(self):
        self.set_global("hey")
        self.assertEqual(self.get_global(), "hey")
        builtins.__dict__[42] = 42

    @run_in_subprocess
    def test_unwatch_builtins(self):
        try:
            self._test_unwatch_builtins()
        finally:
            del builtins.__dict__[42]

    @skip_unless_lazy_imports()
    @run_in_subprocess
    @failUnlessHasOpcodes("LOAD_GLOBAL")
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
            cinderx.jit.clear_runtime_stats()

            import tmp_a

            # Force the compilation if this is running with AutoJIT.
            cinderx.jit.force_compile(tmp_a.get_a)

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
            if cinderx.jit.is_enabled():
                self.assertTrue(cinderx.jit.is_jit_compiled(tmp_a.get_a))
                # The real test here is what when the value of a global changes
                # during compilation preload (as it does in this test because
                # the preload bytescan of get_a() first hits A, loads the old
                # value, then hits B, triggers the lazy import and imports
                # tmp_b, causing the value of A to change), we still have time
                # to compile with the correct (new) value and avoid compiling
                # code that will inevitably deopt, and so we should.
                stats = cinderx.jit.get_and_clear_runtime_stats()
                relevant_deopts = [
                    d for d in stats["deopt"] if d["normal"]["func_qualname"] == "get_a"
                ]
                self.assertEqual(relevant_deopts, [])

    @skip_unless_lazy_imports()
    @run_in_subprocess
    @failUnlessHasOpcodes("LOAD_GLOBAL")
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

            cinderx.jit.clear_runtime_stats()
            import tmp_a

            # Force the compilation if this is running with AutoJIT.
            cinderx.jit.force_compile(tmp_a.get_a)

            tmp_a.get_a()
            self.assertEqual(tmp_a.get_a(), 5)
            self.assertTrue(
                not cinderx.jit.is_enabled() or cinderx.jit.is_jit_compiled(tmp_a.get_a)
            )

    @skip_unless_lazy_imports()
    @run_in_subprocess
    @failUnlessHasOpcodes("LOAD_GLOBAL")
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
            cinderx.jit.clear_runtime_stats()
            import tmp_a

            # Force the compilation if this is running with AutoJIT.
            cinderx.jit.force_compile(tmp_a.get_a)

            tmp_a.get_a()
            self.assertEqual(tmp_a.get_a(), 5)
            self.assertTrue(
                not cinderx.jit.is_enabled() or cinderx.jit.is_jit_compiled(tmp_a.get_a)
            )

    @skip_unless_lazy_imports()
    @run_in_subprocess
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

    @skip_unless_lazy_imports()
    @unittest.skipUnless(sys.version_info >= (3, 15), "requires Python 3.15")
    @run_in_subprocess
    @failUnlessHasOpcodes("LOAD_GLOBAL")
    def test_lazy_import_binds_global_after_compile(self):
        with cinder_support.temp_sys_path() as tmp:
            (tmp / "tmp_lazy_binding_a.py").write_text(
                dedent(
                    """
                    import cinderx.jit
                    import importlib
                    importlib.set_lazy_imports(True)

                    def get_b():
                        return B

                    def get_raw_b():
                        return globals()["B"]

                    cinderx.jit.force_compile(get_b)
                    from tmp_lazy_binding_b import B
                    """
                ),
                encoding="utf8",
            )
            (tmp / "tmp_lazy_binding_b.py").write_text(
                dedent(
                    """
                    B = 3
                    """
                ),
                encoding="utf8",
            )

            import tmp_lazy_binding_a

            self.assertEqual(
                type(tmp_lazy_binding_a.get_raw_b()).__name__, "lazy_import"
            )
            self.assertEqual(tmp_lazy_binding_a.get_b(), 3)
            if cinderx.jit.is_enabled():
                self.assertTrue(cinderx.jit.is_jit_compiled(tmp_lazy_binding_a.get_b))

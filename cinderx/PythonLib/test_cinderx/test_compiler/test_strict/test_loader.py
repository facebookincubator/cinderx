# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe
from __future__ import annotations

import _imp
import dis
import gc
import importlib
import io
import os
import pathlib
import subprocess
import sys
import tempfile
import textwrap
import unittest
from collections.abc import Callable, Generator, Sequence
from contextlib import contextmanager
from importlib.abc import Loader
from importlib.machinery import SOURCE_SUFFIXES, SourceFileLoader
from importlib.util import cache_from_source
from os import path
from py_compile import PycInvalidationMode
from types import ModuleType
from typing import cast, final, List, TYPE_CHECKING, TypeVar
from unittest.mock import patch

from cinderx.compiler.strict.loader import (
    _MAGIC_LEN,
    _MAGIC_NEITHER_STRICT_NOR_STATIC,
    _MAGIC_STRICT_OR_STATIC,
    install,
    strict_compile,
    StrictModule,
    StrictModuleTestingPatchProxy,
    StrictSourceFileLoader,
)
from cinderx.compiler.strict.runtime import set_freeze_enabled
from cinderx.static import StaticTypeError
from cinderx.test_support import passIf, subprocess_env

from . import sandbox as base_sandbox
from .common import init_cached_properties, StrictTestBase
from .sandbox import (
    file_loader,
    on_sys_path,
    restore_static_symtable,
    restore_strict_modules,
    restore_sys_modules,
)

try:
    # pyre-ignore[21]: cinder module not typed.
    from cinder import cinder_set_warn_handler, get_warn_handler

    HAVE_WARN_HANDLERS: bool = True
except ImportError:

    def cinder_set_warn_handler(func):
        pass

    def get_warn_handler():
        return None

    HAVE_WARN_HANDLERS: bool = False


if TYPE_CHECKING:
    # Code that dynamically passes around module objects is hard to type, since
    # modules can have any attribute of any type and Pyre doesn't know anything
    # about it. This fake type makes it less painful while also requiring fewer
    # warnings about using Any.
    @final
    class TModule(ModuleType):
        def __getattr__(self, name: str) -> int: ...


NORMAL_LOADER: tuple[type[SourceFileLoader], list[str]] = (
    SourceFileLoader,
    SOURCE_SUFFIXES,
)


STRICT_LOADER: tuple[Callable[[str, str], Loader], list[str]] = (
    lambda fullname, path: StrictSourceFileLoader(
        fullname,
        path,
        sys.path,
        init_cached_properties=init_cached_properties,
    ),
    SOURCE_SUFFIXES,
)

STRICT_LOADER_ENABLE_PATCHING: tuple[Callable[[str, str], Loader], list[str]] = (
    lambda fullname, path: StrictSourceFileLoader(
        fullname, path, sys.path, enable_patching=True
    ),
    SOURCE_SUFFIXES,
)


STRICT_LOADER_ENABLE_IMPORT_CALL_TRACKING: tuple[
    Callable[[str, str], Loader], list[str]
] = (
    lambda fullname, path: StrictSourceFileLoader(
        fullname,
        path,
        sys.path,
    ),
    SOURCE_SUFFIXES,
)


class AlwaysStrictSourceFileLoader(StrictSourceFileLoader):
    def should_force_strict(self) -> bool:
        return True


STRICT_LOADER_ALWAYS_STRICT: tuple[type[SourceFileLoader], list[str]] = (
    AlwaysStrictSourceFileLoader,
    SOURCE_SUFFIXES,
)


ALLOW_LIST = ["_collections_abc"]


@contextmanager
def write_bytecode(enable: bool = True) -> Generator[None, None, None]:
    orig = sys.dont_write_bytecode
    sys.dont_write_bytecode = not enable
    try:
        yield
    finally:
        sys.dont_write_bytecode = orig


@contextmanager
def ensure_type_patch(enabled: bool = True) -> Generator[None, None, None]:
    prev = set_freeze_enabled(enabled)
    try:
        yield
    finally:
        set_freeze_enabled(prev)


@contextmanager
def with_warn_handler() -> Generator[Sequence[tuple[object, ...]], None, None]:
    warnings: list[tuple[object, ...]] = []

    def warn(*args: object) -> None:
        warnings.append(args)

    prev = get_warn_handler()
    cinder_set_warn_handler(warn)
    try:
        yield warnings
    finally:
        cinder_set_warn_handler(prev)


# pyre-fixme[24]: Generic type `Callable` expects 2 type parameters.
TCallable = TypeVar("TCallable", bound=Callable)


@contextmanager
def callable_file_loader(
    loader: tuple[Callable[[str, str], Loader], list[str]],
) -> Generator[None, None, None]:
    # pyre-ignore[6]: Callable[[str, str], Loader] acts just like type[Loader], but they
    # are different types.
    with file_loader(loader):
        yield


class Sandbox(base_sandbox.Sandbox):
    @contextmanager
    def begin_loader(
        self, loader: tuple[Callable[[str, str], Loader], list[str]]
    ) -> Generator[None, None, None]:
        with (
            callable_file_loader(loader),
            restore_sys_modules(),
            restore_strict_modules(),
        ):
            yield

    def strict_import(self, *module_names: str) -> TModule | list[TModule]:
        """Import and return module(s) from sandbox (with strict module loader installed).

        Leaves no trace on sys.modules; every import is independent.
        """
        with callable_file_loader(STRICT_LOADER):
            return self._import(*module_names)

    def strict_import_patching_enabled(
        self, *module_names: str
    ) -> TModule | list[TModule]:
        """Same as strict_import but with patching enabled."""
        with callable_file_loader(STRICT_LOADER_ENABLE_PATCHING):
            return self._import(*module_names)

    def normal_import(self, *module_names: str) -> TModule | list[TModule]:
        """Import and return module(s) from sandbox (without strict module loader).

        Leaves no trace on sys.modules.
        """
        with callable_file_loader(NORMAL_LOADER):
            return self._import(*module_names)

    def _import(self, *module_names: str) -> TModule | list[TModule]:
        with restore_sys_modules(), restore_strict_modules():
            return self.import_modules(*module_names)

    def import_modules(self, *module_names: str) -> TModule | list[TModule]:
        for mod_name in module_names:
            __import__(mod_name)

        mods = [sys.modules[mod_name] for mod_name in module_names]
        return (
            cast("TModule", mods[0]) if len(mods) == 1 else cast("List[TModule]", mods)
        )

    @contextmanager
    def isolated_strict_loader(self) -> Generator[None, None, None]:
        with (
            callable_file_loader(STRICT_LOADER),
            restore_sys_modules(),
            restore_strict_modules(),
        ):
            yield

    @contextmanager
    def in_strict_module(
        self, *module_names: str
    ) -> Generator[TModule | list[TModule], None, None]:
        with self.isolated_strict_loader():
            yield self.import_modules(*module_names)

    def strict_from_code(self, code: str) -> TModule | list[TModule]:
        """Convenience wrapper to go direct from code to module via strict loader."""
        self.write_file("testmodule.py", code)
        return self.strict_import("testmodule")

    @contextmanager
    def with_strict_patching(self, value: bool = True) -> Generator[None, None, None]:
        with self.begin_loader(
            STRICT_LOADER_ENABLE_PATCHING if value else STRICT_LOADER
        ):
            yield

    @contextmanager
    def with_import_call_tracking(
        self, value: bool = True
    ) -> Generator[None, None, None]:
        with self.begin_loader(
            STRICT_LOADER_ENABLE_IMPORT_CALL_TRACKING if value else STRICT_LOADER
        ):
            yield


@contextmanager
def sandbox() -> Generator[Sandbox, None, None]:
    with base_sandbox.sandbox(Sandbox) as sbx:
        with write_bytecode(), on_sys_path(str(sbx.root)):
            yield sbx


@final
class StrictLoaderInstallTest(StrictTestBase):
    def test_install(self) -> None:
        with callable_file_loader(NORMAL_LOADER):
            orig_hooks_len = len(sys.path_hooks)
            install()
            self.assertEqual(len(sys.path_hooks), orig_hooks_len + 1)


@final
class StrictLoaderTest(StrictTestBase):
    ONCALL_SHORTNAME = "strictmod"

    def setUp(self) -> None:
        # TODO: loader test should also clear classlaoder caches
        self.sbx = base_sandbox.use_cm(sandbox, self)

    def test_ok_strict(self) -> None:
        mod = self.sbx.strict_from_code("import __strict__\nx = 2")
        self.assertEqual(mod.x, 2)
        self.assertIsNotNone(mod.__strict__)
        self.assertEqual(type(mod), StrictModule)

    def test_bad_not_strict(self) -> None:
        mod = self.sbx.strict_from_code('exec("a=2")')
        self.assertEqual(mod.a, 2)

    def test_forced_strict(self) -> None:
        self.sbx.write_file("a.py", "x = 2")
        with callable_file_loader(STRICT_LOADER_ALWAYS_STRICT):
            mod = self.sbx._import("a")
        self.assertEqual(mod.x, 2)
        self.assertEqual(type(mod), StrictModule)

    def test_package_over_module(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __static__
                from b import g
                def f() -> int:
                    return g()
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __static__

                def g() -> str:
                    return "foo"
            """,
        )
        self.sbx.write_file(
            "b/__init__.py",
            """
                import __static__

                def g() -> int:
                    return 42
            """,
        )
        with self.sbx.in_strict_module("a") as a:
            self.assertInBytecode(a.f, "INVOKE_FUNCTION", ((("a",), "g"), 0))
            self.assertEqual(a.f(), 42)

    @passIf(not hasattr(importlib, "set_lazy_imports"), "not supported w/ lazy imports")
    def test_with_lazy_imports_failed_invoke(self) -> None:
        # pyre-ignore[16]: no such attribute
        enabled = importlib.is_lazy_imports_enabled()
        try:
            if not enabled:
                # pyre-ignore[16]: no such attribute
                importlib.set_lazy_imports(True)
            self.sbx.write_file(
                "a.py",
                """
                    import __static__
                    from b import g
                    def f() -> int:
                        global g
                        res = g()
                        del g
                        return res

                    def f1() -> int:
                        return g()
                """,
            )
            self.sbx.write_file(
                "b.py",
                """
                    import __static__
                    def g() -> int:
                        return 42
                """,
            )
            with self.sbx.in_strict_module("a") as a:
                self.assertInBytecode(a.f, "INVOKE_FUNCTION", ((("a",), "g"), 0))
                self.assertEqual(a.f(), 42)
                with self.assertRaisesRegex(
                    StaticTypeError, ".*has been deleted from container, original was.*"
                ):
                    self.assertEqual(a.f1(), 42)
        finally:
            if not enabled:
                # pyre-ignore[16]: no such attribute
                importlib.set_lazy_imports(False)

    def test_strict_second_import(self) -> None:
        """Second import of unmodified strict module (from pyc) is still strict."""
        self.sbx.write_file("a.py", "import __strict__\nx = 2")
        mod1 = self.sbx.strict_import("a")
        # patch source_to_code on the loader to ensure we are loading from pyc
        with patch.object(
            StrictSourceFileLoader, "source_to_code", lambda *a, **kw: None
        ):
            mod2 = self.sbx.strict_import("a")

        self.assertEqual(type(mod1), StrictModule)
        self.assertEqual(type(mod2), StrictModule)

    def test_static_dependency_pyc_invalidation(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __static__
                from b import C
                def g():
                    return C()
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __static__
                def C():
                    return 42
            """,
        )
        with self.sbx.in_strict_module("a") as a1:
            self.assertEqual(a1.g(), 42)
            self.assertInBytecode(a1.g, "INVOKE_FUNCTION", ((("a",), "C"), 0))
        # ensure pycs exist and we can import from them
        with (
            patch.object(
                StrictSourceFileLoader, "source_to_code", lambda *a, **kw: None
            ),
            self.sbx.in_strict_module("a") as a2,
        ):
            self.assertInBytecode(a2.g, "INVOKE_FUNCTION", ((("a",), "C"), 0))
            self.assertEqual(a2.g(), 42)
        # modify dependency, but not module a
        self.sbx.write_file(
            "b.py",
            """
                import __static__
                class C:
                    pass
            """,
        )
        # if we use the previous bytecode for 'a', it will include an
        # INVOKE_FUNCTION, which is no longer correct
        with self.sbx.in_strict_module("a") as a3:
            self.assertInBytecode(a3.g, "TP_ALLOC", ("b", "C", "!"))
            self.assertIsInstance(a3.g(), a3.C)

    def test_static_dependency_on_nonstatic_pyc_invalidation(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __static__
                from b import f
                def g():
                    return f()
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                def f():
                    return 42
            """,
        )
        with self.sbx.in_strict_module("a") as a1:
            self.assertEqual(a1.g(), 42)
            self.assertInBytecode(a1.g, self.CALL, 0)
        # ensure pycs exist and we can import from them
        with (
            patch.object(
                StrictSourceFileLoader, "source_to_code", lambda *a, **kw: None
            ),
            self.sbx.in_strict_module("a") as a2,
        ):
            self.assertInBytecode(a2.g, self.CALL, 0)
            self.assertEqual(a2.g(), 42)
        # modify dependency, but not module a
        self.sbx.write_file(
            "b.py",
            """
                import __static__
                def f():
                    return 43
            """,
        )
        # bytecode for 'a' should now have an INVOKE_FUNCTION
        with self.sbx.in_strict_module("a") as a3:
            self.assertInBytecode(a3.g, "INVOKE_FUNCTION", ((("a",), "f"), 0))
            self.assertEqual(a3.g(), 43)

    def test_static_dependency_deleted_pyc_invalidation(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __static__
                def g():
                    from b import f
                    return f()
            """,
        )
        b_path = self.sbx.write_file(
            "b.py",
            """
                import __static__
                def f():
                    return 42
            """,
        )
        with self.sbx.in_strict_module("a") as a1:
            self.assertEqual(a1.g(), 42)
            self.assertInBytecode(a1.g, "INVOKE_FUNCTION", ((("b",), "f"), 0))
        # ensure pycs exist and we can import from them
        with (
            patch.object(
                StrictSourceFileLoader, "source_to_code", lambda *a, **kw: None
            ),
            self.sbx.in_strict_module("a") as a2,
        ):
            self.assertInBytecode(a2.g, "INVOKE_FUNCTION", ((("b",), "f"), 0))
            self.assertEqual(a2.g(), 42)
        b_path.unlink()
        # bytecode for 'a' should now have CALL_FUNCTION instead
        with self.sbx.in_strict_module("a") as a3:
            self.assertInBytecode(a3.g, self.CALL, 0)
            # will fail because module b is gone
            with self.assertRaises(ModuleNotFoundError):
                a3.g()

    def test_strict_compile(self) -> None:
        fn = self.sbx.write_file("a.py", "import __strict__\nx = 2")
        strict_compile(str(fn), cache_from_source(fn))

        # patch source_to_code on the loader to ensure we are loading from pyc
        with patch.object(
            StrictSourceFileLoader, "source_to_code", lambda *a, **kw: None
        ):
            mod = self.sbx.strict_import("a")

        self.assertEqual(type(mod), StrictModule)

    def test_hash_based_pyc_dependency_invalidation(self) -> None:
        a_path = str(
            self.sbx.write_file(
                "a.py",
                """
                    import __static__
                    from b import C
                    def g():
                        return C()
                """,
            )
        )
        b_path = str(
            self.sbx.write_file(
                "b.py",
                """
                    import __static__
                    def C():
                        return 42
                """,
            )
        )
        strict_compile(
            b_path,
            cache_from_source(b_path),
            invalidation_mode=PycInvalidationMode.CHECKED_HASH,
        )
        strict_compile(
            a_path,
            cache_from_source(a_path),
            invalidation_mode=PycInvalidationMode.CHECKED_HASH,
        )
        # ensure pycs exist and we can import from them
        with (
            patch.object(
                StrictSourceFileLoader, "source_to_code", lambda *a, **kw: None
            ),
            self.sbx.in_strict_module("a") as a2,
        ):
            self.assertInBytecode(a2.g, "INVOKE_FUNCTION", ((("a",), "C"), 0))
            self.assertEqual(a2.g(), 42)
        # modify dependency, but not module a
        self.sbx.write_file(
            "b.py",
            """
                import __static__
                class C:
                    pass
            """,
        )
        with self.sbx.in_strict_module("a") as a3:
            self.assertInBytecode(a3.g, "TP_ALLOC", ("b", "C", "!"))
            self.assertIsInstance(a3.g(), a3.C)

    def test_unchecked_hash_pyc_no_invalidate_dependency(self) -> None:
        a_path = str(
            self.sbx.write_file(
                "a.py",
                """
                    import __static__
                    from b import C
                    def g():
                        return C()
                """,
            )
        )
        b_path = str(
            self.sbx.write_file(
                "b.py",
                """
                    import __static__
                    def C():
                        return 42
                """,
            )
        )
        strict_compile(
            b_path,
            cache_from_source(b_path),
            invalidation_mode=PycInvalidationMode.UNCHECKED_HASH,
        )
        strict_compile(
            a_path,
            cache_from_source(a_path),
            invalidation_mode=PycInvalidationMode.UNCHECKED_HASH,
        )
        # ensure pycs exist and we can import from them
        with (
            patch.object(
                StrictSourceFileLoader, "source_to_code", lambda *a, **kw: None
            ),
            self.sbx.in_strict_module("a") as a2,
        ):
            self.assertInBytecode(a2.g, "INVOKE_FUNCTION", ((("a",), "C"), 0))
            self.assertEqual(a2.g(), 42)
        self.sbx.write_file(
            "b.py",
            """
                import __static__
                class C:
                    pass
            """,
        )
        # unchecked hash, so changes are not respected
        with self.sbx.in_strict_module("a") as a3:
            self.assertInBytecode(a3.g, "INVOKE_FUNCTION", ((("a",), "C"), 0))
            self.assertEqual(a3.g(), 42)

    def test_cached_attr(self) -> None:
        """__cached__ attribute of a strict or non-strict module is correct."""
        self.sbx.write_file("strict.py", "import __strict__\nx = 2")
        self.sbx.write_file("nonstrict.py", "x = 2")
        mod1 = self.sbx.strict_import("strict")
        mod2 = self.sbx.strict_import("nonstrict")
        mod3 = self.sbx.normal_import("nonstrict")
        mod4 = self.sbx.strict_import_patching_enabled("strict")

        # Strict module imported by strict loader should be .strict.pyc
        self.assertTrue(
            mod1.__spec__.cached.endswith(".strict.pyc"),
            f"'{mod1.__spec__.cached}' should end with .strict.pyc",
        )
        self.assertEqual(mod1.__spec__.cached, mod1.__spec__.cached)
        self.assertTrue(os.path.exists(mod1.__spec__.cached))

        # Non-strict module imported by strict loader should also have .strict!
        self.assertTrue(
            mod2.__spec__.cached.endswith(".strict.pyc"),
            f"'{mod2.__spec__.cached}' should end with .strict.pyc",
        )
        self.assertEqual(mod2.__spec__.cached, mod2.__spec__.cached)
        self.assertTrue(os.path.exists(mod2.__spec__.cached))

        # Module imported by non-strict loader should not have -strict
        self.assertFalse(
            mod3.__spec__.cached.endswith(".strict.pyc"),
            f"{mod3.__spec__.cached} should not contain .strict",
        )
        self.assertEqual(mod3.__spec__.cached, mod3.__spec__.cached)
        self.assertTrue(os.path.exists(mod3.__spec__.cached))

        # Strict module imported by strict loader with patching enabled
        self.assertTrue(
            mod4.__spec__.cached.endswith(".strict.patch.pyc"),
            f"'{mod4.__spec__.cached}' should end with .strict.patch.pyc",
        )
        self.assertEqual(mod4.__spec__.cached, mod4.__spec__.cached)
        self.assertTrue(os.path.exists(mod4.__spec__.cached))

    def test_builtins_modified(self) -> None:
        self.sbx.write_file(
            "strict.py", "import __strict__\nfrom dependency import abc"
        )
        with self.sbx.begin_loader(STRICT_LOADER):
            self.sbx.write_file("dependency.py", "import __strict__\nabc = 42")
            self.sbx.import_modules("dependency")

            __builtins__["abc"] = 42
            self.sbx.import_modules("strict")
            del __builtins__["abc"]
            del sys.modules["strict"]
            # should successfully import with `abc` no longer defined
            self.sbx.import_modules("strict")

    def test_magic_number(self) -> None:
        """Extra magic number is written to strict pycs, and validated."""
        self.sbx.write_file("a.py", "import __strict__\nx = 2")
        mod = self.sbx.strict_import("a")

        with open(mod.__spec__.cached, "rb") as fh:
            self.assertEqual(fh.read(_MAGIC_LEN), _MAGIC_STRICT_OR_STATIC)

        BAD_MAGIC = (65535).to_bytes(2, "little") + b"\r\n"

        with open(mod.__spec__.cached, "r+b") as fh:
            fh.write(BAD_MAGIC)

        # with bad magic number, file can still import and correct pyc is written
        mod2 = self.sbx.strict_import("a")

        with open(mod2.__spec__.cached, "rb") as fh:
            self.assertEqual(fh.read(_MAGIC_LEN), _MAGIC_STRICT_OR_STATIC)

    def test_magic_number_non_strict(self) -> None:
        """Extra magic number is written to non-strict pycs, and validated."""
        self.sbx.write_file("a.py", "x=2")
        mod = self.sbx.strict_import("a")

        with open(mod.__spec__.cached, "rb") as fh:
            self.assertEqual(fh.read(_MAGIC_LEN), _MAGIC_NEITHER_STRICT_NOR_STATIC)

        BAD_MAGIC = (65535).to_bytes(2, "little") + b"\r\n"

        with open(mod.__spec__.cached, "r+b") as fh:
            fh.write(BAD_MAGIC)

        # with bad magic number, file can still import and correct pyc is written
        mod2 = self.sbx.strict_import("a")

        with open(mod2.__spec__.cached, "rb") as fh:
            self.assertEqual(fh.read(_MAGIC_LEN), _MAGIC_NEITHER_STRICT_NOR_STATIC)

    def test_strict_loader_toggle(self) -> None:
        """Repeat imports with strict module loader toggled off/on/off work correctly."""
        self.sbx.write_file("a.py", "import __strict__\nx = 2")
        mod1 = self.sbx.normal_import("a")
        mod2 = self.sbx.strict_import("a")
        mod3 = self.sbx.normal_import("a")

        self.assertEqual(type(mod1), ModuleType)
        self.assertEqual(type(mod2), StrictModule)
        self.assertEqual(type(mod3), ModuleType)

    def test_change_strict_module(self) -> None:
        """Changes to strict modules are picked up on subsequent import."""
        self.sbx.write_file("a.py", "import __strict__\nx = 2")
        mod1 = self.sbx.strict_import("a")
        # note it's important to change the length of the file, since Python
        # uses source file size and last-modified time to detect stale pycs, and
        # in the context of this test, last-modified time may not be
        # sufficiently granular to catch the modification.
        self.sbx.write_file("a.py", "import __strict__\nx = 33")
        mod2 = self.sbx.strict_import("a")

        self.assertEqual(type(mod1), StrictModule)
        self.assertEqual(mod1.x, 2)
        self.assertEqual(type(mod2), StrictModule)
        self.assertEqual(mod2.x, 33)

    def test_module_strictness_toggle(self) -> None:
        """Making a non-strict module strict (without removing pycs) works, and v/v."""
        self.sbx.write_file("a.py", "x = 2")
        mod1 = self.sbx.strict_import("a")
        self.sbx.write_file("a.py", "import __strict__\nx = 3")
        mod2 = self.sbx.strict_import("a")
        self.sbx.write_file("a.py", "x = 4")
        mod3 = self.sbx.strict_import("a")

        self.assertEqual(type(mod1), ModuleType)
        self.assertEqual(type(mod2), StrictModule)
        self.assertEqual(type(mod3), ModuleType)

    def test_strict_typing(self) -> None:
        mod = self.sbx.strict_from_code("import __strict__\nfrom typing import TypeVar")
        self.assertIsNotNone(mod.__strict__)
        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(mod.TypeVar, TypeVar)

    def test_cross_module(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import C
                x = C()
            """,
        )
        mod = self.sbx.strict_import("b")

        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(type(mod.C), type)
        self.assertEqual(type(mod.x), mod.C)

    def test_cross_module_static(self) -> None:
        self.sbx.write_file(
            "astatic.py",
            """
                import __strict__
                import __static__
                class C:
                    def f(self) -> int:
                        return 42
            """,
        )
        self.sbx.write_file(
            "bstatic.py",
            """
                import __strict__
                import __static__
                from astatic import C
                def f() -> int:
                    x = C()
                    return x.f()

            """,
        )
        with self.sbx.in_strict_module("bstatic", "astatic") as (mod, amod):
            out = self.get_disassembly_as_string(mod.f)
            self.assertIn("INVOKE_FUNCTION", out)

            out = io.StringIO()
            dis.dis(amod.C.f, file=out)
            self.assertEqual(amod.C.f.__code__.co_consts[-1][1], ("builtins", "int"))

    def test_cross_module_static_typestub(self) -> None:
        self.sbx.write_file(
            "math.pyi",
            """
                import __strict__
                import __static__

                def gcd(a: int, b: int) -> int:
                    ...
            """,
        )
        self.sbx.write_file(
            "bstatic.py",
            """
                import __strict__
                import __static__
                from math import gcd
                def e() -> int:
                    return gcd(15, 25)
            """,
        )
        with restore_static_symtable(), self.sbx.in_strict_module("bstatic") as mod:
            disassembly = self.get_disassembly_as_string(mod.e)
            self.assertIn("INVOKE_FUNCTION", disassembly)

    def test_cross_module_nonstatic_typestub(self) -> None:
        self.sbx.write_file(
            "math.pyi",
            """
                def gcd(a: int, b: int) -> int:
                    ...
            """,
        )
        self.sbx.write_file(
            "bstatic.py",
            """
                import __strict__
                from math import gcd
                def e() -> int:
                    return gcd(15, 25)
            """,
        )
        with restore_static_symtable(), self.sbx.in_strict_module("bstatic") as mod:
            out = io.StringIO()
            dis.dis(mod, file=out)
            disassembly = out.getvalue()
            self.assertIn(self.CALL, disassembly)

    def test_cross_module_static_typestub_ensure_types_untrusted(self) -> None:
        self.sbx.write_file(
            "math.pyi",
            """
                import __strict__
                import __static__

                def gcd(a: int, b: int) -> int:
                    ...
            """,
        )
        self.sbx.write_file(
            "bstatic.py",
            """
                import __strict__
                import __static__
                from math import gcd
                def e() -> int:
                    return gcd("abc", "pqr")
            """,
        )
        with restore_static_symtable(), self.sbx.in_strict_module("bstatic") as mod:
            disassembly = self.get_disassembly_as_string(mod.e)
            self.assertIn("INVOKE_FUNCTION", disassembly)

    def test_cross_module_static_typestub_missing(self) -> None:
        self.sbx.write_file(
            "astatic.py",
            """
                import __strict__
                import __static__
                from math import gcd
                def e() -> int:
                    return gcd(15, 25)
            """,
        )
        with restore_static_symtable(), self.sbx.in_strict_module("astatic") as mod:
            out = io.StringIO()
            dis.dis(mod, file=out)
            disassembly = out.getvalue()
            self.assertIn(self.CALL, disassembly)

    def test_cross_module_2(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import C
            """,
        )
        self.sbx.write_file(
            "c.py",
            """
                import __strict__
                from b import C
                x = C()
            """,
        )
        mod = self.sbx.strict_import("c")

        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(type(mod.C), type)
        self.assertEqual(type(mod.x), mod.C)

    def test_cross_module_package(self) -> None:
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import C
                x = C()
            """,
        )
        mod = self.sbx.strict_import("b")

        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(type(mod.C), type)
        self.assertEqual(type(mod.x), mod.C)

    def test_cross_module_ns_package(self) -> None:
        """we allow from imports through non-strict modules like namespace
        packages as long as the module is fully qualified after the from"""
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a.b import C
                x = C()
            """,
        )
        self.sbx.strict_import("b")

    def test_dataclass_frozen_instantiation(self) -> None:
        test_case = """
            import __strict__
            from dataclasses import dataclass
            from typing import Dict, Iterable, Optional

            @dataclass(frozen=True)
            class C:
                'doc str'
                foo: bool = False
                bar: bool = True
            D = C()

        """
        self.sbx.write_file("foo.py", test_case)
        self.sbx.strict_import("foo")

    def test_import_child_module(self) -> None:
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import b
                x = b.C()
            """,
        )
        mod = self.sbx.strict_import("b")

        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(type(mod.b.C), type)
        self.assertEqual(type(mod.x), mod.b.C)

    def test_import_child_module_not_strict(self) -> None:
        self.sbx.write_file(
            "a/b.py",
            """
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import b
            """,
        )
        mod = self.sbx.strict_import("b")

        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(type(mod.b.C), type)

    def test_import_child_module_side_effects(self) -> None:
        """We disallow the assignment to a strict module when it's not actually
        the correct child module of the strict module."""
        self.sbx.write_file(
            "a/c.py",
            """
                import sys
                sys.modules['a.b'] = sys.modules['a.c']
                class D:
                    pass
                C = D
            """,
        )

        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                from a import c
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import b
                x = b.C()
            """,
        )
        self.sbx.strict_import("b")

    def test_import_child_module_aliased(self) -> None:
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
                b = 42
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import b
            """,
        )
        mod = self.sbx.strict_import("b")

        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(mod.b, 42)

    def test_import_child_module_imported_in_package(self) -> None:
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
                from a import b
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import b
                x = b.C()
            """,
        )
        b = self.sbx.strict_import("b")
        self.assertEqual(type(b.x).__name__, "C")

    def test_import_child_module_imported_in_package_and_aliased(self) -> None:
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
                from a import b
                b = 42
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import b
                x = b
            """,
        )
        b = self.sbx.strict_import("b")
        self.assertEqual(b.x, 42)

    def test_import_child_module_changes_name(self) -> None:
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                class C:
                    pass
            """,
        )
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
                __name__ = 'bar'
            """,
        )
        self.sbx.write_file(
            "bar/__init__.py",
            """
                import __strict__
            """,
        )
        self.sbx.write_file(
            "bar/b.py",
            """
                import __strict__
                class C:
                    x = 2
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import b
                x = b.C.x
            """,
        )
        b = self.sbx.strict_import("b")
        self.assertEqual(b.x, 2)

    def test_cross_module_circular(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                import b

                class C(b.Base):
                    pass

                x = 42
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                import a

                class Base:
                    def f(self):
                        return a.x
            """,
        )
        mod = self.sbx.strict_import("a")

        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(mod.C().f(), 42)

    def test_cross_module_package_import_from_strict_package(self) -> None:
        """import from through a strict package, we can trust the child"""
        self.sbx.write_file("a/__init__.py", "import __strict__")
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                from a import c

                x = c.C()
            """,
        )

        self.sbx.write_file(
            "a/c.py",
            """
                import __strict__
                class C: pass
            """,
        )

        self.sbx.strict_import("a.b")

    def test_cross_module_package_import_from_nonstrict_package_direct_import(
        self,
    ) -> None:
        """we get the 'a.c' module directly and don't go through any additional load
        attrs against a's module"""
        self.sbx.write_file("a/__init__.py", "")
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                from a.c import C

                x = C()
            """,
        )

        self.sbx.write_file(
            "a/c.py",
            """
                import __strict__
                class C: pass
            """,
        )

        self.sbx.strict_import("a.b")

    def test_cross_module_package_import_from_strict_package_direct_import(
        self,
    ) -> None:
        """we get the 'a.c' module directly and don't go through any additional load
        attrs against a's module, but this time through a strict package"""
        self.sbx.write_file("a/__init__.py", "import __strict__")
        self.sbx.write_file(
            "a/b.py",
            """
                import __strict__
                from a.c import C

                x = C()
            """,
        )

        self.sbx.write_file(
            "a/c.py",
            """
                import __strict__
                class C: pass
            """,
        )

        self.sbx.strict_import("a.b")

    def test_cross_module_package_import_child_not_published(self) -> None:
        """child package isn't published on parent of strict module."""
        self.sbx.write_file("a/__init__.py", "import __strict__")
        self.sbx.write_file("a/b.py", "import __strict__")

        a_b, a = self.sbx.strict_import("a.b", "a")
        self.assertFalse(hasattr(a, "b"))

    def test_cross_module_package_import_from_namespace_package_child(self) -> None:
        self.sbx.write_file("package/__init__.py", "import __strict__")

        self.sbx.write_file(
            "package/nspackage/mod.py",
            """
                import __strict__
            """,
        )

        self.sbx.strict_import("package.nspackage")

    def test_cross_module_package_import_child_published_explicitly(self) -> None:
        self.sbx.write_file(
            "a/__init__.py",
            """
                import __strict__
                from a import b
                b = 1
            """,
        )
        self.sbx.write_file("a/b.py", "import __strict__")

        a_b, a = self.sbx.strict_import("a.b", "a")
        self.assertEqual(a.b, 1)

    def test_cross_module_submodule_as_import(self) -> None:
        """Submodule import with as-name respects parent module attribute shadowing."""
        self.sbx.write_file(
            "pkg/__init__.py",
            """
                import __strict__
                from pkg import a as b
            """,
        )
        self.sbx.write_file(
            "pkg/a.py",
            """
                import __strict__
                def a_func():
                    return 1
            """,
        )
        self.sbx.write_file("pkg/b.py", "import __strict__")
        self.sbx.write_file(
            "entry.py",
            """
                import __strict__
                import pkg.b as actually_a

                x = actually_a.a_func()
            """,
        )

        mod = self.sbx.strict_import("entry")
        self.assertEqual(type(mod), StrictModule)
        self.assertEqual(mod.x, 1)

    def test_cross_module_ignore_typing_imports(self) -> None:
        """Ignore typing-only imports so they don't create cycles."""
        self.sbx.write_file(
            "jkbase.py",
            """
                import __strict__
                from requestcontext import get_current_request

                class JustKnobBoolean:
                    def for_request(self) -> bool:
                        request = get_current_request()
                        return True
            """,
        )
        self.sbx.write_file(
            "requestcontext.py",
            """
                import __strict__
                from typing import Type, TYPE_CHECKING
                if TYPE_CHECKING:
                    from other import SomeType

                def get_current_request():
                    return None
            """,
        )
        self.sbx.write_file(
            "other.py",
            """
                import __strict__
                from jkbase import JustKnobBoolean

                x = JustKnobBoolean()

                class SomeType:
                    pass
            """,
        )
        jkbase, other = self.sbx.strict_import("jkbase", "other")
        self.assertEqual(type(other.x), jkbase.JustKnobBoolean)

    @passIf(sys.version_info >= (3, 14), "not supported on 3.14")
    def test_annotations_present(self) -> None:
        code = """
            import __strict__
            x: int
        """
        mod = self.sbx.strict_from_code(code)
        self.assertEqual(mod.__annotations__, {"x": int})

    @passIf(sys.version_info >= (3, 14), "not supported on 3.14")
    def test_annotations_non_name(self) -> None:
        code = """
            import __strict__
            class C: pass

            C.x: int
        """
        mod = self.sbx.strict_from_code(code)
        self.assertEqual(mod.__annotations__, {})

    @passIf(sys.version_info >= (3, 14), "not supported on 3.14")
    def test_annotations_reinit(self) -> None:
        code = """
            import __strict__
            __annotations__ = {'y': 100}
            x: int
        """
        mod = self.sbx.strict_from_code(code)
        self.assertEqual(mod.__annotations__, {"x": int, "y": 100})

    def test_class_instance_field_ok(self) -> None:
        code = """
            import __strict__
            class C:
                x: int
        """
        self.sbx.strict_from_code(code)

    @passIf(sys.version_info >= (3, 14), "not supported on 3.14")
    def test_class_try_except_else(self) -> None:
        """try/orelse/except visit order matches symbol visitor"""
        code = """
            import __strict__
            class C:
                try:
                    pass
                except:
                    def f(self):
                        return bar
                else:
                    def f(self, x):
                        return abc
        """
        self.sbx.strict_from_code(code)

    def test_source_callback(self) -> None:
        calls: list[str] = []
        # pyre-ignore[21]: this test relies on this being imported already.
        import __strict__

        def log(filename: str, bytecode_path: str | None, bytecode_found: bool) -> None:
            calls.append(filename)
            self.assertEqual(bytecode_found, False)
            assert bytecode_path is not None
            self.assertTrue(bytecode_path.endswith(".pyc"))

        logging_loader = (
            lambda fullname, path: StrictSourceFileLoader(
                fullname, path, sys.path, log_source_load=log
            ),
            SOURCE_SUFFIXES,
        )

        self.sbx.write_file(
            "a.py",
            """
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                import a
            """,
        )

        with self.sbx.begin_loader(logging_loader):
            __import__("b")
            self.assertEqual(len(calls), 2)
            self.assertEqual(path.split(calls[0])[-1], "b.py")
            self.assertEqual(path.split(calls[1])[-1], "a.py")

    def test_proxy_setter(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                proxy.x = 100  # pyre-ignore[16]: no attribute x
                self.assertEqual(a.x, 100)
            self.assertEqual(a.x, 2)

    def test_proxy_setter_twice(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                proxy.x = 100  # pyre-ignore[16]: no attribute x
                self.assertEqual(a.x, 100)
                proxy.x = 200
                self.assertEqual(a.x, 200)
            self.assertEqual(a.x, 2)

    def test_proxy_setter_no_attribute(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\n")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertFalse(hasattr(a, "x"))
            with StrictModuleTestingPatchProxy(a) as proxy:
                proxy.x = 100  # pyre-ignore[16]: no attribute x
                self.assertEqual(a.x, 100)
                proxy.x = 200
                self.assertEqual(a.x, 200)
            self.assertFalse(hasattr(a, "x"))

    def test_proxy_set_then_del_no_attribute(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\n")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertFalse(hasattr(a, "x"))
            with StrictModuleTestingPatchProxy(a) as proxy:
                proxy.x = 100  # pyre-ignore[16]: no attribute x
                self.assertEqual(a.x, 100)
                del proxy.x
                self.assertFalse(hasattr(a, "x"))
            self.assertFalse(hasattr(a, "x"))

    def test_proxy_setter_restore(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                proxy.x = 100  # pyre-ignore[16]: no attribute x
                self.assertEqual(a.x, 100)
                proxy.x = 2
                self.assertEqual(a.x, 2)
                self.assertEqual(len(object.__getattribute__(proxy, "_patches")), 0)
            self.assertEqual(a.x, 2)

    def test_proxy_deleter(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                del proxy.x  # pyre-ignore[16]: no attribute x
                self.assertFalse(hasattr(a, "x"))
            self.assertEqual(a.x, 2)

    def test_proxy_deleter_restore(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                del proxy.x  # pyre-ignore[16]: no attribute x
                self.assertFalse(hasattr(a, "x"))
                proxy.x = 2
                self.assertEqual(a.x, 2)
                self.assertEqual(len(object.__getattribute__(proxy, "_patches")), 0)
            self.assertEqual(a.x, 2)

    def test_proxy_not_disposed(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            proxy = StrictModuleTestingPatchProxy(a)
            proxy.x = 100  # pyre-ignore[16]: no attribute x
            abort_called = False

            def abort() -> None:
                nonlocal abort_called
                abort_called = True

            err = io.StringIO()
            with patch("os.abort", abort), patch("sys.stderr", err):
                del proxy
                gc.collect()

            self.assertTrue(abort_called)
            self.assertEqual(
                err.getvalue(),
                "Patch(es) x failed to be detached from strict module 'a'\n",
            )

    def test_proxy_not_enabled(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        a = self.sbx.strict_import("a")
        with self.sbx.with_strict_patching(False):
            with self.assertRaises(ValueError):
                StrictModuleTestingPatchProxy(a)

    def test_proxy_nested_setter(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                proxy.x = 100  # pyre-ignore[16]: no attribute x
                self.assertEqual(a.x, 100)
                with StrictModuleTestingPatchProxy(a) as proxy2:
                    proxy2.x = 200
                    self.assertEqual(a.x, 200)
                self.assertEqual(a.x, 100)
            self.assertEqual(a.x, 2)

    def test_proxy_nested_setter_restore(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                proxy.x = 100  # pyre-ignore[16]: no attribute x
                self.assertEqual(a.x, 100)
                with StrictModuleTestingPatchProxy(a) as proxy2:
                    proxy2.x = 200
                    self.assertEqual(a.x, 200)
                    proxy2.x = 100
                    self.assertEqual(a.x, 100)
                    self.assertEqual(
                        len(object.__getattribute__(proxy2, "_patches")), 0
                    )
                proxy.x = 2
                self.assertEqual(a.x, 2)
                self.assertEqual(len(object.__getattribute__(proxy, "_patches")), 0)
            self.assertEqual(a.x, 2)

    def test_proxy_nested_deleter(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                del proxy.x  # pyre-ignore[16]: no attribute x
                self.assertFalse(hasattr(a, "x"))
                with StrictModuleTestingPatchProxy(a) as proxy2:
                    proxy2.x = 100
                    self.assertEqual(a.x, 100)
                self.assertFalse(hasattr(a, "x"))
            self.assertEqual(a.x, 2)

    def test_proxy_nested_deleter_restore(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nx = 2")

        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.x, 2)
            with StrictModuleTestingPatchProxy(a) as proxy:
                del proxy.x  # pyre-ignore[16]: no attribute x
                self.assertFalse(hasattr(a, "x"))
                with StrictModuleTestingPatchProxy(a) as proxy2:
                    proxy2.x = 3
                    self.assertEqual(a.x, 3)
                proxy.x = 2
                self.assertEqual(a.x, 2)
                self.assertEqual(len(object.__getattribute__(proxy, "_patches")), 0)
            self.assertEqual(a.x, 2)

    def test_generic_namedtuple(self) -> None:
        code = """
            import __strict__

            from typing import NamedTuple

            class C(NamedTuple):
                x: int
                y: str

            a = C(42, 'foo')
        """
        mod = self.sbx.strict_from_code(code)
        self.assertEqual(mod.a, (42, "foo"))

    def test_type_freeze(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nclass C: pass")
        with ensure_type_patch():
            C = self.sbx.strict_import("a").C
            with self.assertRaises(TypeError):
                C.foo = 42

    def test_type_freeze_mutate_after(self) -> None:
        self.sbx.write_file("a.py", "import __strict__\nclass C: pass\nC.foo = 42")
        with ensure_type_patch():
            C = self.sbx.strict_import("a").C
            self.assertEqual(C.foo, 42)
            with self.assertRaises(TypeError):
                C.foo = 100

    def test_type_freeze_func(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                def f():
                    class C: pass
                    return C
            """,
        )
        with ensure_type_patch():
            C = self.sbx.strict_import("a").f()
            with self.assertRaises(TypeError):
                C.foo = 100

    def test_type_freeze_func_loop(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                def f():
                    l = []
                    for i in range(2):
                        class C: pass
                        l.append(C)
                    return l
            """,
        )
        with ensure_type_patch():
            for C in self.sbx.strict_import("a").f():
                with self.assertRaises(TypeError):
                    C.foo = 100

    def test_type_freeze_func_mutate_after(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                def f():
                    class C: pass
                    C.foo = 42
                    return C
            """,
        )
        with ensure_type_patch():
            C = self.sbx.strict_import("a").f()
            self.assertEqual(C.foo, 42)
            with self.assertRaises(TypeError):
                C.foo = 100

    def test_type_freeze_nested(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                class C:
                    class D: pass
            """,
        )
        with ensure_type_patch():
            D = self.sbx.strict_import("a").C.D
            with self.assertRaises(TypeError):
                D.foo = 100

    def test_type_mutable(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                from __strict__ import mutable

                @mutable
                class C:
                    pass
            """,
        )
        with ensure_type_patch():
            C = self.sbx.strict_import("a").C
            C.foo = 42
            self.assertEqual(C.foo, 42)

    def test_type_freeze_disabled(self) -> None:
        with ensure_type_patch(False):
            self.sbx.write_file("a.py", "import __strict__\nclass C: pass")
            C = self.sbx.strict_import("a").C
            C.foo = 42
            self.assertEqual(C.foo, 42)

    @passIf(
        HAVE_WARN_HANDLERS, "T214641462: Strict Modules warn handlers not supported"
    )
    def test_class_explicit_dict_no_warning(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__

                class C:
                    __dict__: object
            """,
        )
        with ensure_type_patch(), with_warn_handler() as warnings:
            C = self.sbx.strict_import("a").C
            a = C()
            a.foo = 42
            self.assertEqual(warnings, [])

    def test_attribute_error(self) -> None:
        self.sbx.write_file("a.py", "import __strict__")
        a = self.sbx.strict_import("a")
        with self.assertRaisesRegex(
            AttributeError, "strict module 'a' has no attribute 'foo'"
        ):
            a.foo

    def test_cross_module_raise_handled(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
            import __strict__
            def f():
                raise ValueError()
        """,
        )
        self.sbx.write_file(
            "b.py",
            """
            import __strict__
            from a import f
            try:
                f()
            except Exception as e:
                y = e
        """,
        )

        b = self.sbx.strict_import("b")
        self.assertEqual(type(b.y), ValueError)

    def test_lru_cache(self) -> None:
        # lru cache exists and can be used normally
        self.sbx.write_file(
            "a.py",
            """
            import __strict__
            from functools import lru_cache

            class C:
                def __init__(self):
                    self.calls = 0

                @lru_cache(42)
                def f(self):
                    self.calls += 1
                    return 42
        """,
        )
        a = self.sbx.strict_import("a")
        x = a.C()
        self.assertEqual(x.f(), 42)
        self.assertEqual(x.f(), 42)
        self.assertEqual(x.calls, 1)

    def test_is_strict_module(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
            import __strict__
        """,
        )
        a = self.sbx.strict_import("a")
        self.assertTrue(isinstance(a, StrictModule))

    def test_static_python(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                import __static__
                from typing import Optional
                class C:
                    def __init__(self):
                        self.x: Optional[C] = None
            """,
        )
        with self.sbx.in_strict_module("a") as mod:
            a = mod.C()
            self.assertEqual(a.x, None)

    def test_static_python_del_builtin(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                import __static__
                for int in [1, 2]:
                    pass
                del int
                def f():
                    return int('3')
            """,
        )
        with self.sbx.in_strict_module("a") as mod:
            self.assertEqual(mod.f(), 3)

    def test_static_python_final_globals_patch(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                import __static__
                from typing import Final

                a: Final[int] = 1337

                def fn():
                    return a
            """,
        )
        with self.sbx.with_strict_patching():
            a = __import__("a")
            assert isinstance(a, StrictModule)
            self.assertEqual(a.__final_constants__, ("a",))

            with (
                StrictModuleTestingPatchProxy(a) as proxy,
                self.assertRaisesRegex(
                    AttributeError, "Cannot patch Final attribute `a` of module `a`"
                ),
            ):
                # pyre-ignore [16]: `proxy` has no attribute `a`
                proxy.a = 0xDEADBEEF

            with (
                StrictModuleTestingPatchProxy(a) as proxy,
                self.assertRaisesRegex(
                    AttributeError, "Cannot patch Final attribute `a` of module `a`"
                ),
            ):
                del proxy.a

    def test_future_annotations_with_strict_modules(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
            from __future__ import annotations
            import __strict__
            from typing import TYPE_CHECKING, List
            if TYPE_CHECKING:
                from b import C
            class D:
                x : List[C] = []
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
            from __future__ import annotations
            import __strict__
            class C:
                pass
            """,
        )
        a, b = self.sbx.strict_import("a", "b")
        self.assertEqual(a.D.x, [])

    def test_loading_allowlisted_dependencies(self) -> None:
        with patch.object(base_sandbox, "ALLOW_LIST", ["dir_a.a"]):
            self.sbx.write_file(
                "dir_a/a.py",
                """
                class A:
                    def __init__(self):
                        self.x = True
                """,
            )
            self.sbx.write_file(
                "b.py",
                """
                import __strict__
                from __strict__ import strict_slots
                from dir_a.a import A
                a = A()
                if a.x:
                    y = 1
                else:
                    unknown.call()
                @strict_slots
                class B:
                    pass
                """,
            )
            # analysis of b correctly uses value from `a`
            # since `a` is allowlisted
            a, b = self.sbx.strict_import("dir_a.a", "b")
        # a is not created as a strict module, but b is
        self.assertNotEqual(type(a), StrictModule)
        self.assertEqual(type(b), StrictModule)
        self.assertEqual(b.y, 1)

    def test_relative_import(self) -> None:
        self.sbx.write_file(
            "package_a/a.py",
            """
            import __strict__
            x = 1
            """,
        )
        self.sbx.write_file(
            "package_a/b.py",
            """
            import __strict__
            from .a import x
            y = x + 1
            """,
        )
        self.sbx.write_file(
            "package_a/subpackage/c.py",
            """
            import __strict__
            from ..b import y
            z = y + 1
            """,
        )
        a, b, c = self.sbx.strict_import(
            "package_a.a", "package_a.b", "package_a.subpackage.c"
        )
        self.assertEqual(a.x, 1)
        self.assertEqual(b.y, 2)
        self.assertEqual(c.z, 3)

    def test_disable_slots(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
            import __strict__
            class C:
                x: int
            """,
        )
        a = self.sbx.strict_import("a")
        self.assertFalse(hasattr(a.C, "__slots__"))

    def test_cross_module_first_analysis_wins(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                import __strict__
                class C:
                    def f(self):
                        return 1
                X = C()
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
                import __strict__
                from a import X
                x = X.f()
            """,
        )
        self.sbx.write_file(
            "c.py",
            """
                import __strict__
                from a import X as Xa
                from b import X as Xb

                if Xa is not Xb:
                    raise Exception('no way')
            """,
        )
        # These should pass strict module analysis and
        # load successfully, we shouldn't end up with any
        # weird identity with our analysis objects between
        # analysis and execution.
        with self.sbx.begin_loader(STRICT_LOADER):
            __import__("b")
            __import__("a")
            __import__("c")

    def test_syntax_error_source(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                aa bb
            """,
        )
        with self.assertRaises(SyntaxError):
            self.sbx.strict_import("a")

    def test_syntax_error_import(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
                aa bb
            """,
        )

        self.sbx.write_file(
            "b.py",
            """
                import a
                a.x
            """,
        )
        with self.assertRaises(SyntaxError):
            self.sbx.strict_import("b")

    @passIf(sys.version_info >= (3, 15), "no lazy imports on 3.15")
    def test_strict_loader_lazy_imports_cycle(self) -> None:
        self.sbx.write_file(
            "main.py",
            """
            from cinderx.compiler.strict.loader import install
            install()
            from staticmod import f
            print(f())
            """,
        )
        self.sbx.write_file(
            "staticmod.py",
            """
            import __static__
            def f():
                return 42
            """,
        )

        proc = subprocess.run(
            [sys.executable, "-L", "main.py"],
            cwd=str(self.sbx.root),
            env=self.subprocess_env,
            capture_output=True,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr.decode())
        self.assertIn(b"42", proc.stdout, proc.stdout.decode())

    @property
    def subprocess_env(self):
        return {**os.environ, **subprocess_env()}

    def test_strict_loader_stub_path(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
            import b
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
            import __strict__
            from c import f

            f()
            """,
        )
        self.sbx.write_file(
            "c.py",
            """
            def f():
                print("hi")
            """,
        )
        with tempfile.TemporaryDirectory(prefix="strict_stubs") as raw_stubs_path:
            stubs_path = pathlib.Path(raw_stubs_path)
            stub_contents = textwrap.dedent(
                """
                def f(): ...
            """
            )
            (stubs_path / "c.pys").write_text(stub_contents)

            res = subprocess.run(
                [sys.executable, "-X", "install-strict-loader", "a.py"],
                cwd=str(self.sbx.root),
                env={
                    **self.subprocess_env,
                    "PYTHONSTRICTMODULESTUBSPATH": raw_stubs_path,
                },
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(res.returncode, 0)
            output = res.stdout.decode()
            self.assertEqual(output, "hi\n")

    def test_strict_loader_stub_path_x_arg(self) -> None:
        self.sbx.write_file(
            "a.py",
            """
            import b
            """,
        )
        self.sbx.write_file(
            "b.py",
            """
            import __strict__
            from c import f

            f()
            """,
        )
        self.sbx.write_file(
            "c.py",
            """
            def f():
                print("hi")
            """,
        )
        with tempfile.TemporaryDirectory(prefix="strict_stubs") as raw_stubs_path:
            stubs_path = pathlib.Path(raw_stubs_path)
            stub_contents = textwrap.dedent(
                """
                def f(): ...
            """
            )
            (stubs_path / "c.pys").write_text(stub_contents)

            res = subprocess.run(
                [
                    sys.executable,
                    "-X",
                    "install-strict-loader",
                    "-X",
                    f"strict-module-stubs-path={raw_stubs_path}",
                    "a.py",
                ],
                cwd=str(self.sbx.root),
                env=self.subprocess_env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(res.returncode, 0)
            output = res.stdout.decode()
            self.assertEqual(output, "hi\n")

    def test_clear_classloader_cache_on_aborted_import(self):
        flagcode = """
            val = True

            def maybe_throw(c):
                if val:
                    # ensure we populate the classloader cache with this version of C
                    import other
                    other.f(c)
                    raise ImportError("this module fails to import")
        """
        modcode = """
            import __strict__
            import __static__
            from __strict__ import allow_side_effects
            from flag import maybe_throw

            class C:
                pass

            maybe_throw(C())
        """
        othercode = """
            from __future__ import annotations
            import __strict__
            import __static__
            from __strict__ import allow_side_effects

            import mod

            def f(c: mod.C):
                return c
        """
        self.sbx.write_file("other.py", othercode)
        self.sbx.write_file("mod.py", modcode)
        self.sbx.write_file("flag.py", flagcode)

        with self.sbx.isolated_strict_loader(), write_bytecode(False):
            # import bad version of 'mod' to populate class loader cache and then roll back import
            with self.assertRaisesRegex(ImportError, "this module fails to import"):
                # pyre-ignore[21]: Intentionally meant to fail.
                import mod
            # pyre-ignore[21]: Loaded dynamically.
            import flag

            flag.val = False
            # pyre-ignore[21]: Loaded dynamically.
            import mod  # noqa: E401, F811

            # pyre-ignore[21]: Loaded dynamically.
            import other  # noqa: E401, F811

            c = mod.C()

            # if we have the bad classloader cache still, this call will fail
            # because the instance we are passing in is an instance of the new
            # version of the C class, but the argument check will be against the
            # old version of the C class from the failed import
            self.assertIs(other.f(c), c)

    @passIf(sys.version_info >= (3, 15), "no lazy imports on 3.15")
    def test_strict_lazy_import_cycle(self):
        self.sbx.write_file(
            "mod/__init__.py",
            """
            import __strict__
            from . import version
            """,
        )
        self.sbx.write_file(
            "mod/version.py",
            """
            import __strict__
            VERSION = 1
            """,
        )
        self.sbx.write_file(
            "entry.py",
            """
            from mod import version
            v = version.VERSION
            """,
        )
        for lazy in [True, False]:
            with self.subTest(lazy=lazy):
                orig = _imp._set_lazy_imports(lazy)
                try:
                    mod = self.sbx.strict_import("entry")
                finally:
                    _imp._set_lazy_imports(*orig)
                self.assertEqual(mod.v, 1)

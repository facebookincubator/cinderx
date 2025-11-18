# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
import asyncio
import builtins
import gc
import os
import re
import sys
from contextlib import AbstractContextManager, contextmanager
from functools import wraps
from types import CodeType, FunctionType, ModuleType
from typing import Any, Awaitable, Callable, ContextManager, final, Generator, Mapping

import cinderx

import cinderx.jit
from cinderx import StrictModule

from cinderx.compiler.dis_stable import Disassembler
from cinderx.compiler.errors import (
    CollectingErrorSink,
    ErrorSink,
    PerfWarning,
    TypedSyntaxError,
)
from cinderx.compiler.pycodegen import CodeGenerator
from cinderx.compiler.static import StaticCodeGenerator
from cinderx.compiler.static.compiler import Compiler
from cinderx.compiler.static.module_table import DepTrackingOptOut, ModuleTable
from cinderx.compiler.static.types import Value
from cinderx.compiler.strict.common import FIXED_MODULES
from cinderx.compiler.strict.compiler import Compiler as StrictCompiler
from cinderx.compiler.strict.flag_extractor import Flags
from cinderx.compiler.strict.loader import init_static_python
from cinderx.compiler.strict.runtime import set_freeze_enabled

from cinderx.static import (
    __build_cinder_class__,
    TYPED_BOOL,
    TYPED_INT16,
    TYPED_INT32,
    TYPED_INT64,
    TYPED_INT8,
    TYPED_UINT16,
    TYPED_UINT32,
    TYPED_UINT64,
    TYPED_UINT8,
)
from cinderx.test_support import compiles_after_one_call

from ..common import CompilerTest

TEST_OPT_OUT = DepTrackingOptOut("tests")


# Code that dynamically passes around module objects is hard to type, since
# modules can have any attribute of any type and Pyre doesn't know anything
# about it. This fake type makes it less painful while also requiring fewer
# warnings about using Any.
@final
class TModule(ModuleType):
    # pyre-ignore[3]: Have fun trying to type this as non-Any.
    def __getattr__(self, name: str) -> Any: ...


def type_mismatch(from_type: str, to_type: str) -> str:
    return re.escape(f"type mismatch: {from_type} cannot be assigned to {to_type}")


def bad_ret_type(from_type: str, to_type: str) -> str:
    return re.escape(
        f"mismatched types: expected {to_type} because of return type, found {from_type} instead"
    )


def disable_hir_inliner(f: Callable[..., object]) -> Callable[..., object]:
    @wraps(f)
    def impl(*args: object, **kwargs: object) -> object:
        enabled = cinderx.jit.is_hir_inliner_enabled()
        if enabled:
            cinderx.jit.disable_hir_inliner()
        result = f(*args, **kwargs)
        if enabled:
            cinderx.jit.enable_hir_inliner()
        return result

    return impl


PRIM_NAME_TO_TYPE: Mapping[str, int] = {
    "cbool": TYPED_BOOL,
    "int8": TYPED_INT8,
    "int16": TYPED_INT16,
    "int32": TYPED_INT32,
    "int64": TYPED_INT64,
    "uint8": TYPED_UINT8,
    "uint16": TYPED_UINT16,
    "uint32": TYPED_UINT32,
    "uint64": TYPED_UINT64,
}


def dis(code: CodeType) -> None:
    Disassembler().dump_code(code, file=sys.stdout)


def get_child(mod: ModuleTable, name: str) -> Value | None:
    return mod.get_child(name, TEST_OPT_OUT)


class TestCompiler(Compiler):
    def __init__(
        self,
        source_by_name: Mapping[str, str],
        test_case: StaticTestBase,
        code_generator: type[StaticCodeGenerator] = StaticCodeGenerator,
        error_sink: ErrorSink | None = None,
        strict_modules: bool = False,
        enable_patching: bool = False,
    ) -> None:
        self.source_by_name = source_by_name
        self.test_case = test_case
        self.strict_modules = strict_modules
        self.enable_patching = enable_patching
        super().__init__(code_generator, error_sink)

    def import_module(self, name: str, optimize: int = 0) -> ModuleTable | None:
        if name not in self.modules:
            tree = self._get_module_ast(name)
            if tree is not None:
                self.add_module(
                    name,
                    self._get_filename(name),
                    tree,
                    self.source_by_name.get(name) or name,
                    optimize,
                )
        return self.modules.get(name)

    def compile_module(self, name: str, optimize: int = 0) -> CodeType:
        tree = self._get_module_ast(name)
        if tree is None:
            raise ValueError(f"No source found for module '{name}'")
        return self.compile(
            name,
            self._get_filename(name),
            tree,
            self.source_by_name.get(name) or name,
            optimize=optimize,
        )

    def _get_module_ast(self, name: str) -> ast.Module | None:
        source = self.source_by_name.get(name)
        if source is not None:
            return ast.parse(source)

    @contextmanager
    def in_module(
        self, modname: str, optimize: int = 0
    ) -> Generator[ModuleType, None, None]:
        for mod in self.gen_modules(optimize):
            if mod.__name__ == modname:
                yield mod
                break
        else:
            raise Exception(f"No module named '{modname}' found")

    def gen_modules(self, optimize: int = 0) -> Generator[ModuleType, None, None]:
        names = self.source_by_name.keys()
        compiled = [self.compile_module(name, optimize) for name in names]
        try:
            dicts = []
            for name, codeobj in zip(names, compiled):
                d, m = self._in_module(name, codeobj)
                dicts.append(d)
                yield m
        finally:
            for name, d in zip(names, dicts):
                self.test_case._finalize_module(name, d)

    def _in_module(
        self, name: str, codeobj: CodeType
    ) -> tuple[dict[str, object], ModuleType]:
        if self.strict_modules:
            return self.test_case._in_strict_module(
                name, codeobj, enable_patching=self.enable_patching
            )
        return self.test_case._in_module(name, codeobj)

    def type_error(
        self, name: str, pattern: str, at: str | None = None
    ) -> TestCompiler:
        source = self.source_by_name[name]
        with self.test_case.type_error_ctx(source, pattern, at):
            self.compile_module(name)
        return self

    def revealed_type(self, name: str, type: str) -> TestCompiler:
        source = self.source_by_name[name]
        with self.test_case.revealed_type_ctx(source, type):
            self.compile_module(name)
        return self

    def _get_filename(self, name: str) -> str:
        return name.split(".")[-1] + ".py"


def add_fixed_module(d: dict[str, Any]) -> None:
    d["<fixed-modules>"] = FIXED_MODULES


class ErrorMatcher:
    __slots__ = (
        "msg",
        "at",
    )

    def __init__(self, msg: str, at: str | None = None) -> None:
        self.msg = msg
        self.at = at


class TestErrors:
    def __init__(
        self,
        case: StaticTestBase,
        code: str,
        errors: list[TypedSyntaxError],
        warnings: list[PerfWarning],
    ) -> None:
        self.case = case
        self.code = code
        self.errors = errors
        self.warnings = warnings

    def _check(
        self,
        kind: str,
        errors: list[TypedSyntaxError] | list[PerfWarning],
        *matchers: ErrorMatcher,
        loc_only: bool = False,
        use_end_offset: bool = True,
    ) -> None:
        self.case.assertEqual(
            len(matchers),
            len(errors),
            f"Expected {len(matchers)} {kind}s, got {errors}",
        )
        for exc, matcher in zip(errors, matchers):
            if not loc_only:
                self.case.assertIn(matcher.msg, str(exc))
            if (at := matcher.at) is not None:
                lineno = exc.lineno
                assert lineno, f"Exception expected to have a line number: {exc}"
                offset = exc.offset
                assert offset, f"Exception expected to have an offset: {exc}"
                end_offset = None
                temp = exc.end_offset
                if temp is not None and use_end_offset:
                    end_offset = temp - 1
                actual = self.code.split("\n")[lineno - 1][offset - 1 : end_offset]
                if (use_end_offset and actual != at) or (
                    not use_end_offset and not actual.startswith(at)
                ):
                    self.case.fail(
                        f"Expected {kind} '{matcher.msg}' at '{at}', occurred at '{actual}'"
                    )

    def check(
        self,
        *matchers: ErrorMatcher,
        loc_only: bool = False,
        use_end_offset: bool = True,
    ) -> None:
        self._check(
            "error",
            self.errors,
            *matchers,
            loc_only=loc_only,
            use_end_offset=use_end_offset,
        )

    def check_warnings(self, *matchers: ErrorMatcher, loc_only: bool = False) -> None:
        self._check("warning", self.warnings, *matchers, loc_only=loc_only)

    def match(self, msg: str, at: str | None = None) -> ErrorMatcher:
        return ErrorMatcher(msg, at)


class StaticTestBase(CompilerTest):
    _inline_comprehensions: bool = bool(
        os.getenv("PYTHONINLINECOMPREHENSIONS")
    ) or sys.version_info >= (3, 12)

    @classmethod
    def setUpClass(cls) -> None:
        init_static_python()

    def shortDescription(self) -> None:
        return None

    def compile(
        self,
        code: str,
        generator: type[CodeGenerator] | None = StaticCodeGenerator,
        modname: str = "<module>",
        optimize: int = 0,
        ast_optimizer_enabled: bool = True,
        enable_patching: bool = False,
    ) -> CodeType:
        assert ast_optimizer_enabled

        if generator is not StaticCodeGenerator:
            return super().compile(
                code,
                generator,
                modname,
                optimize,
                ast_optimizer_enabled,
            )

        compiler = Compiler(StaticCodeGenerator)
        tree = ast.parse(self.clean_code(code))
        return compiler.compile(
            modname,
            f"{modname}.py",
            tree,
            code,
            optimize,
            enable_patching=enable_patching,
        )

    def get_strict_compiler(self, enable_patching: bool = False) -> StrictCompiler:
        return StrictCompiler(
            [], "", [], [], raise_on_error=True, enable_patching=enable_patching
        )

    def compile_strict(
        self,
        codestr: str,
        modname: str = "<module>",
        optimize: int = 0,
        enable_patching: bool = False,
        override_flags: Flags | None = None,
    ) -> CodeType | None:
        compiler = self.get_strict_compiler(enable_patching=enable_patching)

        code, is_valid_strict, _is_static = compiler.load_compiled_module_from_source(
            self.clean_code(codestr),
            f"{modname}.py",
            modname,
            optimize,
            override_flags=override_flags or Flags(is_strict=True),
        )
        assert is_valid_strict
        return code

    def lint(self, code: str) -> TestErrors:
        errors = CollectingErrorSink()
        code = self.clean_code(code)
        tree = ast.parse(code)
        compiler = Compiler(StaticCodeGenerator, errors)
        compiler.bind("<module>", "<module>.py", tree, code, optimize=0)
        return TestErrors(self, code, errors.errors, errors.warnings)

    def perf_lint(self, code: str) -> TestErrors:
        errors = CollectingErrorSink()
        code = self.clean_code(code)
        tree = ast.parse(code)
        compiler = Compiler(StaticCodeGenerator, errors)
        tree, s = compiler._bind("<module>", "<module>.py", tree, code, optimize=0)
        if not errors.errors:
            graph = StaticCodeGenerator.flow_graph(
                "<module>", "<module>.py", s.scopes[tree]
            )
            code_gen = StaticCodeGenerator(None, tree, s, graph, compiler, "<module>")
            code_gen.visit(tree)
        return TestErrors(self, code, errors.errors, errors.warnings)

    def get_arg_check_types(self, func: FunctionType | CodeType) -> tuple[object, ...]:
        if isinstance(func, CodeType):
            return func.co_consts[-1][0]
        return func.__code__.co_consts[-1][0]

    @contextmanager
    def type_error_ctx(
        self,
        code: str,
        pattern: str,
        at: str | None = None,
        lineno: int | None = None,
        offset: int | None = None,
    ) -> Generator[None, None, None]:
        with self.assertRaisesRegex(TypedSyntaxError, pattern) as ctx:
            yield
        exc = ctx.exception
        errors = TestErrors(self, self.clean_code(code), [ctx.exception], [])
        if at is not None:
            errors.check(ErrorMatcher(pattern, at), loc_only=True, use_end_offset=False)
        if lineno is not None:
            self.assertEqual(exc.lineno, lineno)
        if offset is not None:
            self.assertEqual(exc.offset, offset)

    # pyre-ignore[34]: missing type variable
    def revealed_type_ctx(self, code: str, type: str) -> ContextManager[None]:
        # pyre-ignore[7]: bad return type
        return self.type_error_ctx(
            code, rf"reveal_type\(.+\): '{re.escape(type)}'", at="reveal_type("
        )

    def type_error(
        self,
        code: str,
        pattern: str,
        at: str | None = None,
        lineno: int | None = None,
        offset: int | None = None,
    ) -> None:
        with self.type_error_ctx(code, pattern, at, lineno, offset):
            self.compile(code)

    def revealed_type(self, code: str, type: str) -> None:
        with self.revealed_type_ctx(code, type):
            self.compile(code)

    def _clean_sources(self, sources: dict[str, str]) -> dict[str, str]:
        return {name: self.clean_code(code) for name, code in sources.items()}

    def compiler(self, **sources: str) -> TestCompiler:
        return TestCompiler(self._clean_sources(sources), self)

    def strict_compiler(self, **sources: str) -> TestCompiler:
        return TestCompiler(self._clean_sources(sources), self, strict_modules=True)

    def strict_patch_compiler(self, **sources: str) -> TestCompiler:
        return TestCompiler(
            self._clean_sources(sources),
            self,
            strict_modules=True,
            enable_patching=True,
        )

    _temp_mod_num = 0

    def _temp_mod_name(self) -> str:
        StaticTestBase._temp_mod_num += 1
        # pyre-ignore[16]: Expecting a certain number of frames.
        return sys._getframe().f_back.f_back.f_back.f_code.co_name + str(
            StaticTestBase._temp_mod_num
        )

    def _finalize_module(
        self, name: str, mod_dict: dict[str, object] | None = None
    ) -> None:
        if name in sys.modules:
            del sys.modules[name]
        if mod_dict is not None:
            mod_dict.clear()
        gc.collect()

    def _in_module(
        self, name: str, code_obj: CodeType
    ) -> tuple[dict[str, object], ModuleType]:
        m = type(sys)(name)
        d = m.__dict__
        d["<builtins>"] = builtins.__dict__
        add_fixed_module(d)
        sys.modules[name] = m
        exec(code_obj, d)
        d["__name__"] = name
        return d, m

    @contextmanager
    def with_freeze_type_setting(self, freeze: bool) -> Generator[None, None, None]:
        old_setting = set_freeze_enabled(freeze)
        try:
            yield
        finally:
            set_freeze_enabled(old_setting)

    @contextmanager
    def in_module(
        self,
        code: str,
        name: str | None = None,
        code_gen: type[CodeGenerator] = StaticCodeGenerator,
        optimize: int = 0,
        freeze: bool = False,
        enable_patching: bool = False,
        dump_bytecode: bool = False,
    ) -> Generator[ModuleType, None, None]:
        d = None
        if name is None:
            name = self._temp_mod_name()
        old_setting = set_freeze_enabled(freeze)
        try:
            compiled = self.compile(
                code, code_gen, name, optimize, enable_patching=enable_patching
            )
            if dump_bytecode:
                dis(compiled)
            d, m = self._in_module(name, compiled)
            yield m
        finally:
            set_freeze_enabled(old_setting)
            self._finalize_module(name, d)

    def _in_strict_module(
        self,
        name: str,
        code_obj: CodeType,
        enable_patching: bool = False,
    ) -> tuple[dict[str, object], ModuleType]:
        d = {
            "__name__": name,
            "<builtins>": builtins.__dict__,
            "<imported-from>": code_obj.co_consts[-1],
        }
        add_fixed_module(d)
        m = StrictModule(d, enable_patching)
        # pyre-ignore[6]: Expecting ModuleType but getting StrictModule.
        sys.modules[name] = m
        exec(code_obj, d)
        # pyre-ignore[7]: Treating StrictModule as ModuleType, again.
        return d, m

    @contextmanager
    def in_strict_module(
        self,
        code: str,
        name: str | None = None,
        optimize: int = 0,
        enable_patching: bool = False,
        freeze: bool = True,
        dump_bytecode: bool = False,
    ) -> Generator[TModule, None, None]:
        d = None
        if name is None:
            name = self._temp_mod_name()
        old_setting = set_freeze_enabled(freeze)
        try:
            compiled = self.compile_strict(
                code,
                name,
                optimize,
                enable_patching=enable_patching,
                override_flags=Flags(is_strict=True, is_static=True),
            )
            if compiled is None:
                raise RuntimeError("Failed to compile strict module")
            if dump_bytecode:
                dis(compiled)
            d, m = self._in_strict_module(name, compiled, enable_patching)
            # pyre-ignore[7]: Treat this ModuleType as TModule.
            yield m
        finally:
            set_freeze_enabled(old_setting)
            self._finalize_module(name, d)

    def _run_code(
        self, code: str, generator: type[CodeGenerator] | None, modname: str | None
    ) -> tuple[str, dict[str, Any]]:
        if modname is None:
            modname = self._temp_mod_name()
        compiled = self.compile(code, generator, modname)
        d = {"<builtins>": builtins.__dict__}
        add_fixed_module(d)
        exec(compiled, d)
        return modname, d

    def run_code(
        self,
        code: str,
        generator: type[CodeGenerator] | None = StaticCodeGenerator,
        modname: str | None = None,
    ) -> dict[str, Any]:
        _, r = self._run_code(code, generator, modname)
        return r

    @property
    def base_size(self) -> int:
        class C:
            __slots__ = ()

        # pyre-ignore[16]: Pyre is confused and thinks we're accessing attribute `C`
        # from `int`.
        return C().__sizeof__()

    @property
    def ptr_size(self) -> int:
        return 8 if sys.maxsize > 2**32 else 4

    def build_static_type(
        self, slots: tuple[str], slot_types: dict[str, str] | None
    ) -> type[object]:
        c = compile(
            f"""
__slots__ = {slots!r}
__slot_types__ = {slot_types!r}
        """,
            "exec",
            "exec",
        )
        return __build_cinder_class__(
            FunctionType(c, globals(), "C"), "C", None, False, (), False, ()
        )

    def assert_jitted(self, func: Callable[..., object]) -> None:
        if not cinderx.jit.is_enabled():
            return

        # Can't guarantee that functions will be called with arbitrary JitAuto settings.
        if not compiles_after_one_call():
            return

        self.assertTrue(cinderx.jit.is_jit_compiled(func), func.__name__)

    def assert_not_jitted(self, func: Callable[..., object]) -> None:
        if not cinderx.jit.is_enabled():
            return

        self.assertFalse(cinderx.jit.is_jit_compiled(func))

    def setUp(self) -> None:
        # ensure clean classloader/vtable slate for all tests
        cinderx.clear_classloader_caches()

        # Ensure our async tests don't change the event loop policy.  The default should
        # currently be None, as this is the setUp() call which runs early in the test
        # process.  We can't explicitly check though, as asyncio.get_event_loop_policy()
        # will initialize a proper default policy object. This leads to the test
        # framework complaining about environment changes.
        self.addCleanup(lambda: asyncio.set_event_loop_policy(None))

    def subTest(
        self, msg: str | None = None, **kwargs: object
    ) -> AbstractContextManager[object, bool]:
        cinderx.clear_classloader_caches()
        return super().subTest(msg=msg, **kwargs)

    def make_async_func_hot(self, func: Callable[[], Awaitable[object]]) -> None:
        async def make_hot() -> None:
            for _ in range(50):
                await func()

        asyncio.run(make_hot())

    def assertReturns(self, code: str, typename: str) -> None:
        actual = self.bind_final_return(code).name
        self.assertEqual(actual, typename)

    def bind_final_return(self, code: str) -> Value:
        mod, comp = self.bind_module(code)
        types = comp.modules["foo"].types
        # pyre-ignore[16]: Expecting a statement here.
        node = mod.body[-1].body[-1].value
        return types[node]

    def bind_stmt(
        self,
        code: str,
        optimize: bool = False,
        getter: Callable[[ast.stmt], ast.AST] = lambda stmt: stmt,
    ) -> tuple[Value, Compiler]:
        mod, comp = self.bind_module(code, optimize)
        types = comp.modules["foo"].types
        return types[getter(mod.body[-1])], comp

    def bind_expr(
        self,
        code: str,
        optimize: bool = False,
    ) -> tuple[Value, Compiler]:
        mod, comp = self.bind_module(code, optimize)
        types = comp.modules["foo"].types
        # pyre-ignore[16]: Expecting an expression here.
        return types[mod.body[-1].value], comp

    def bind_module(
        self,
        code: str,
        optimize: int = 0,
    ) -> tuple[ast.Module, Compiler]:
        tree = ast.parse(self.clean_code(code))

        compiler = Compiler(StaticCodeGenerator)
        tree, s = compiler._bind("foo", "foo.py", tree, code, optimize=optimize)

        # Make sure we can compile the code, just verifying all nodes are
        # visited.
        graph = StaticCodeGenerator.flow_graph("foo", "foo.py", s.scopes[tree])
        code_gen = StaticCodeGenerator(None, tree, s, graph, compiler, "foo", optimize)
        code_gen.visit(tree)

        return tree, compiler

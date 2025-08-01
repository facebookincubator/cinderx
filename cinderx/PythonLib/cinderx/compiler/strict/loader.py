# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

try:  # ensure all imports in this module are eager, to avoid cycles
    import _imp
    import builtins

    import importlib
    import marshal
    import os
    import sys

    from enum import Enum

    # pyre-ignore[21]: typeshed doesn't know about this
    from importlib import _bootstrap, _pack_uint32

    # pyre-ignore[21]: typeshed doesn't know about this
    from importlib._bootstrap_external import (
        _classify_pyc,
        _compile_bytecode,
        _validate_hash_pyc,
        _validate_timestamp_pyc,
    )
    from importlib.abc import Loader
    from importlib.machinery import (
        BYTECODE_SUFFIXES,
        EXTENSION_SUFFIXES,
        ExtensionFileLoader,
        FileFinder,
        ModuleSpec,
        SOURCE_SUFFIXES,
        SourceFileLoader,
        SourcelessFileLoader,
    )

    from importlib.util import cache_from_source, MAGIC_NUMBER
    from io import BytesIO
    from os import getenv, makedirs
    from os.path import dirname, isdir
    from py_compile import (
        _get_default_invalidation_mode,
        PycInvalidationMode,
        PyCompileError,
    )
    from types import CodeType, ModuleType
    from typing import Callable, cast, Collection, final, Iterable, Mapping

    from _cinderx import StrictModule, watch_sys_modules

    from cinderx.static import install_sp_audit_hook

    from ..consts import CI_CO_STATICALLY_COMPILED
    from .common import (
        DEFAULT_STUB_PATH,
        FIXED_MODULES,
        MAGIC_NUMBER as STRICT_MAGIC_NUMBER,
    )
    from .compiler import Compiler, Dependencies, SourceInfo, TIMING_LOGGER_TYPE
    from .flag_extractor import Flags
except BaseException:
    raise


_MAGIC_STRICT_OR_STATIC: bytes = (STRICT_MAGIC_NUMBER + 2**15).to_bytes(
    2, "little"
) + b"\r\n"
# We don't actually need to increment anything here, because the strict modules
# AST rewrite has no impact on pycs for non-strict modules. So we just always
# use two zero bytes. This simplifies generating "fake" strict pycs for
# known-not-to-be-strict third-party modules.
_MAGIC_NEITHER_STRICT_NOR_STATIC: bytes = (0).to_bytes(2, "little") + b"\r\n"
_MAGIC_LEN: int = len(_MAGIC_STRICT_OR_STATIC)


@final
class _PatchState(Enum):
    """Singleton used for tracking values which have not yet been patched."""

    Patched = 1
    Deleted = 2
    Unpatched = 3


# Unfortunately module passed in could be a mock object,
# which also has a `patch` method that clashes with the StrictModule method.
# Directly get the function to avoid name clash.


def _set_patch(module: StrictModule, name: str, value: object) -> None:
    type(module).patch(module, name, value)


def _del_patch(module: StrictModule, name: str) -> None:
    type(module).patch_delete(module, name)


@final
class StrictModuleTestingPatchProxy:
    """Provides a proxy object which enables patching of a strict module if the
    module has been loaded with the StrictSourceWithPatchingFileLoader.  The
    proxy can be used as a context manager in which case exiting the with block
    will result in the patches being disabled.  The process will be terminated
    if the patches are not unapplied and the proxy is deallocated."""

    def __init__(self, module: StrictModule) -> None:
        object.__setattr__(self, "module", module)
        object.__setattr__(self, "_patches", {})
        object.__setattr__(self, "__name__", module.__name__)
        object.__setattr__(
            self, "_final_constants", getattr(module, "__final_constants__", ())
        )
        # pyre-ignore[16]: pyre doesn't understand properties well enough
        if not type(module).__patch_enabled__.__get__(module, type(module)):
            raise ValueError(f"strict module {module} does not allow patching")

    def __setattr__(self, name: str, value: object) -> None:
        patches = object.__getattribute__(self, "_patches")
        prev_patched = patches.get(name, _PatchState.Unpatched)
        module = object.__getattribute__(self, "module")
        final_constants = object.__getattribute__(self, "_final_constants")
        if name in final_constants:
            raise AttributeError(
                f"Cannot patch Final attribute `{name}` of module `{module.__name__}`"
            )
        if value is prev_patched:
            # We're restoring the previous value
            del patches[name]
        elif prev_patched is _PatchState.Unpatched:
            # We're overwriting a value
            # only set patches[name] when name is patched for the first time
            patches[name] = getattr(module, name, _PatchState.Patched)

        if value is _PatchState.Deleted:
            _del_patch(module, name)
        else:
            _set_patch(module, name, value)

    def __delattr__(self, name: str) -> None:
        StrictModuleTestingPatchProxy.__setattr__(self, name, _PatchState.Deleted)

    def __getattribute__(self, name: str) -> object:
        res = getattr(object.__getattribute__(self, "module"), name)
        return res

    def __enter__(self) -> StrictModuleTestingPatchProxy:
        return self

    def __exit__(self, *excinfo: object) -> None:
        StrictModuleTestingPatchProxy.cleanup(self)

    def cleanup(self, ignore: Collection[str] | None = None) -> None:
        patches = object.__getattribute__(self, "_patches")
        module = object.__getattribute__(self, "module")
        for name, value in list(patches.items()):
            if ignore and name in ignore:
                del patches[name]
                continue
            if value is _PatchState.Patched:
                # value is patched means that module originally
                # does not contain this field.
                try:
                    _del_patch(module, name)
                except AttributeError:
                    pass
                finally:
                    del patches[name]
            else:
                setattr(self, name, value)
        assert not patches

    def __del__(self) -> None:
        patches = object.__getattribute__(self, "_patches")
        if patches:
            print(
                "Patch(es)",
                ", ".join(patches.keys()),
                "failed to be detached from strict module",
                "'" + object.__getattribute__(self, "module").__name__ + "'",
                file=sys.stderr,
            )
            # There's a test that depends on this being mocked out.
            os.abort()


__builtins__: ModuleType


class StrictBytecodeError(ImportError):
    pass


def classify_strict_pyc(
    data: bytes, name: str, exc_details: dict[str, str]
) -> tuple[int, bool]:
    # pyre-ignore[16]: typeshed doesn't know about this
    flags = _classify_pyc(data[_MAGIC_LEN:], name, exc_details)
    magic = data[:_MAGIC_LEN]
    if magic == _MAGIC_NEITHER_STRICT_NOR_STATIC:
        strict_or_static = False
    elif magic == _MAGIC_STRICT_OR_STATIC:
        strict_or_static = True
    else:
        raise StrictBytecodeError(
            f"Bad magic number {magic!r} in {exc_details['path']}"
        )
    return (flags, strict_or_static)


# A dependency is (name, mtime_and_size_or_hash)
DependencyTuple = tuple[str, bytes]


def validate_dependencies(
    compiler: Compiler, deps: tuple[DependencyTuple, ...], hash_based: bool
) -> None:
    """Raise ImportError if any dependency has changed."""
    for modname, data in deps:
        # empty invalidation data means non-static
        was_static = bool(data)
        source_info = compiler.get_source(
            modname, need_contents=(hash_based or not was_static)
        )
        if source_info is None:
            if was_static:
                raise ImportError(f"{modname} is missing")
            continue
        if was_static:
            expected_data = get_dependency_data(source_info, hash_based)
            if data != expected_data:
                raise ImportError(f"{modname} has changed")
        else:
            assert source_info.source is not None
            # if a previously non-static dependency is now found and has the text
            # "import __static__" in it, we invalidate
            if b"import __static__" in source_info.source:
                raise ImportError(f"{modname} may now be static")


def get_deps(
    deps: Dependencies | None, hash_based: bool
) -> tuple[DependencyTuple, ...]:
    ret = []
    if deps is None:
        return ()
    for source_info in deps.static:
        ret.append(
            (
                source_info.name,
                get_dependency_data(source_info, hash_based),
            )
        )
    for modname in deps.nonstatic:
        ret.append((modname, b""))
    return tuple(ret)


def get_dependency_data(source_info: SourceInfo, hash_based: bool) -> bytes:
    if hash_based:
        assert source_info.source is not None
        return importlib.util.source_hash(source_info.source)
    else:
        # pyre-ignore[16]: typeshed doesn't know about this
        return _pack_uint32(source_info.mtime) + _pack_uint32(source_info.size)


def code_to_strict_timestamp_pyc(
    code: CodeType,
    strict_or_static: bool,
    deps: Dependencies | None,
    mtime: int = 0,
    source_size: int = 0,
) -> bytearray:
    "Produce the data for a strict timestamp-based pyc."
    data = bytearray(
        _MAGIC_STRICT_OR_STATIC
        if strict_or_static
        else _MAGIC_NEITHER_STRICT_NOR_STATIC
    )
    data.extend(MAGIC_NUMBER)
    # pyre-ignore[16]: typeshed doesn't know about this
    data.extend(_pack_uint32(0))
    # pyre-ignore[16]: typeshed doesn't know about this
    data.extend(_pack_uint32(mtime))
    # pyre-ignore[16]: typeshed doesn't know about this
    data.extend(_pack_uint32(source_size))
    data.extend(marshal.dumps(get_deps(deps, hash_based=False)))
    data.extend(marshal.dumps(code))
    return data


def code_to_strict_hash_pyc(
    code: CodeType,
    strict_or_static: bool,
    deps: Dependencies | None,
    source_hash: bytes,
    checked: bool = True,
) -> bytearray:
    "Produce the data for a strict hash-based pyc."
    data = bytearray(
        _MAGIC_STRICT_OR_STATIC
        if strict_or_static
        else _MAGIC_NEITHER_STRICT_NOR_STATIC
    )
    data.extend(MAGIC_NUMBER)
    flags = 0b1 | checked << 1
    # pyre-ignore[16]: typeshed doesn't know about this
    data.extend(_pack_uint32(flags))
    assert len(source_hash) == 8
    data.extend(source_hash)
    data.extend(marshal.dumps(get_deps(deps, hash_based=True)))
    data.extend(marshal.dumps(code))
    return data


class StrictSourceFileLoader(SourceFileLoader):
    compiler: Compiler | None = None
    module: ModuleType | None = None

    def __init__(
        self,
        fullname: str,
        path: str,
        import_path: Iterable[str] | None = None,
        stub_path: str | None = None,
        allow_list_prefix: Iterable[str] | None = None,
        allow_list_exact: Iterable[str] | None = None,
        enable_patching: bool = False,
        log_source_load: Callable[[str, str | None, bool], None] | None = None,
        init_cached_properties: None
        | (
            Callable[
                [Mapping[str, str | tuple[str, bool]]],
                Callable[[type[object]], type[object]],
            ]
        ) = None,
        log_time_func: Callable[[], TIMING_LOGGER_TYPE] | None = None,
        use_py_compiler: bool = False,
        # The regexes are parsed on the C++ side, so re.Pattern is not accepted.
        allow_list_regex: Iterable[str] | None = None,
    ) -> None:
        self.name = fullname
        self.path = path
        self.import_path: Iterable[str] = import_path or list(sys.path)
        configured_stub_path = sys._xoptions.get("strict-module-stubs-path") or getenv(
            "PYTHONSTRICTMODULESTUBSPATH"
        )
        if stub_path is None:
            stub_path = configured_stub_path or DEFAULT_STUB_PATH
        if stub_path and not isdir(stub_path):
            raise ValueError(f"Strict module stubs path does not exist: {stub_path}")
        self.stub_path: str = stub_path
        self.allow_list_prefix: Iterable[str] = allow_list_prefix or []
        self.allow_list_exact: Iterable[str] = allow_list_exact or []
        self.allow_list_regex: Iterable[str] = allow_list_regex or []
        self.enable_patching = enable_patching
        self.log_source_load: Callable[[str, str | None, bool], None] | None = (
            log_source_load
        )
        self.bytecode_found = False
        self.bytecode_path: str | None = None
        self.init_cached_properties = init_cached_properties
        self.log_time_func = log_time_func
        self.use_py_compiler = use_py_compiler
        self.strict_or_static: bool = False
        self.is_static: bool = False

    @classmethod
    def ensure_compiler(
        cls,
        path: Iterable[str],
        stub_path: str,
        allow_list_prefix: Iterable[str],
        allow_list_exact: Iterable[str],
        log_time_func: Callable[[], TIMING_LOGGER_TYPE] | None,
        enable_patching: bool = False,
        allow_list_regex: Iterable[str] | None = None,
    ) -> Compiler:
        if (comp := cls.compiler) is None:
            comp = cls.compiler = Compiler(
                path,
                stub_path,
                allow_list_prefix,
                allow_list_exact,
                raise_on_error=True,
                log_time_func=log_time_func,
                enable_patching=enable_patching,
                allow_list_regex=allow_list_regex or [],
            )
        return comp

    def get_compiler(self) -> Compiler:
        return self.ensure_compiler(
            self.import_path,
            self.stub_path,
            self.allow_list_prefix,
            self.allow_list_exact,
            self.log_time_func,
            self.enable_patching,
            self.allow_list_regex,
        )

    def get_code(self, fullname: str) -> CodeType:
        source_path = self.get_filename(fullname)
        source_mtime = None
        source_bytes = None
        source_hash = None
        hash_based = False
        check_source = True
        try:
            bytecode_path = cache_from_source(source_path)
        except NotImplementedError:
            bytecode_path = None
        else:
            bytecode_path = self.bytecode_path = add_strict_tag(
                bytecode_path, self.enable_patching
            )
            try:
                st = self.path_stats(source_path)
            except OSError:
                pass
            else:
                source_mtime = int(st["mtime"])
                try:
                    data = self.get_data(bytecode_path)
                except OSError:
                    pass
                else:
                    self.bytecode_found = True
                    exc_details = {
                        "name": fullname,
                        "path": bytecode_path,
                    }
                    try:
                        flags, strict_or_static = classify_strict_pyc(
                            data, fullname, exc_details
                        )
                        self.strict_or_static = strict_or_static
                        # unmarshal dependencies
                        bytes_data = BytesIO(data)
                        bytes_data.seek(20)
                        deps = marshal.load(bytes_data)
                        hash_based = flags & 0b1 != 0
                        if hash_based:
                            check_source = flags & 0b10 != 0
                            if _imp.check_hash_based_pycs != "never" and (
                                check_source or _imp.check_hash_based_pycs == "always"
                            ):
                                source_bytes = self.get_data(source_path)
                                source_hash = importlib.util.source_hash(source_bytes)
                                # pyre-ignore[16]: typeshed doesn't know about this
                                _validate_hash_pyc(
                                    data[_MAGIC_LEN:],
                                    source_hash,
                                    fullname,
                                    exc_details,
                                )
                                if deps:
                                    validate_dependencies(
                                        self.get_compiler(), deps, hash_based=True
                                    )
                        else:
                            # pyre-ignore[16]: typeshed doesn't know about this
                            _validate_timestamp_pyc(
                                data[_MAGIC_LEN:],
                                source_mtime,
                                st["size"],
                                fullname,
                                exc_details,
                            )
                            if deps:
                                validate_dependencies(
                                    self.get_compiler(), deps, hash_based=False
                                )
                    except (ImportError, EOFError):
                        pass
                    else:
                        # pyre-ignore[16]: typeshed doesn't know about this
                        _bootstrap._verbose_message(
                            "{} matches {}", bytecode_path, source_path
                        )
                        # pyre-ignore[16]: typeshed doesn't know about this
                        return _compile_bytecode(
                            memoryview(data)[bytes_data.tell() :],
                            name=fullname,
                            bytecode_path=bytecode_path,
                            source_path=source_path,
                        )
        if source_bytes is None:
            source_bytes = self.get_data(source_path)
        code_object = self.source_to_code(source_bytes, source_path)
        # pyre-ignore[16]: typeshed doesn't know about this
        _bootstrap._verbose_message("code object from {}", source_path)
        if (
            not sys.dont_write_bytecode
            and bytecode_path is not None
            and source_mtime is not None
        ):
            if self.is_static:
                deps = self.get_compiler().get_dependencies(fullname)
            else:
                deps = None
            if hash_based:
                if source_hash is None:
                    source_hash = importlib.util.source_hash(source_bytes)
                data = code_to_strict_hash_pyc(
                    code_object,
                    self.strict_or_static,
                    deps,
                    source_hash,
                    check_source,
                )
            else:
                data = code_to_strict_timestamp_pyc(
                    code_object,
                    self.strict_or_static,
                    deps,
                    source_mtime,
                    len(source_bytes),
                )
            try:
                # pyre-ignore[16]: typeshed doesn't know about this
                self._cache_bytecode(source_path, bytecode_path, data)
            except NotImplementedError:
                pass
        return code_object

    def should_force_strict(self) -> bool:
        return False

    # pyre-fixme[14]: `source_to_code` overrides method defined in `InspectLoader`
    #  inconsistently.
    def source_to_code(
        self, data: bytes | str, path: str, *, _optimize: int = -1
    ) -> CodeType:
        log_source_load = self.log_source_load
        if log_source_load is not None:
            log_source_load(path, self.bytecode_path, self.bytecode_found)
        # pyre-ignore[28]: typeshed doesn't know about _optimize arg
        code = super().source_to_code(data, path, _optimize=_optimize)
        force = self.should_force_strict()
        if force or "__strict__" in code.co_names or "__static__" in code.co_names:
            # Since a namespace package will never call `source_to_code` (there
            # is no source!), there are only two possibilities here: non-package
            # (submodule_search_paths should be None) or regular package
            # (submodule_search_paths should have one entry, the directory
            # containing the "__init__.py").
            submodule_search_locations = None
            if path.endswith("__init__.py"):
                submodule_search_locations = [path[:12]]
            # Usually _optimize will be -1 (which means "default to the value
            # of sys.flags.optimize"). But this default happens very deep in
            # Python's compiler (in PyAST_CompileObject), so if we just pass
            # around -1 and rely on that, it means we can't make any of our own
            # decisions based on that flag. So instead we do the default right
            # here, so we have the correct optimize flag value throughout our
            # compiler.
            opt = sys.flags.optimize if _optimize == -1 else _optimize
            # Let the ast transform attempt to validate the strict module.  This
            # will return an unmodified module if import __strict__ isn't
            # actually at the top-level
            (
                code,
                is_valid_strict,
                is_static,
            ) = self.get_compiler().load_compiled_module_from_source(
                data,
                path,
                self.name,
                opt,
                submodule_search_locations,
                override_flags=Flags(is_strict=force),
            )
            self.strict_or_static = is_valid_strict or is_static
            self.is_static = is_static
            assert code is not None
            return code

        self.strict_or_static = False
        return code

    def exec_module(self, module: ModuleType) -> None:
        # This ends up being slightly convoluted, because create_module
        # gets called, then source_to_code gets called, so we don't know if
        # we have a strict module until after we were requested to create it.
        # So we'll run the module code we get back in the module that was
        # initially published in sys.modules, check and see if it's a strict
        # module, and then run the strict module body after replacing the
        # entry in sys.modules with a StrictModule entry.  This shouldn't
        # really be observable because no user code runs between publishing
        # the normal module in sys.modules and replacing it with the
        # StrictModule.
        code = self.get_code(module.__name__)
        if code is None:
            raise ImportError(
                f"Cannot import module {module.__name__}; get_code() returned None"
            )
        # fix up the pyc path
        cached = getattr(module, "__cached__", None)
        if cached:
            module.__cached__ = cached = add_strict_tag(cached, self.enable_patching)
        spec: ModuleSpec | None = module.__spec__
        if cached and spec and spec.cached:
            spec.cached = cached

        if self.strict_or_static:
            if spec is None:
                raise ImportError(f"Missing module spec for {module.__name__}")

            new_dict = {
                "<fixed-modules>": cast(object, FIXED_MODULES),
                "<builtins>": builtins.__dict__,
                "<init-cached-properties>": self.init_cached_properties,
            }
            if code.co_flags & CI_CO_STATICALLY_COMPILED:
                init_static_python()
                new_dict["<imported-from>"] = code.co_consts[-1]

            new_dict.update(module.__dict__)
            strict_mod = StrictModule(new_dict, self.enable_patching)

            sys.modules[module.__name__] = strict_mod

            exec(code, new_dict)
        else:
            exec(code, module.__dict__)


class StrictSourceFileLoaderWithPatching(StrictSourceFileLoader):
    def __init__(
        self,
        fullname: str,
        path: str,
        import_path: Iterable[str] | None = None,
        stub_path: str | None = None,
        allow_list_prefix: Iterable[str] | None = None,
        allow_list_exact: Iterable[str] | None = None,
        enable_patching: bool = True,
        log_source_load: Callable[[str, str | None, bool], None] | None = None,
        init_cached_properties: None
        | (
            Callable[
                [Mapping[str, str | tuple[str, bool]]],
                Callable[[type[object]], type[object]],
            ]
        ) = None,
        log_time_func: Callable[[], TIMING_LOGGER_TYPE] | None = None,
        use_py_compiler: bool = False,
        # The regexes are parsed on the C++ side, so re.Pattern is not accepted.
        allow_list_regex: Iterable[str] | None = None,
    ) -> None:
        super().__init__(
            fullname,
            path,
            import_path,
            stub_path,
            allow_list_prefix,
            allow_list_exact,
            enable_patching,
            log_source_load,
            init_cached_properties,
            log_time_func,
            use_py_compiler,
            allow_list_regex,
        )


def add_strict_tag(path: str, enable_patching: bool) -> str:
    base, __, ext = path.rpartition(".")
    enable_patching_marker = ".patch" if enable_patching else ""

    return f"{base}.strict{enable_patching_marker}.{ext}"


def _get_supported_file_loaders(
    enable_patching: bool = False,
) -> list[tuple[type[Loader], list[str]]]:
    """Returns a list of file-based module loaders.

    Each item is a tuple (loader, suffixes).
    """
    extensions = ExtensionFileLoader, EXTENSION_SUFFIXES
    source = (
        (
            StrictSourceFileLoaderWithPatching
            if enable_patching
            else StrictSourceFileLoader
        ),
        SOURCE_SUFFIXES,
    )
    bytecode = SourcelessFileLoader, BYTECODE_SUFFIXES
    return [extensions, source, bytecode]


def strict_compile(
    file: str,
    cfile: str,
    dfile: str | None = None,
    doraise: bool = False,
    optimize: int = -1,
    invalidation_mode: PycInvalidationMode | None = None,
    loader_override: object = None,
    loader_options: dict[str, str | int | bool] | None = None,
) -> str | None:
    """Byte-compile one Python source file to Python bytecode, using strict loader.

    :param file: The source file name.
    :param cfile: The target byte compiled file name.
    :param dfile: Purported file name, i.e. the file name that shows up in
        error messages.  Defaults to the source file name.
    :param doraise: Flag indicating whether or not an exception should be
        raised when a compile error is found.  If an exception occurs and this
        flag is set to False, a string indicating the nature of the exception
        will be printed, and the function will return to the caller. If an
        exception occurs and this flag is set to True, a PyCompileError
        exception will be raised.
    :param optimize: The optimization level for the compiler.  Valid values
        are -1, 0, 1 and 2.  A value of -1 means to use the optimization
        level of the current interpreter, as given by -O command line options.
    :return: Path to the resulting byte compiled file.

    Copied and modified from https://github.com/python/cpython/blob/3.6/Lib/py_compile.py#L65

    This version does not support cfile=None, since compileall never passes that.

    """
    modname = file
    for dir in sys.path:
        if file.startswith(dir):
            modname = file[len(dir) :]
            break

    modname = modname.replace("/", ".")
    if modname.endswith("__init__.py"):
        modname = modname[: -len("__init__.py")]
    elif modname.endswith(".py"):
        modname = modname[: -len(".py")]
    modname = modname.strip(".")

    if loader_options is None:
        loader_options = {}

    # TODO we ignore loader_override
    loader = StrictSourceFileLoader(
        modname,
        file,
        import_path=sys.path,
        **loader_options,
    )
    cfile = add_strict_tag(cfile, enable_patching=loader.enable_patching)
    source_bytes = loader.get_data(file)
    try:
        code = loader.source_to_code(source_bytes, dfile or file, _optimize=optimize)
        deps = loader.get_compiler().get_dependencies(modname)
    except Exception as err:
        raise
        py_exc = PyCompileError(err.__class__, err, dfile or file)
        if doraise:
            raise py_exc
        else:
            sys.stderr.write(py_exc.msg + "\n")
            return

    makedirs(dirname(cfile), exist_ok=True)

    if invalidation_mode is None:
        invalidation_mode = _get_default_invalidation_mode()
    if invalidation_mode == PycInvalidationMode.TIMESTAMP:
        source_stats = loader.path_stats(file)
        bytecode = code_to_strict_timestamp_pyc(
            code,
            loader.strict_or_static,
            deps,
            source_stats["mtime"],
            source_stats["size"],
        )
    else:
        source_hash = importlib.util.source_hash(source_bytes)
        bytecode = code_to_strict_hash_pyc(
            code,
            loader.strict_or_static,
            deps,
            source_hash,
            (invalidation_mode == PycInvalidationMode.CHECKED_HASH),
        )

    # pyre-ignore[16]: typeshed doesn't know about this
    loader._cache_bytecode(file, cfile, bytecode)
    return cfile


def init_static_python() -> None:
    """Idempotent global initialization of Static Python.

    Should be called at least once if any Static modules/functions exist.
    """
    watch_sys_modules()
    install_sp_audit_hook()


def install(enable_patching: bool = False) -> None:
    """Installs a loader which is capable of loading and validating strict modules"""
    supported_loaders = _get_supported_file_loaders(enable_patching)

    for index, hook in enumerate(sys.path_hooks):
        if not isinstance(hook, type):
            sys.path_hooks.insert(index, FileFinder.path_hook(*supported_loaders))
            break
    else:
        sys.path_hooks.insert(0, FileFinder.path_hook(*supported_loaders))

    # We need to clear the path_importer_cache so that our new FileFinder will
    # start being used for existing directories we've loaded modules from.
    sys.path_importer_cache.clear()


if __name__ == "__main__":
    install()
    del sys.argv[0]
    mod: object = __import__(sys.argv[0])
    if not isinstance(mod, StrictModule):
        raise TypeError(
            "compiler.strict.loader should be used to run strict modules: "
            + type(mod).__name__
        )
    mod.__main__()

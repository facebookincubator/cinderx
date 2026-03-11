# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

import sys
from importlib.abc import Loader
from importlib.machinery import (
    BYTECODE_SUFFIXES,
    EXTENSION_SUFFIXES,
    ExtensionFileLoader,
    FileFinder,
    SOURCE_SUFFIXES,
    SourcelessFileLoader,
)
from typing import Callable, Iterable, Mapping

from cinderx.compiler.static.pyrefly_compiler import PyreflyCompiler
from cinderx.compiler.static.pyrefly_type_binder import PyreflyTypeInfo

from .compiler import Compiler, TIMING_LOGGER_TYPE
from .loader import StrictSourceFileLoader


EMPTY_TYPE_INFO = PyreflyTypeInfo(
    {
        "type_table": [],
        "locations": [],
    }
)


class PyreflyLoader(StrictSourceFileLoader):
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
            comp = cls.compiler = PyreflyCompiler(
                type_info=EMPTY_TYPE_INFO,
                static_opt_out=None,
                static_opt_in=None,
                path=path,
                stub_path=stub_path,
                allow_list_prefix=allow_list_prefix,
                allow_list_exact=allow_list_exact,
                log_time_func=log_time_func,
                enable_patching=enable_patching,
                allow_list_regex=allow_list_regex or [],
            )
        return comp

    def should_force_strict(self) -> bool:
        return True


class PyreflyLoaderWithPatching(PyreflyLoader):
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


def _get_supported_file_loaders(
    enable_patching: bool = False,
) -> list[tuple[type[Loader], list[str]]]:
    """Returns a list of file-based module loaders.

    Each item is a tuple (loader, suffixes).
    """
    extensions = ExtensionFileLoader, EXTENSION_SUFFIXES
    source = (
        (PyreflyLoaderWithPatching if enable_patching else PyreflyLoader),
        SOURCE_SUFFIXES,
    )
    bytecode = SourcelessFileLoader, BYTECODE_SUFFIXES
    return [extensions, source, bytecode]


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

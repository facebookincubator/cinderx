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
from cinderx.compiler.static.pyrefly_info import Pyrefly

from .compiler import Compiler, TIMING_LOGGER_TYPE
from .loader import StrictSourceFileLoader


OPT_OUT_LIST: set[str] | None = None
OPT_IN_LIST: set[str] | None = None


class PyreflyLoader(StrictSourceFileLoader):
    pyrefly_type_dir: str | None = None

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
            if cls.pyrefly_type_dir is not None:
                pyrefly = Pyrefly(cls.pyrefly_type_dir)
            else:
                pyrefly = None
            comp = cls.compiler = PyreflyCompiler(
                static_opt_out=OPT_OUT_LIST,
                static_opt_in=OPT_IN_LIST,
                path=path,
                stub_path=stub_path,
                allow_list_prefix=allow_list_prefix,
                allow_list_exact=allow_list_exact,
                pyrefly=pyrefly,
                log_time_func=log_time_func,
                enable_patching=enable_patching,
                allow_list_regex=allow_list_regex or [],
            )
        return comp

    def should_force_strict(self) -> bool:
        if OPT_IN_LIST is not None:
            return self.name in OPT_IN_LIST
        if OPT_OUT_LIST is not None:
            return self.name not in OPT_OUT_LIST
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


def install(
    enable_patching: bool = False,
    pyrefly_type_dir: str | None = None,
    opt_in_list: set[str] | None = None,
    opt_out_list: set[str] | None = None,
) -> None:
    """Installs a loader which is capable of loading and validating strict modules"""
    PyreflyLoader.pyrefly_type_dir = pyrefly_type_dir
    supported_loaders = _get_supported_file_loaders(enable_patching)

    global OPT_IN_LIST, OPT_OUT_LIST
    OPT_IN_LIST = opt_in_list
    OPT_OUT_LIST = opt_out_list

    for index, hook in enumerate(sys.path_hooks):
        if not isinstance(hook, type):
            sys.path_hooks.insert(index, FileFinder.path_hook(*supported_loaders))
            break
    else:
        sys.path_hooks.insert(0, FileFinder.path_hook(*supported_loaders))

    # We need to clear the path_importer_cache so that our new FileFinder will
    # start being used for existing directories we've loaded modules from.
    sys.path_importer_cache.clear()

#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# pyre-unsafe

import os
import os.path
import subprocess

from concurrent.futures import ThreadPoolExecutor
from typing import Callable

from setuptools import Extension, find_packages, setup
from setuptools.command.build_ext import build_ext


def get_compile_workers() -> int:
    return os.cpu_count() or 1


class CinderBuildExt(build_ext):
    """Monkey-patch the ability to compile C++ files (but not C files) with
    -std=c++20 and perform compilation in parallel."""

    def build_extension(self, ext):
        old_compile_func = self.compiler.compile
        old__compile_func = self.compiler._compile
        max_workers = get_compile_workers()

        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            compilation_futures = []

            def new_compile(
                sources,
                output_dir=None,
                macros=None,
                include_dirs=None,
                debug=0,
                extra_preargs=None,
                extra_postargs=None,
                depends=None,
            ):
                r = old_compile_func(
                    sources,
                    output_dir,
                    macros,
                    include_dirs,
                    debug,
                    extra_preargs,
                    extra_postargs,
                    depends,
                )
                for fut in compilation_futures:
                    fut.result()
                return r

            def new__compile(obj, src, ext, cc_args, extra_postargs, pp_opts):
                if src.endswith(".cpp"):
                    cc_args = cc_args + ["-std=c++20"]
                compilation_futures.append(
                    executor.submit(
                        old__compile_func,
                        obj,
                        src,
                        ext,
                        cc_args,
                        extra_postargs,
                        pp_opts,
                    )
                )

            self.compiler.compile = new_compile
            self.compiler._compile = new__compile

            return super().build_extension(ext)


def find_files(directories: list[str], pred: Callable[[str], bool]) -> list[str]:
    sources = []
    for directory in directories:
        for root, dirs, files in os.walk(directory):
            # Ignore the build directory when scanning for CinderX files.  The projects
            # in the build directory are built separately and get linked in or included
            # as header files.
            for i in range(len(dirs)):
                if dirs[i] == "_build":
                    dirs.pop(i)
                    break

            for f in files:
                if pred(f):
                    sources.append(os.path.join(root, f))
    return sources


def find_source_files(directories: list[str]) -> list[str]:
    return find_files(
        directories,
        lambda f: f.endswith(".cpp") or f.endswith(".c"),
    )


def find_header_files(directories: list[str]) -> list[str]:
    return find_files(directories, lambda f: f.endswith(".h"))


def make_dir(working_dir: str, new_dir_name: str) -> str:
    new_dir = os.path.join(working_dir, new_dir_name)
    os.makedirs(new_dir, exist_ok=True)
    return new_dir


def run(*args, check: bool = True, **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(*args, check=check, encoding="utf-8", **kwargs)


def cmake(cwd: str, *args: str) -> None:
    command = ["cmake", "-S", cwd, "-B", cwd]
    command.extend(args)
    run(command)


def make(cwd: str) -> None:
    run(["make", "-j", str(get_compile_workers())], cwd=cwd)


def clone(project: str, version: str, cwd: str) -> str:
    # Ideally would use a shallow clone here but no guarantee as to how deep
    # `version` is into the history.
    #
    # Allow the clone to fail if the
    result = run(
        ["git", "clone", project],
        check=False,
        stderr=subprocess.PIPE,
        cwd=cwd,
    )
    exists = "already exists" in result.stderr
    if result.returncode != 0 and not exists:
        raise RuntimeError(f"Failed to clone {project}\n\nStderr:\n{result.stderr}")

    project_name = project.split("/")[-1]
    project_dir = os.path.join(cwd, project_name)

    if not exists:
        run(["git", "switch", "--detach", version], cwd=project_dir)

    return project_dir


def initialize_asmjit(working_dir: str) -> tuple[str, str]:
    project_dir = clone(
        "https://github.com/asmjit/asmjit",
        "2e93826348d6cd1325a8b1f7629e193c58332da9",
        working_dir,
    )

    cmake(project_dir, "-DASMJIT_STATIC=TRUE")
    make(project_dir)

    return (os.path.join(project_dir, "src"), project_dir)


def initialize_gtest(working_dir: str) -> tuple[str, str]:
    project_dir = clone(
        "https://github.com/google/googletest",
        "v1.15.2",
        working_dir,
    )

    # gtest tries to install itself by default.
    cmake(project_dir, "-DINSTALL_GTEST=OFF")
    make(project_dir)

    return (
        os.path.join(project_dir, "googletest", "include"),
        os.path.join(project_dir, "lib"),
    )


def initialize_fmt(working_dir: str) -> tuple[str, str]:
    project_dir = clone(
        "https://github.com/fmtlib/fmt",
        "11.0.2",
        working_dir,
    )

    # FMT_MASTER_PROJECT controls whether tests and other utilities are built.
    cmake(project_dir, "-DFMT_MASTER_PROJECT=OFF")
    make(project_dir)

    return (os.path.join(project_dir, "include"), project_dir)


def initialize_nlohmann(working_dir: str) -> str:
    project_dir = clone(
        "https://github.com/nlohmann/json",
        "b36f4c477c40356a0ae1204b567cca3c2a57d201",
        working_dir,
    )

    return os.path.join(project_dir, "single_include")


def main() -> None:
    top_dir = os.path.dirname(os.path.realpath(__file__))
    parent_dir = os.path.realpath(os.path.join(top_dir, ".."))

    build_dir = make_dir(top_dir, "_build")
    asmjit_include_dir, asmjit_library_dir = initialize_asmjit(build_dir)
    gtest_include_dir, gtest_library_dir = initialize_gtest(build_dir)
    fmt_include_dir, fmt_library_dir = initialize_fmt(build_dir)
    json_include_dir = initialize_nlohmann(build_dir)

    include_dirs = [
        # CinderX includes are prefixed with "cinderx/".
        parent_dir,
        # Dependencies.
        asmjit_include_dir,
        gtest_include_dir,
        fmt_include_dir,
        json_include_dir,
    ]

    library_dirs = [
        asmjit_library_dir,
        gtest_library_dir,
        fmt_library_dir,
    ]

    with open("README.md", encoding="utf-8") as fh:
        long_description = fh.read()

    cinderx_dirs = [top_dir]

    header_files = find_header_files(cinderx_dirs)
    source_files = find_source_files(cinderx_dirs)

    setup(
        name="cinderx",
        version="0.0.3",
        author="Meta Platforms, Inc.",
        author_email="cinder@meta.com",
        description="High-performance Python runtime extensions",
        long_description=long_description,
        long_description_content_type="text/markdown",
        url="https://github.com/facebookincubator/cinder",
        cmdclass={"build_ext": CinderBuildExt},
        ext_modules=[
            Extension(
                "_cinderx",
                sources=source_files,
                include_dirs=include_dirs,
                library_dirs=library_dirs,
                define_macros=[
                    # Not using parallel-hashmap simplifies things quite a bit.
                    ("JIT_FORCE_STL_CONTAINERS", None),
                    ("Py_BUILD_CORE", None),
                ],
                extra_compile_args=[
                    "-Wno-ambiguous-reversed-operator",
                ],
                depends=header_files,
            )
        ],
    )

    # Separate from the above so we can skip using CinderBuildExt. This behaves
    # weirdly for these extensions and it's not worth tracking down as this is
    # all very temporary.
    setup(
        name="cinderx_modules",
        version="0.0.1",
        author="Meta Platforms, Inc.",
        author_email="cinder@meta.com",
        description="High-performance Python runtime extension extensions",
        url="https://github.com/facebookincubator/cinder",
        ext_modules=[
            Extension(
                "_static",
                sources=["StaticPython/_static.c"],
                include_dirs=include_dirs,
                library_dirs=library_dirs,
                define_macros=[("Py_BUILD_CORE_MODULE", None)],
                depends=header_files,
            ),
            Extension(
                "_strictmodule",
                sources=["StrictModules/_strictmodule.c"],
                include_dirs=include_dirs,
                library_dirs=library_dirs,
                define_macros=[("Py_BUILD_CORE_MODULE", None)],
                depends=header_files,
            ),
            Extension(
                "xxclassloader",
                sources=["StaticPython/xxclassloader.c"],
                include_dirs=include_dirs,
                library_dirs=library_dirs,
                define_macros=[("Py_BUILD_CORE_MODULE", None)],
                depends=header_files,
            ),
        ],
        packages=find_packages(),
    )


if __name__ == "__main__":
    main()

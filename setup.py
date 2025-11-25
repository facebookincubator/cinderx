#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#
# pyre-unsafe
# @noautodeps

import os
import os.path
import shutil
import sys
import sysconfig

from typing import Callable

from setuptools import Extension, find_packages, setup
from setuptools.command.build import build as build
from setuptools.command.build_ext import build_ext
from setuptools.command.build_py import build_py

CHECKOUT_ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
SOURCE_DIR = os.path.join(CHECKOUT_ROOT_DIR, "cinderx")
PYTHON_LIB_DIR = os.path.join(SOURCE_DIR, "PythonLib")


def find_files(path: str, pred: Callable[[str], bool]) -> list[str]:
    result = []
    for root, dirs, files in os.walk(path):
        for f in files:
            if pred(f):
                # Files are returned relative to the top directory.
                abs_file = os.path.join(root, f)
                result.append(os.path.relpath(abs_file, path))
    return result


def find_native_sources(path: str) -> list[str]:
    return find_files(path, lambda f: f.endswith(".cpp") or f.endswith(".c"))


def find_python_sources(path: str) -> list[str]:
    return find_files(path, lambda f: f.endswith(".py"))


def compute_py_version() -> str:
    return f"{sys.version_info.major}.{sys.version_info.minor}"


class BuildCommand(build):
    # Don't use the setuptools default under "build/" as this clashes with
    # "build/fbcode_builder/" (auto-added to the OSS view of CinderX).
    def initialize_options(self):
        build.initialize_options(self)
        self.build_base = os.path.join(CHECKOUT_ROOT_DIR, "scratch")


class BuildPy(build_py):
    def run(self) -> None:
        # I have no idea what is supposed to set this up, but if it isn't set
        # we'll get an AttributeError at runtime.
        if not hasattr(self.distribution, "namespace_packages"):
            self.distribution.namespace_packages = None

        super().run()

        # Copy opcodes/${PY_VERSION}/opcode.py to cinderx/opcode.py.
        py_version = compute_py_version()
        out_path = self.get_module_outfile(self.build_lib, ["cinderx"], "opcode")
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        self.copy_file(
            os.path.join(PYTHON_LIB_DIR, f"opcodes/{py_version}/opcode.py"),
            out_path,
            preserve_mode=False,
        )

        # For OSS builds always surface the CinderX import errors
        dev_build_file = os.path.join(self.get_package_dir("cinderx"), ".dev_build")
        print(f"Writing .dev_build file to {dev_build_file}")
        with open(dev_build_file, "w") as f:
            f.write("\n")


class CMakeExtension(Extension):
    """
    Subclass to indicate to BuildExt that this should be handled specially.
    """

    pass


class BuildExt(build_ext):
    def run(self) -> None:
        # Partition into CMake extensions and everything else.
        cmake_extensions = []
        other_extensions = []
        # pyre-ignore[16]: No pyre types for build_ext.
        for extension in self.extensions:
            if isinstance(extension, CMakeExtension):
                cmake_extensions.append(extension)
            else:
                other_extensions.append(extension)

        # Handle CMake specially, leave everything else to super().
        for extension in cmake_extensions:
            self._run_cmake(extension)

        # pyre-ignore[16]: No pyre types for build_ext.
        self.extensions = other_extensions
        super().run()

    def _run_cmake(self, extension: CMakeExtension) -> None:
        # pyre-ignore[16]: No pyre types for build_ext.
        build_dir = self.build_temp
        os.makedirs(build_dir, exist_ok=True)

        # pyre-ignore[16]: No pyre types for build_ext.
        extension_dir = os.path.abspath(self.get_ext_fullpath(extension.name))
        os.makedirs(extension_dir, exist_ok=True)

        # Prefer Clang because that's what we develop against but some systems
        # including the manylinux build environment only have GCC.
        cc = self._find_binary(["clang", "gcc"])
        cxx = self._find_binary(["clang++", "g++"])

        build_type = os.environ.get("CMAKE_BUILD_TYPE", "RelWithDebInfo")
        cmake_args = [
            f"-DCMAKE_BUILD_TYPE={build_type}",
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={os.path.dirname(extension_dir)}",
            f"-DCMAKE_C_COMPILER={cc}",
            f"-DCMAKE_CXX_COMPILER={cxx}",
        ]

        options: dict[str, str] = {}

        def set_option(var: str, default: object) -> None:
            if type(default) == bool:
                default = int(default)
            if type(default) == int:
                default = str(default)
            if type(default) != str:
                raise ValueError(f"Not sure what to do with default value {default}")

            value = os.environ.get(var, default)
            options[var] = value

        # Python version is always the same as what's running setuptools.
        py_version = compute_py_version()
        options["PY_VERSION"] = py_version
        options["Python_ROOT_DIR"] = self._find_python()

        meta_python = "+meta" in sys.version
        linux = sys.platform == "linux"
        meta_312 = meta_python and py_version == "3.12"
        is_314 = py_version == "3.14"

        set_option("META_PYTHON", meta_python)
        set_option("ENABLE_ADAPTIVE_STATIC_PYTHON", meta_312)
        set_option("ENABLE_DISASSEMBLER", False)
        set_option("ENABLE_ELF_READER", linux)
        set_option("ENABLE_EVAL_HOOK", meta_312)
        set_option("ENABLE_FUNC_EVENT_MODIFY_QUALNAME", meta_312)
        set_option("ENABLE_GENERATOR_AWAITER", meta_312)
        set_option("ENABLE_INTERPRETER_LOOP", meta_312 or is_314)
        set_option("ENABLE_LAZY_IMPORTS", meta_312)
        set_option("ENABLE_LIGHTWEIGHT_FRAMES", meta_312)
        set_option("ENABLE_PARALLEL_GC", meta_312)
        set_option("ENABLE_PEP523_HOOK", meta_312 or is_314)
        set_option("ENABLE_PERF_TRAMPOLINE", meta_312)
        set_option("ENABLE_SYMBOLIZER", linux)
        set_option("ENABLE_USDT", linux)

        for name, value in options.items():
            cmake_args.append(f"-D{name}={value}")

        build_args = [
            "--config",
            build_type,
            "--",
            "-j",
            "8",
        ]

        # pyre-ignore[16]: No pyre types for build_ext.
        self.spawn(["cmake"] + cmake_args + ["-B", build_dir, CHECKOUT_ROOT_DIR])
        self.spawn(["cmake", "--build", build_dir] + build_args)

    def _find_binary(self, name_options: list[str]) -> str:
        for name in name_options:
            result = shutil.which(name)
            if result is not None:
                return result
        raise RuntimeError(f"Cannot find any binaries out of {name_options}")

    def _find_python(self) -> str:
        # Normally this would use "data", but that goes to a temporary build directory
        # under uv.  Work off of the include directory instead.
        include_dir = sysconfig.get_path("include")
        return os.path.join(include_dir, "..", "..")


def main() -> None:
    # Native sources.
    native_sources = find_native_sources(SOURCE_DIR)

    # Python sources.
    python_sources = find_python_sources(PYTHON_LIB_DIR)
    python_sources = [f for f in python_sources if "test_cinderx" not in f]

    sources = native_sources + python_sources

    setup(
        name="cinderx",
        description="High-performance Python runtime extension",
        url="https://www.github.com/facebookincubator/cinderx",
        version="0.1",
        classifers=[
            "Development Status :: 3 - Alpha",
            "Intended Audience :: Developers",
            "License :: OSI Approved :: MIT License",
        ],
        ext_modules=[
            CMakeExtension(
                name="_cinderx",
                sources=native_sources,
            ),
        ],
        sources=sources,
        cmdclass={
            "build": BuildCommand,
            "build_py": BuildPy,
            "build_ext": BuildExt,
        },
        packages=find_packages(where=PYTHON_LIB_DIR, exclude=["test_cinderx*"]),
        package_dir={"": PYTHON_LIB_DIR},
        package_data={"cinderx": [".dev_build"]},
        python_requires=">=3.14,<3.15",
    )


if __name__ == "__main__":
    main()

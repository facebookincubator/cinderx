# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

import argparse
import builtins
import importlib.util
import json
import marshal
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dis import dis
from types import CodeType
from typing import Pattern, TextIO

from . import get_disassembly_as_string
from .pycodegen import CinderCodeGenerator, compile_code, make_header
from .static import FIXED_MODULES, StaticCodeGenerator
from .static.pyrefly_info import Pyrefly
from .strict import StrictCodeGenerator

try:
    from cinder import StrictModule
except ImportError:
    StrictModule = None

# https://www.python.org/dev/peps/pep-0263/
coding_re: Pattern = re.compile(rb"^[ \t\f]*#.*?coding[:=][ \t]*([-_.a-zA-Z0-9]+)")


def open_with_coding(fname: str) -> TextIO:
    with open(fname, "rb") as f:
        line = f.readline()
        m = coding_re.match(line)
        if not m:
            line = f.readline()
            m = coding_re.match(line)
        encoding = "utf-8"
        if m:
            encoding = m.group(1).decode()
    return open(fname, encoding=encoding)


def run_pyrefly_check(
    input_path: str, search_paths: list[str], types_dir: str
) -> tuple[list[dict[str, str]], set[str]]:
    subprocess.run(
        [
            "pyrefly",
            "check",
            *[arg for p in search_paths for arg in ("--search-path", p)],
            "--report-cinderx",
            types_dir,
            input_path,
        ],
        check=True,
    )

    with open(os.path.join(types_dir, "index.json")) as f:
        index = json.load(f)
    modules_list = index.get("modules", index) if isinstance(index, dict) else index
    source_modules: set[str] = set()
    for entry in modules_list:
        if entry.get("path", "").endswith(".py"):
            name = entry.get("module_name") or entry.get("module")
            if name:
                source_modules.add(name)

    types_subdir = os.path.join(types_dir, "types")
    if not os.path.isdir(types_subdir):
        types_subdir = types_dir

    from cinderx.compiler.strict.pyrefly_loader import install as install_loader

    install_loader(pyrefly_type_dir=types_subdir, opt_in_list=source_modules)
    return modules_list, source_modules


def compile_pyrefly(
    source: str,
    input_path: str,
    input_dir: str,
    types_dir: str,
    modname: str,
    modules_list: list[dict[str, str]],
    source_modules: set[str],
    optimize: int,
) -> CodeType:
    from .static.pyrefly_compiler import PyreflyCompiler

    pyrefly_modname = modname
    for entry in modules_list:
        entry_path = entry.get("path", "")
        if entry_path and os.path.abspath(entry_path) == input_path:
            pyrefly_modname = entry.get("module_name") or entry.get("module") or modname
            break
    source_modules.add(pyrefly_modname)

    pyrefly = Pyrefly(types_dir)
    pyrefly_compiler = PyreflyCompiler(
        pyrefly=pyrefly,
        static_opt_in=source_modules,
        path=[input_dir] + sys.path,
    )
    result = pyrefly_compiler.load_compiled_module_from_source(
        source,
        input_path,
        pyrefly_modname,
        optimize=optimize,
    )
    codeobj = result[0]
    if codeobj is None:
        raise RuntimeError("Static compilation with pyrefly type data failed")
    return codeobj


def main() -> None:
    argparser = argparse.ArgumentParser(
        prog="compiler",
        description="Compile/execute a Python3 source file",
        epilog="""\
    By default, compile source code into in-memory code object and execute it.
    If -c is specified, instead of executing write .pyc file.
    """,
    )
    argparser.add_argument(
        "-c", action="store_true", help="compile into .pyc file instead of executing"
    )
    argparser.add_argument(
        "--dis", action="store_true", help="disassemble compiled code"
    )
    argparser.add_argument("--output", help="path to the output .pyc file")
    argparser.add_argument(
        "--modname",
        help="module name to compile as (default __main__)",
        default="__main__",
    )
    argparser.add_argument("input", help="source .py file")
    group = argparser.add_mutually_exclusive_group()
    group.add_argument(
        "--static", action="store_true", help="compile using static compiler"
    )
    group.add_argument(
        "--builtin", action="store_true", help="compile using built-in C compiler"
    )
    group.add_argument(
        "--pyrefly",
        action="store_true",
        help="run pyrefly type checker and execute with Static Python compilation",
    )
    argparser.add_argument(
        "--pyrefly-types-dir",
        help="directory for pyrefly type output (default: auto-created temp directory)",
    )
    argparser.add_argument(
        "--pyrefly-search-path",
        help="search path for pyrefly (default: directory of input file)",
    )
    argparser.add_argument(
        "--jit",
        action="store_true",
        help="enable CinderX JIT compiler",
    )
    argparser.add_argument(
        "--opt",
        action="store",
        type=int,
        default=-1,
        help="set optimization level to compile with",
    )
    argparser.add_argument("--strict", action="store_true", help="run in strict module")
    args = argparser.parse_args()

    with open_with_coding(args.input) as f:
        fileinfo = os.stat(args.input)
        source = f.read()

    pyrefly_cleanup_dir: str | None = None
    if args.pyrefly:
        input_path = os.path.abspath(args.input)
        input_dir = os.path.dirname(input_path)
        search_paths = [args.pyrefly_search_path or input_dir]
        static_spec = importlib.util.find_spec("__static__")
        if static_spec and static_spec.submodule_search_locations:
            static_parent = os.path.dirname(static_spec.submodule_search_locations[0])
            if static_parent not in search_paths:
                search_paths.append(static_parent)

        if args.pyrefly_types_dir:
            types_dir: str = args.pyrefly_types_dir
        else:
            types_dir = tempfile.mkdtemp(prefix="cinderx_pyrefly_")
            pyrefly_cleanup_dir = types_dir

        modules_list, source_modules = run_pyrefly_check(
            input_path, search_paths, types_dir
        )
        sys.path.insert(0, input_dir)

    try:
        if args.builtin:
            codeobj = compile(source, args.input, "exec")
            assert isinstance(codeobj, CodeType)
        elif args.pyrefly:
            codeobj = compile_pyrefly(
                source,
                input_path,
                input_dir,
                types_dir,
                args.modname,
                modules_list,
                source_modules,
                optimize=sys.flags.optimize if args.opt == -1 else args.opt,
            )
        else:
            if args.static and args.strict:
                raise ValueError("Cannot specify both --static and --strict options.")

            compiler = (
                StaticCodeGenerator
                if args.static
                else StrictCodeGenerator
                if args.strict
                else CinderCodeGenerator
            )

            codeobj = compile_code(
                source,
                args.input,
                "exec",
                optimize=args.opt,
                compiler=compiler,
                modname=args.modname,
            )

        if args.dis:
            print(get_disassembly_as_string(codeobj, recurse=True), end="")

        if args.c:
            if args.output:
                name = args.output
            else:
                name = args.input.rsplit(".", 1)[0] + ".pyc"
            with open(name, "wb") as f:
                hdr = make_header(int(fileinfo.st_mtime), fileinfo.st_size)
                f.write(importlib.util.MAGIC_NUMBER)
                f.write(hdr)
                marshal.dump(codeobj, f)
        else:
            if args.strict and StrictModule is not None:
                d: dict[str, object] = {"__name__": "__main__"}
                mod = StrictModule(d, False)
            else:
                mod = type(sys)("__main__")
                d = mod.__dict__
            if args.static or args.strict or args.pyrefly:
                d["<fixed-modules>"] = FIXED_MODULES
            d["<builtins>"] = builtins.__dict__
            sys.modules["__main__"] = mod
            # don't confuse the script with args meant for us
            sys.argv = sys.argv[:1]

            if args.jit:
                import cinderx.jit

                cinderx.jit.enable()
                cinderx.jit.auto()

            exec(codeobj, d, d)
    finally:
        if pyrefly_cleanup_dir:
            shutil.rmtree(pyrefly_cleanup_dir, ignore_errors=True)


main()

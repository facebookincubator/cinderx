# Copyright (c) Meta Platforms, Inc. and affiliates.

# Find symbols referenced in cinderx that are not exported by cpython.
#
# Helper script for upstream borrowing; the output of find_missing.py will be
# processed by callgraph.py, and the output of that pasted into
# borrowed.template.c
#
# NOTE: This has no dependencies, and can be run via buck run or simply as
#   $ fbpython find_missing.py missing.json

# pyre-strict
import json
import os
import os.path
import random
import subprocess
import sys
from collections import defaultdict
from typing import Iterable


# pyre-ignore[2]: Parameter `**kwargs` has no type specified.
def run(cmd: list[str], **kwargs) -> str:
    """Capture stdout, let stderr be displayed to show progress."""
    result = subprocess.Popen(cmd, text=True, stdout=subprocess.PIPE, **kwargs)
    ret, _ = result.communicate()
    return ret


def build(target: str) -> str:
    cmd = ["buck", "build", "-c", "cinderx.use_3_12=true", target, "--show-full-output"]
    return run(cmd).strip().split(" ")[-1]


def is_python_symbol(s: str) -> bool:
    return (" Py" in s) or (" _Py" in s)


def get_python_symbols(output: str) -> list[str]:
    lines = output.split("\n")
    return [x for x in lines if is_python_symbol(x)]


def get_cinderx_undef() -> set[str]:
    sh_file = build("fbcode//cinderx:python")
    d = os.path.dirname(os.path.dirname(sh_file))
    so_file = os.path.join(
        d,
        "PythonBin/__python__/out/install/lib/python3.12/lib-dynload/",
        "_cinderx.cpython-312-x86_64-linux-gnu.so",
    )
    undef = run(["nm", "-u", "--demangle", so_file])
    return {x.lstrip(" U") for x in get_python_symbols(undef)}


def get_cpython_exported() -> set[str]:
    so_file = build("fbsource//third-party/python/3.12:libpython[shared]")
    exported = run(["nm", "--dynamic", "--defined", so_file])
    exported = [x.split(" ", 3)[-1] for x in get_python_symbols(exported)]
    return set(exported)


def get_fbsource_root() -> str:
    try:
        fbsource_root = subprocess.run(
            ["hg", "root"], capture_output=True, encoding="utf-8", check=True
        ).stdout.strip()
    except subprocess.CalledProcessError as e:
        raise Exception(
            f"Failing stdout:\n{e.stdout}\nFailing stderr:\n{e.stderr}\n"
        ) from e
    return fbsource_root


def find_defs(syms: Iterable[str]) -> dict[str, list[str]]:
    # grep through the cpython source for the definition of each symbol.
    # We take advantage of the fact that the code is formatted like
    # void
    # fn(args)
    # {
    # so we can search for "^symbol" to distinguish definitions from function
    # calls.

    patfile = "/tmp/patterns-" + str(random.randint(0, 1024))
    with open(patfile, "w") as f:
        for sym in syms:
            f.write(f"^{sym}\\b\n")
    # We need to grep over the cpython base dir
    path = os.path.join(get_fbsource_root(), "third-party/python/3.12")
    ret = run(["grep", "-oHR", "-f", patfile, path])
    out = defaultdict(list)
    seen = set()  # make sure each symbol has a single definition
    for line in ret.strip().split("\n"):
        file, sym = line.split(":")
        if not file.endswith(".c"):
            continue
        if sym in seen:
            raise RuntimeError(f"Multiple definitions found for {sym}")
        seen.add(sym)
        out[file.lstrip("./")].append(sym)
    return out


def find_decls(syms: Iterable[str]) -> dict[str, list[int]]:
    # grep through the cpython headers for the declaration of each symbol.

    # NOTE: This uses ripgrep rather than grep (and hence relies on ripgrep
    # being installed locally), since grep cannot mix PCRE character classes
    # with a pattern file. If this is an issue, modify the pattern below to use
    # grep character classes instead.

    patfile = "/tmp/patterns-" + str(random.randint(0, 1024))
    print(patfile)
    with open(patfile, "w") as f:
        for sym in syms:
            f.write(rf"^(\w[\w\s*]*)?\b{sym}\b" + "\n")
    # We need to grep over the cpython base dir
    path = os.path.join(get_fbsource_root(), "third-party/python/3.12/Include")
    ret = run(["rg", "-n", "-f", patfile, path])
    if not ret:
        # No missing symbols found
        return {}
    out = defaultdict(list)
    for line in ret.strip().split("\n"):
        file, lineno, decl = line.split(":", 3)
        if not file.endswith(".h"):
            continue
        if decl.startswith(" ") or decl.startswith("PyAPI_"):
            continue
        out[file].append(int(lineno))
    return out


def find_missing_symbols() -> set[str]:
    undef = get_cinderx_undef()
    exported = get_cpython_exported()
    return undef - exported


def find_missing_defs() -> dict[str, list[str]]:
    missing = find_missing_symbols()
    return find_defs(missing)


def main() -> None:
    missing = find_missing_defs()
    if len(sys.argv) == 2:
        outfile = sys.argv[1]
        with open(outfile, "w") as f:
            json.dump(missing, f)
        print(f"Wrote missing symbols to {outfile}")
    else:
        for f, s in missing.items():
            print(f"{f}:")
            for sym in s:
                print(f"  {sym}")


if __name__ == "__main__":
    main()

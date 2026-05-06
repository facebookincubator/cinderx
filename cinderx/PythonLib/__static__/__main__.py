# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Run a Python script with Static Python compilation.

Usage: python -m __static__ script.py [args...]
"""

import importlib.util
import os
import sys


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: python -m __static__ <script.py> [args...]", file=sys.stderr)
        sys.exit(1)

    script = sys.argv[1]
    if not os.path.isfile(script):
        print(f"Error: {script!r} is not a file", file=sys.stderr)
        sys.exit(1)

    sys.argv[:] = sys.argv[1:]

    from cinderx.compiler.strict.loader import install, StrictSourceFileLoader

    install()

    script_dir = os.path.dirname(os.path.abspath(script))
    if script_dir not in sys.path:
        sys.path.insert(0, script_dir)

    loader = StrictSourceFileLoader("__main__", script)
    spec = importlib.util.spec_from_file_location("__main__", script, loader=loader)
    if spec is None:
        print(f"Error: could not load {script!r}", file=sys.stderr)
        sys.exit(1)

    module = importlib.util.module_from_spec(spec)
    sys.modules["__main__"] = module
    spec.loader.exec_module(module)


main()

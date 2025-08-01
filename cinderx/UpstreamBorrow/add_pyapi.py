# Copyright (c) Meta Platforms, Inc. and affiliates.

# Modify the cpython source to export all symbols
# referenced in cinderx

# NOTE: This can be run via `buck run` or from the cinderx directory as
#   $ fbpython -m UpstreamBorrow.add_pyapi
#
# NOTE: This relies on ripgrep being installed (see find_decls() in
# find_missing.py for reasons).

# pyre-strict
import re

from .find_missing import find_decls, find_missing_symbols


def find_missing_decls() -> dict[str, list[int]]:
    missing = find_missing_symbols()
    return find_decls(missing)


def transform(path: str, decls: list[int]) -> None:
    with open(path, "r") as f:
        lines = f.read().split("\n")

    for decl in decls:
        # We want zero-indexing
        decl = decl - 1
        split = False
        if lines[decl].startswith("_Py"):
            # We have a declaration split over two lines; the return type is on
            # the line before the function name.
            decl = decl - 1
            split = True
        line = lines[decl]

        if line.startswith("PyAPI_"):
            continue

        if line.startswith("extern "):
            line = line[7:]

        if split:
            line = f"PyAPI_FUNC({line.rstrip()})"
        else:
            if "(" in line:
                line = re.sub(r"(.*?)\s?(_Py\w+\()", r"PyAPI_FUNC(\1) \2", line)
            else:
                line = re.sub(r"(.*?)\s?(_Py\w+)", r"PyAPI_DATA(\1) \2", line)
        lines[decl] = line

    with open(path, "w") as f:
        f.write("\n".join(lines))


def main() -> None:
    missing = find_missing_decls()
    for path, decls in missing.items():
        _, relpath = path.split("3.12/")
        print(f"Processing {relpath}")
        transform(path, decls)


if __name__ == "__main__":
    main()

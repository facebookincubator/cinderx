# Copyright (c) Meta Platforms, Inc. and affiliates.

import sys

from cinderx import opcode

STATIC_OPNAMES: list[str] = [f"<{i}>" for i in range(256)]
STATIC_OPMAP: dict[str, int] = {}
opcode.init(
    STATIC_OPNAMES,
    STATIC_OPMAP,
    [],
    [],
    [],
    [],
    [],
    {},
    {},
    {},
)


template = """
// Copyright (c) Meta Platforms, Inc. and affiliates.

// 3.14 has a simple file that just defines the relavant ids:

#include "opcode.h"

// 0x200 to make sure we don't collide with pseudo instructions
#define EXTENDED_OPCODE_FLAG 0x200

"""


def main():
    if len(sys.argv) < 2:
        print("no file specified")
        sys.exit(1)

    res = [template]
    for name, op in STATIC_OPMAP.items():
        res.append(f"#define {name} ({op} | EXTENDED_OPCODE_FLAG)")

    with open(sys.argv[1], "w") as f:
        f.write("\n".join(res))
        f.write("\n")


if __name__ == "__main__":
    main()

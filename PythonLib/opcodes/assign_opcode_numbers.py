# Copyright (c) Meta Platforms, Inc. and affiliates.

# Assign opcode numbers for python 3.12

# Run from cinderx directory:
#  buck run PythonLib/opcodes:assign_opcode_numbers -- \
#    PythonLib/opcodes/opcode_312.py

import re
import sys

from . import cinderx_opcodes as cx


# From inspection of the 3.12 opcodes we have a large empty range starting
# here, so we can keep the cinderx opcodes contiguous.
#
# NOTE: When upgrading to python 3.13+ the easiest thing to do would be to find
# a similar range of unassigned numbers; if that is not possible, we will need
# to scrape opcodes from Include/opcode.h and fit our opcodes into the gaps.
START_NUM = 184
END_NUM = 236


HEADER = """
# Copyright (c) Meta Platforms, Inc. and affiliates.

# Generated via assign_opcode_numbers.py, do not edit.

# This is an addition to python/3.12/Lib/opcode.py, and is intended to be run
# via `exec` in generate_opcode_h.py with the globals dict obtained from
# running Lib/opcode.py.

# flake8: noqa


# Lib/opcode.py deletes these functions so we need to define them again here.
# We also need to update opname when we call def_op().
def def_op(name, op):
    opmap[name] = op
    opname[op] = name


def name_op(name, op):
    def_op(name, op)
    hasname.append(op)


def jrel_op(name, op):
    def_op(name, op)
    hasjrel.append(op)


def jabs_op(name, op):
    def_op(name, op)
    hasjabs.append(op)


""".lstrip()


def assign_numbers() -> list[str]:
    i = START_NUM
    out = []
    for name, flags in cx.CINDER_OPS.items():
        if flags & cx.NAME:
            f = "name_op"
        elif flags & cx.JREL:
            f = "jrel_op"
        elif flags & cx.JABS:
            f = "jabs_op"
        else:
            f = "def_op"
        i += 1
        if i > END_NUM:
            raise ValueError("Not enough free space for cinderx opcodes!")
        out.append(f'{f}("{name}", {i})')
        if flags & cx.CONST:
            out.append(f"hasconst.append({i})")
    return out


def main():
    if len(sys.argv) == 2:
        outfile = sys.argv[1]
    else:
        print("Usage:\n  fbpython assign_opcode_numbers.py <outfile>")
        sys.exit()

    out = assign_numbers()
    with open(outfile, "w") as f:
        f.write(HEADER)
        f.write("\n".join(out))
        f.write("\n")


if __name__ == "__main__":
    main()

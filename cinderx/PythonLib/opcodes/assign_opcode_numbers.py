# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

# Assign opcode numbers for python 3.12

# Run from cinderx directory:
#  buck run PythonLib/opcodes:assign_opcode_numbers -- \
#    PythonLib/opcodes/opcode_312.py

import opcode
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


HEADER: str = """
# Copyright (c) Meta Platforms, Inc. and affiliates.

# Generated via assign_opcode_numbers.py, do not edit.

# This is an addition to python/3.12/Lib/opcode.py, and is intended to be run
# via `exec` in generate_opcode_h.py with the globals dict obtained from
# running Lib/opcode.py.

# flake8: noqa


# Lib/opcode.py deletes these functions so we need to define them again here.
# We also need to update opname when we call def_op().
def init(
    opname,
    opmap,
    hasname,
    hasjrel,
    hasjabs,
    hasconst,
    hasarg,
    cache_format,
    specializations,
    inline_cache_entries,
    interp_only=False,
):
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


def process_opcode(
    name: str,
    flags: int,
    out: list[str],
    opcode_idx: int,
    cache_size: int = 0,
    cache_format: dict[str, int] | None = None,
    parent: str | None = None,
) -> None:
    if flags & cx.NAME:
        f = "name_op"
    elif flags & cx.JREL:
        f = "jrel_op"
    elif flags & cx.JABS:
        f = "jabs_op"
    else:
        f = "def_op"
    if flags & cx.IMPLEMENTED_IN_INTERPRETER:
        out.append(f'    {f}("{name}", {opcode_idx})')
        if flags & cx.CONST:
            out.append(f"    hasconst.append({opcode_idx})")
        if flags & (cx.ARG | cx.CONST):
            out.append(f"    hasarg.append({opcode_idx})")
        if cache_format is not None:
            out.append(f'    cache_format["{name}"] = "{cache_format}"')
        if parent:
            out.append(f'    if "{parent}" not in specializations:')
            out.append(f'        specializations["{parent}"] = []')
            out.append(f'    specializations["{parent}"].append("{name}")')
        if cache_size:
            if sys.version_info >= (3, 14):
                out.append(f'    inline_cache_entries["{name}"] = {cache_size}')
            else:
                out.append(f"    inline_cache_entries[{opcode_idx}] = {cache_size}")

    else:
        out.append("    if not interp_only:")
        out.append(f'        {f}("{name}", {opcode_idx})')
        if flags & cx.CONST:
            out.append(f"        hasconst.append({opcode_idx})")
        if flags & (cx.ARG | cx.CONST):
            out.append(f"        hasarg.append({opcode_idx})")


def assign_numbers312() -> list[str]:
    i = START_NUM
    out: list[str] = []

    def inc() -> None:
        nonlocal i
        i += 1
        if i == 229:
            # skip 229 which is used as an invalid byte code in test_code.
            i += 1
        if i > END_NUM:
            raise ValueError("Not enough free space for cinderx opcodes!")

    for name, val in cx.CINDER_OPS.items():
        inc()
        if isinstance(val, cx.Family):
            cache_size = sum(val.cache_format.values())
            process_opcode(name, val.flags, out, i, cache_size, val.cache_format)
            for specialization in val.specializations:
                inc()
                process_opcode(
                    specialization, val.flags, out, i, cache_size, parent=name
                )
        else:
            process_opcode(name, val, out, i)

    return out


def build_size_map() -> dict[int, list[int]]:
    size_by_name: dict[str, int] = {}

    def add_one(name: str, size_from: str) -> None:
        # pyre-ignore[16]: unknown attribute
        size_by_name[name] = opcode._inline_cache_entries.get(size_from, 0)

    # First add add the specialized opcodes based upon their parent
    # pyre-ignore[16]: unknown attribute
    for op, specializations in opcode._specializations.items():
        add_one(op, op)
        for specialization in specializations:
            add_one(specialization, op)

    # Then add all of the remaining non specialized opcodes
    for op in opcode.opname:
        if op == "POP_JUMP_IF_FALSE" or op == "POP_JUMP_IF_TRUE":
            continue
        if op not in size_by_name:
            add_one(op, op)

    size_groups: dict[int, list[int]] = {}
    for op, size in size_by_name.items():
        opnum = opcode.opname.index(op)
        if opnum > 255 or opnum == 0:
            continue
        if size not in size_groups:
            size_groups[size] = [opnum]
        else:
            size_groups[size].append(opnum)

    for val in size_groups.values():
        val.sort()
    return size_groups


def assign_numbers314() -> list[str]:
    size_groups: dict[int, list[int]] = build_size_map()
    start_num = 1
    out: list[str] = []

    def inc(cache_size: int, flags: int) -> int:
        group = size_groups[cache_size]
        i = None
        # Try and assign our extended opcodes into instructions with like
        # attributes so that things that naively look at extended ops see
        # the something similar to what they expect.
        for i, op in enumerate(group):
            if flags & (cx.JABS | cx.JREL):
                if op in opcode.hasjrel:
                    break
            elif flags & cx.NAME:
                if op in opcode.hasname:
                    break
            elif (
                op not in opcode.hasjrel
                and op not in opcode.hasname
                and op not in opcode.hasconst
            ):
                break
        else:
            raise NotImplementedError(
                f"Couldn't find compatible opcode: {name} {flags:x} {group} {opcode.hasjrel}"
            )

        assert i is not None
        res = group[i]
        del group[i]
        return res

    name: str
    for name, val in cx.CINDER_OPS.items():
        if name in ("JUMP_IF_ZERO_OR_POP", "JUMP_IF_NONZERO_OR_POP"):
            # special case - these are not used in 3.14.
            continue

        if isinstance(val, cx.Family):
            # TODO: Enable cache formats eventually
            cache_size = 0  # sum(val.cache_format.values())
            cache_format = {}  # val.cache_format

            process_opcode(
                name,
                val.flags,
                out,
                inc(cache_size, val.flags),
                cache_size,
                val.cache_format,
            )
            for specialization in val.specializations:
                process_opcode(
                    specialization,
                    val.flags,
                    out,
                    inc(cache_size, val.flags),
                    cache_size,
                    parent=name,
                )
        elif name == "POP_JUMP_IF_ZERO":
            process_opcode(name, val, out, opcode.opmap["POP_JUMP_IF_FALSE"], 1)
        elif name == "POP_JUMP_IF_NONZERO":
            process_opcode(name, val, out, opcode.opmap["POP_JUMP_IF_TRUE"], 1)
        else:
            process_opcode(name, val, out, inc(0, val))

    return out


def main() -> None:
    if len(sys.argv) != 2:
        print("Usage:\n  fbpython assign_opcode_numbers.py <outfile>")
        sys.exit()

    outfile = sys.argv[1]
    if sys.version_info >= (3, 14):
        out = assign_numbers314()
    else:
        out = assign_numbers312()
    with open(outfile, "w") as f:
        f.write(HEADER)
        f.write("\n".join(out))
        f.write("\n")


if __name__ == "__main__":
    main()

# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict


MANGLE_LEN = 256  # magic constant from compile.c


def mangle(name: str, klass: str | None) -> str:
    if klass is None:
        return name
    if not name.startswith("__"):
        return name
    if len(name) + 2 >= MANGLE_LEN:
        return name
    # TODO: Probably need to split and mangle recursively?
    if "." in name:
        return name
    if name.endswith("__"):
        return name
    try:
        i = 0
        while klass[i] == "_":
            i = i + 1
    except IndexError:
        return name
    klass = klass[i:]

    tlen = len(klass) + len(name)
    if tlen > MANGLE_LEN:
        klass = klass[: MANGLE_LEN - tlen]

    return "_{}{}".format(klass, name)

# Copyright (c) Meta Platforms, Inc. and affiliates.

# During a proper build, setup.py copies the version-specific opcode file here.
# On OSS builds running from source, this shim loads the correct version dynamically.

import importlib.util
import os
import sys

_is_oss = "+meta" not in sys.version and "+cinder" not in sys.version


def _find_opcode_module(opcodes_dir: str) -> str | None:
    current_version = (sys.version_info.major, sys.version_info.minor)
    candidates: list[tuple[tuple[int, int], str]] = []

    for entry in os.scandir(opcodes_dir):
        if not entry.is_dir():
            continue
        try:
            major, minor = map(int, entry.name.split("."))
        except ValueError:
            continue
        opcode_path = os.path.join(entry.path, "opcode.py")
        if os.path.exists(opcode_path):
            candidates.append(((major, minor), opcode_path))

    if not candidates:
        return None

    older_or_equal = [
        (version, path) for version, path in candidates if version <= current_version
    ]
    if older_or_equal:
        return max(older_or_equal, key=lambda item: item[0])[1]

    return min(candidates, key=lambda item: item[0])[1]


if _is_oss:
    _opcodes_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), "opcodes")
    _path = _find_opcode_module(_opcodes_dir)

    if _path is not None:
        _spec = importlib.util.spec_from_file_location(__name__, _path)
        _loader = _spec.loader
        _loader.exec_module(sys.modules[__name__])

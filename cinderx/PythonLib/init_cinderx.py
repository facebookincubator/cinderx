# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

import importlib.util

# `cinderx` is the Python module in fbcode/cinderx/PythonLib which wraps the
# `_cinderx` C++ extension in fbcode/cinderx/_cinderx.cpp.  Both need to be
# available to load CinderX.
if not (
    importlib.util.find_spec("cinderx") is None
    or importlib.util.find_spec("_cinderx") is None
):
    try:
        # pyre-ignore[21]: The cinderx module is an optional dependency here, its
        # existence is confirmed in the if statement above.
        import cinderx

        cinderx.init()
    except Exception as e:
        raise RuntimeError("Failed to initialize CinderX module") from e

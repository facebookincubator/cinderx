import importlib.util

# `cinderx` is the Python module in fbcode/cinderx/PythonLib which wraps the
# `_cinderx` C++ extension in fbcode/cinderx/_cinderx.cpp.  Both need to be
# available to load CinderX.
if not (
    importlib.util.find_spec("cinderx") is None
    or importlib.util.find_spec("_cinderx") is None
):
    try:
        import cinderx

        cinderx.init()
    except Exception as e:
        raise RuntimeError("Failed to initialize CinderX module") from e

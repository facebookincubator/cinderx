# pyre-ignore-all-errors
def f():
    try:
        pass
    except KeyError:
        try:
            pass
        finally:
            pass
        return is_enabled

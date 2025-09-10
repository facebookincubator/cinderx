# pyre-ignore-all-errors
def f():
    try:
        for el in x:
            break
    except Exception:
        pass

    foo()

# pyre-ignore-all-errors
def f():
    from x import y
    try:
        y()
    except NotImplementedError:
        pass
    else:
        from x import z

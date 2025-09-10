# pyre-ignore-all-errors
def f():
    if not abc:
        try:
            pass
        except ValueError:
            pass
        else:
            if x:
                pass
    else:
        pass
    # does the body have a fixed length? (of zero)
    if y:
        pass

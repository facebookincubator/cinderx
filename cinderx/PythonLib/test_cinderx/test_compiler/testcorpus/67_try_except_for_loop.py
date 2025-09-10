# pyre-ignore-all-errors
def f(y):
    try:
        pass
    except TypeError:
        for x in y:
            break

    x()
   
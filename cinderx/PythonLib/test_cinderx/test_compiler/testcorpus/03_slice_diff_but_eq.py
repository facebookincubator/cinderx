# pyre-ignore-all-errors
def f():
    x[:0] = y
    x[:0.0] = y
    x[:False] = y
    x[:None] = y

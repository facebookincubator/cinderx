# pyre-ignore[3]: Returning None
def test():
    global x
    # pyre-ignore[10]: Name `x` is used but not defined in the current scope.
    x = {}
    [y for y in x if x]

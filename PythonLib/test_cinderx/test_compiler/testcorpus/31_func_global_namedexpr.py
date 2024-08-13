# pyre-ignore[3]: Returning None
def f():
    global GLOBAL_VAR
    [GLOBAL_VAR := 42 for _ in range(1)]

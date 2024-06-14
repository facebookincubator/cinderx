# pyre-ignore-all-errors
def f():
    return 42
# EXPECTED:
[
    ...,
    CODE_START('f'),
    RETURN_CONST(42),
]

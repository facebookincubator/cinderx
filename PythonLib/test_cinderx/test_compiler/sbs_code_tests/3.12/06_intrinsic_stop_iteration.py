# pyre-ignore-all-errors
def f():
    yield 42
# EXPECTED:
[
    RESUME(0),
    SETUP_CLEANUP(None),
    ...,
    LOAD_CONST(42),
    YIELD_VALUE(0),
    RESUME(1),
    POP_TOP(0),
    RETURN_CONST(None),
    CALL_INTRINSIC_1(3),
    RERAISE(1),
]

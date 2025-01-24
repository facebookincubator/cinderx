# pyre-ignore-all-errors
async def f():
    yield 42
# EXPECTED:
[
    ...,
    LOAD_CONST(42),
    CALL_INTRINSIC_1(4),
    YIELD_VALUE(0),
    RESUME(1),
    ...
]

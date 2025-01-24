# pyre-ignore-all-errors
def f():
    yield 42
# EXPECTED:
[
    ...,

    __BLOCK__('start: 1'),
    LOAD_CONST(42),
    YIELD_VALUE(0),
    RESUME(1),
    POP_TOP(0),
    RETURN_CONST(None),

    __BLOCK__('handler: 2'),
    CALL_INTRINSIC_1(3),
    RERAISE(1),
]

# pyre-ignore-all-errors
(await g() for _ in range(10))
# EXPECTED:
[
    ...,
    LOAD_GLOBAL(('g', 1)),
    CALL(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    SEND(Block(6)),
    YIELD_VALUE(0),
    ...,
]

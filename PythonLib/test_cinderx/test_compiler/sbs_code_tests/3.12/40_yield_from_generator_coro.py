# pyre-ignore-all-errors
(await g() for _ in range(10))
# EXPECTED:
[
    ...,
    LOAD_GLOBAL('g'),
    CALL(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    SEND(Block(5)),
    SETUP_FINALLY(Block(4)),
    YIELD_VALUE(0),
    ...,
]

# pyre-ignore-all-errors
(await g() for _ in range(10))
# EXPECTED:
[
    ...,
    LOAD_GLOBAL('g'),
    CALL_FUNCTION(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    YIELD_FROM(0),
    ...,
]

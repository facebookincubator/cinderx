# pyre-ignore-all-errors
def f():
    yield from g
# EXPECTED:
[
    ...,
    LOAD_GLOBAL('g'),
    GET_YIELD_FROM_ITER(0),
    LOAD_CONST(None),
    YIELD_FROM(0),
    ...,
]

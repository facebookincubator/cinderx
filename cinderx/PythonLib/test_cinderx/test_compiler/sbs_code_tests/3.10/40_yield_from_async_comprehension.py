# pyre-ignore-all-errors
(i async for i in aiter())
# EXPECTED:
[
    ...,
    YIELD_FROM(0),
    POP_BLOCK(0),
    STORE_FAST('i'),
    LOAD_FAST('i'),
    YIELD_VALUE(0),
    ...,
]

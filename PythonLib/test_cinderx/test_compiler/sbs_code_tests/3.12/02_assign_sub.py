# pyre-ignore-all-errors
x[y] += z
# EXPECTED:
[
    ...,
    LOAD_NAME('x'),
    LOAD_NAME('y'),
    COPY(2),
    COPY(2),
    BINARY_SUBSCR(0),
    LOAD_NAME('z'),
    BINARY_OP(13),
    SWAP(3),
    SWAP(2),
    STORE_SUBSCR(0),
    ...
]

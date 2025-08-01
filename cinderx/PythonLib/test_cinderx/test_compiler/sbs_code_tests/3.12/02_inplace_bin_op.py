# pyre-ignore-all-errors
x += y
# EXPECTED:
[
    ...,
    LOAD_NAME('x'),
    LOAD_NAME('y'),
    BINARY_OP(13),
    STORE_NAME('x'),
    ...,
]

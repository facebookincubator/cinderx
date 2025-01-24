# pyre-ignore-all-errors
x = y = 1
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    LOAD_CONST(1),
    COPY(1),
    STORE_NAME('x'),
    STORE_NAME('y'),
    ...,
]

def f():
    a = (x:= 42)
# EXPECTED:
[
    ...,
    CODE_START('f'),

    # __BLOCK__('entry: 0'),
    ...,
    LOAD_CONST(42),
    COPY(1),
    STORE_FAST('x'),
    STORE_FAST('a'),
    ...,
]

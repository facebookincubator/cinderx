# pyre-ignore-all-errors
def f():
    (some_global): int
    print(some_global)

# EXPECTED:
[
    ...,
    LOAD_CONST(Code(('f'))),
    LOAD_CONST('f'),
    MAKE_FUNCTION(0),
    STORE_NAME('f'),
    ...,
    CODE_START('f'),
    ~LOAD_CONST('int'),
]

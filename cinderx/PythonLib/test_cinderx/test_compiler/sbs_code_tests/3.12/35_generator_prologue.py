# pyre-ignore-all-errors
def foo():
    yield 42

# EXPECTED:
[
    ...,

    CODE_START('foo'),
    RETURN_GENERATOR(0),
    POP_TOP(0),
    RESUME(0),
    ...,
]

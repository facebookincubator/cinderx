# pyre-ignore-all-errors
foo.fun()
# EXPECTED:
[
    ...,
    LOAD_ATTR(('fun', 1)),
    CALL(0),
    ...,
]

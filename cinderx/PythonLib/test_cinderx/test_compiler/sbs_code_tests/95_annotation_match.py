# pyre-ignore-all-errors
match f:
    case _:
        x: int = 3
# EXPECTED:
[
    ...,
    SETUP_ANNOTATIONS(0),
    ...
]

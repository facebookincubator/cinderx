# pyre-ignore-all-errors
class F:
    class D:
        x: int = 42
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ~SETUP_ANNOTATIONS(0),
    LOAD_CONST('F.D'),
    ...,
]

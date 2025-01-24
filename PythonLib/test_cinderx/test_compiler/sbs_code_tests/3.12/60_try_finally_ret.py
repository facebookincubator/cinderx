# pyre-ignore-all-errors
def f():
    try:
        a
    finally:
        return 42
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('f'),

    ...,

    # __BLOCK__('try_finally_end: 4'),
    PUSH_EXC_INFO(0),
    POP_TOP(0),
    POP_EXCEPT(0),
    RETURN_CONST(42),
    
    ...,
]

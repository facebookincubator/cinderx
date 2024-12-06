# pyre-ignore-all-errors
try:
    pass
finally:
    if x:
        y = x
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,

    POP_JUMP_IF_FALSE(Block(9)),

    # __BLOCK__(': 2'),
    LOAD_NAME('x'),
    STORE_NAME('y'),

    # __BLOCK__('if_end: 3'),
    RETURN_CONST(None),

    # __BLOCK__(': 9'),
    RETURN_CONST(None),

    # __BLOCK__('try_finally_end: 4'),
    PUSH_EXC_INFO(0),
    LOAD_NAME('x'),
    POP_JUMP_IF_FALSE(Block(10)),

    # __BLOCK__(': 5'),
    LOAD_NAME('x'),
    STORE_NAME('y'),

    # __BLOCK__('if_end: 6'),
    RERAISE(0),

    # __BLOCK__(': 10'),
    RERAISE(0),

    # __BLOCK__('cleanup: 7'),
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),
]

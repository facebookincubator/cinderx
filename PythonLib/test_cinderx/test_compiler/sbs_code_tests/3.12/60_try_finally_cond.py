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

    POP_JUMP_IF_FALSE(Block(9, label='if_end copied')),

    # __BLOCK__(': 5'),
    LOAD_NAME('x'),
    STORE_NAME('y'),

    # __BLOCK__('if_end: 6'),
    RERAISE(0),

    # __BLOCK__('if_end copied: 9'),
    RERAISE(0),

    # __BLOCK__('cleanup: 7'),
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),
]

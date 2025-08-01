# pyre-ignore-all-errors
def f():
    x = 10
    while x > 3:
        x -= 1

# EXPECTED:
[
    ...,

    # __BLOCK__('start: 1'),
    LOAD_CONST(10),
    STORE_FAST('x'),

    __BLOCK__('while_loop: 2'),
    LOAD_FAST('x'),
    LOAD_CONST(3),
    COMPARE_OP('>'),
    POP_JUMP_IF_FALSE(Block(8)),

    __BLOCK__('while_body: 4'),
    LOAD_FAST('x'),
    LOAD_CONST(1),
    BINARY_OP(23),
    STORE_FAST('x'),
    LOAD_FAST('x'),
    LOAD_CONST(3),
    COMPARE_OP('>'),
    POP_JUMP_IF_FALSE(Block(7, label='while_after')),

    __BLOCK__('backwards_jump: 9'),
    JUMP_BACKWARD(Block(4, label='while_body')),

    __BLOCK__('while_after: 7'),
    RETURN_CONST(None),

    __BLOCK__(': 8'),
    RETURN_CONST(None),
]

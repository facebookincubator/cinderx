# pyre-ignore-all-errors
async def f():
    async with foo as bar:
        pass
# EXPECTED:
[
    ...,
    CODE_START('f'),

    __BLOCK__('entry: 0'),
    RETURN_GENERATOR(0),
    POP_TOP(0),
    RESUME(0),
    LOAD_GLOBAL('foo'),
    BEFORE_ASYNC_WITH(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),

    __BLOCK__('send: 1'),
    SEND(Block(3, label='exit')),
    SETUP_FINALLY(Block(2, label='fail')),
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(1, label='send')),

    __BLOCK__('exit: 3'),
    END_SEND(0),
    SETUP_WITH(Block(8, label='with_finally')),

    __BLOCK__('with_block: 4'),
    STORE_FAST('bar'),
    POP_BLOCK(0),
    LOAD_CONST(None),
    LOAD_CONST(None),
    LOAD_CONST(None),
    CALL(2),
    GET_AWAITABLE(0),
    LOAD_CONST(None),

    __BLOCK__('send: 5'),
    SEND(Block(7, label='exit')),
    SETUP_FINALLY(Block(6, label='fail')),
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(5, label='send')),

    __BLOCK__('exit: 7'),
    END_SEND(0),
    POP_TOP(0),
    JUMP_FORWARD(Block(16, label='with_exit')),

    __BLOCK__('with_exit: 16'),
    RETURN_CONST(None),
    CALL_INTRINSIC_1(3),
    RERAISE(1),

    __BLOCK__('fail: 2'),
    CLEANUP_THROW(0),

    __BLOCK__('explicit_jump: 17'),
    JUMP_BACKWARD(Block(3, label='exit')),

    __BLOCK__('fail: 6'),
    CLEANUP_THROW(0),

    __BLOCK__('explicit_jump: 18'),
    JUMP_BACKWARD(Block(7, label='exit')),

    __BLOCK__('with_finally: 8'),
    SETUP_CLEANUP(Block(14, label='cleanup')),
    PUSH_EXC_INFO(0),
    WITH_EXCEPT_START(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),

    __BLOCK__('send: 9'),
    SEND(Block(11, label='exit')),
    SETUP_FINALLY(Block(10, label='fail')),
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(9, label='send')),

    __BLOCK__('fail: 10'),
    CLEANUP_THROW(0),

    __BLOCK__('exit: 11'),
    END_SEND(0),
    POP_JUMP_IF_TRUE(Block(13, label='suppress')),

    __BLOCK__(': 12'),
    RERAISE(2),

    __BLOCK__('suppress: 13'),
    POP_TOP(0),
    POP_BLOCK(0),
    POP_EXCEPT(0),
    POP_TOP(0),
    POP_TOP(0),
    JUMP_BACKWARD(Block(16, label='with_exit')),

    __BLOCK__('cleanup: 14'),
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),
    ...,
]

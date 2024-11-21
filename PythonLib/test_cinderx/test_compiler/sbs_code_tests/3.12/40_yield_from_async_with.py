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

    __BLOCK__('start: 1'),
    LOAD_GLOBAL('foo'),
    BEFORE_ASYNC_WITH(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),

    __BLOCK__('send: 2'),
    SEND(Block(4, label='exit')),
    YIELD_VALUE(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(2, label='send')),

    __BLOCK__('exit: 4'),
    END_SEND(0),

    __BLOCK__('with_block: 5'),
    STORE_FAST('bar'),
    NOP(0),
    LOAD_CONST(None),
    LOAD_CONST(None),
    LOAD_CONST(None),
    CALL(2),
    GET_AWAITABLE(0),
    LOAD_CONST(None),

    __BLOCK__('send: 6'),
    SEND(Block(8, label='exit')),
    YIELD_VALUE(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(6, label='send')),

    __BLOCK__('exit: 8'),
    END_SEND(0),
    POP_TOP(0),
    RETURN_CONST(None),

    __BLOCK__('fail: 3'),
    CLEANUP_THROW(0),

    __BLOCK__('explicit_jump: 19'),
    JUMP_BACKWARD(Block(4, label='exit')),

    __BLOCK__('fail: 7'),
    CLEANUP_THROW(0),

    __BLOCK__('explicit_jump: 20'),
    JUMP_BACKWARD(Block(8, label='exit')),

    __BLOCK__('with_finally: 9'),
    PUSH_EXC_INFO(0),
    WITH_EXCEPT_START(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),

    __BLOCK__('send: 10'),
    SEND(Block(12, label='exit')),
    YIELD_VALUE(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(10, label='send')),

    __BLOCK__('fail: 11'),
    CLEANUP_THROW(0),

    __BLOCK__('exit: 12'),
    END_SEND(0),
    POP_JUMP_IF_TRUE(Block(14, label='suppress')),

    __BLOCK__(': 13'),
    RERAISE(2),

    __BLOCK__('suppress: 14'),
    POP_TOP(0),
    POP_EXCEPT(0),
    POP_TOP(0),
    POP_TOP(0),
    RETURN_CONST(None),

    __BLOCK__('cleanup: 15'),
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),

    __BLOCK__('handler: 18'),
    CALL_INTRINSIC_1(3),
    RERAISE(1),
]

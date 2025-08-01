# pyre-ignore-all-errors
async def f():
    async with foo as bar:
        pass
# EXPECTED:
[
     __BLOCK__('entry: 0'),
    ...,
    CODE_START('f'),

     __BLOCK__('entry: 0'),
    RETURN_GENERATOR(0),
    POP_TOP(0),
    RESUME(0),

     __BLOCK__('start: 1'),
    LOAD_GLOBAL('foo'),
    BEFORE_ASYNC_WITH(0),
    GET_AWAITABLE(1),
    LOAD_CONST(None),

     __BLOCK__('send: 2'),
    SEND(Block(5, label='exit')),

     __BLOCK__('post send: 3'),
    YIELD_VALUE(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(2, label='send')),

     __BLOCK__('exit: 5'),
    END_SEND(0),

     __BLOCK__('with_block: 6'),
    STORE_FAST('bar'),
    NOP(0),
    LOAD_CONST(None),
    LOAD_CONST(None),
    LOAD_CONST(None),
    CALL(2),
    GET_AWAITABLE(2),
    LOAD_CONST(None),

     __BLOCK__('send: 7'),
    SEND(Block(10, label='exit')),

     __BLOCK__('post send: 8'),
    YIELD_VALUE(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(7, label='send')),

     __BLOCK__('exit: 10'),
    END_SEND(0),
    POP_TOP(0),
    RETURN_CONST(None),

     __BLOCK__('fail: 4'),
    CLEANUP_THROW(0),

     __BLOCK__('explicit_jump: 22'),
    JUMP_BACKWARD(Block(5, label='exit')),

     __BLOCK__('fail: 9'),
    CLEANUP_THROW(0),

     __BLOCK__('explicit_jump: 23'),
    JUMP_BACKWARD(Block(10, label='exit')),

     __BLOCK__('with_finally: 11'),
    PUSH_EXC_INFO(0),
    WITH_EXCEPT_START(0),
    GET_AWAITABLE(2),
    LOAD_CONST(None),

     __BLOCK__('send: 12'),
    SEND(Block(15, label='exit')),

     __BLOCK__('post send: 13'),
    YIELD_VALUE(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(12, label='send')),

     __BLOCK__('fail: 14'),
    CLEANUP_THROW(0),

     __BLOCK__('exit: 15'),
    END_SEND(0),
    POP_JUMP_IF_TRUE(Block(17, label='suppress')),

     __BLOCK__(': 16'),
    RERAISE(2),

     __BLOCK__('suppress: 17'),
    POP_TOP(0),
    POP_EXCEPT(0),
    POP_TOP(0),
    POP_TOP(0),
    RETURN_CONST(None),

     __BLOCK__('cleanup: 18'),
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),

     __BLOCK__('handler: 21'),
    CALL_INTRINSIC_1(3),
    RERAISE(1),
]

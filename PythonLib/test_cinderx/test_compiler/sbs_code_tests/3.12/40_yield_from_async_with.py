# pyre-ignore-all-errors
async def f():
    async with foo as bar:
        pass
# EXPECTED:
[
    ...,
    CODE_START('f'),
    # Block 0 entry
    RETURN_GENERATOR(0),
    POP_TOP(0),
    RESUME(0),
    LOAD_GLOBAL('foo'),
    BEFORE_ASYNC_WITH(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    # Block 1 send
    SEND(Block(3)),
    SETUP_FINALLY(Block(2)),
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(1)),
    # Block 3 exit
    END_SEND(0),
    SETUP_WITH(Block(8)),
    # Block 4 with_block
    STORE_FAST('bar'),
    POP_BLOCK(0),
    LOAD_CONST(None),
    LOAD_CONST(None),
    LOAD_CONST(None),
    CALL(2),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    # Block 5 send
    SEND(Block(7)),
    SETUP_FINALLY(Block(6)),
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(5)),
    # Block 7 exit
    END_SEND(0),
    POP_TOP(0),
    JUMP_FORWARD(Block(16)),
    # Block 16 with_exit
    RETURN_CONST(None),
    # Block 19 
    CALL_INTRINSIC_1(3),
    RERAISE(1),

    # Block 2 fail
    CLEANUP_THROW(0),
    # Block 17 explicit_jump
    JUMP_BACKWARD(Block(3)),
    # Block 6 fail
    CLEANUP_THROW(0),
    # Block 18 explicit_jump
    JUMP_BACKWARD(Block(7)),
    # Block 8 with_finally
    SETUP_CLEANUP(Block(14)),
    PUSH_EXC_INFO(0),
    WITH_EXCEPT_START(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    # Block 9 send
    SEND(Block(11)),
    SETUP_FINALLY(Block(10)),
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(9)),
    # Block 10 fail
    CLEANUP_THROW(0),
    # Block 11 exit
    END_SEND(0),
    POP_JUMP_IF_TRUE(Block(13)),
    # Block 12 
    RERAISE(2),
    # Block 13 cleanup
    POP_TOP(0),
    POP_BLOCK(0),
    POP_EXCEPT(0),
    POP_TOP(0),
    POP_TOP(0),
    JUMP_BACKWARD(Block(16)),
    # Block 14 cleanup
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),
    ...,
]

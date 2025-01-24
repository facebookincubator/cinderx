def f():
   try:
       a
   except* (ValueError, KeyError, IndexError) as e:
       b
   finally:
       c
# EXPECTED:
[
    ...,

    # __BLOCK__('start: 1'),

    # __BLOCK__('try_finally_body: 2'),
    NOP(0),

    # __BLOCK__('try*_body: 3'),
    LOAD_GLOBAL('a'),
    POP_TOP(0),
    LOAD_GLOBAL('c'),
    POP_TOP(0),
    RETURN_CONST(None),

    # __BLOCK__('try*_except: 4'),
    PUSH_EXC_INFO(0),
    BUILD_LIST(0),
    COPY(2),
    LOAD_GLOBAL('ValueError'),
    LOAD_GLOBAL('KeyError'),
    LOAD_GLOBAL('IndexError'),
    BUILD_TUPLE(3),
    CHECK_EG_MATCH(0),
    COPY(1),
    POP_JUMP_IF_NONE(Block(9, label='no_match_0')),

    # __BLOCK__('try*_insert_block: 5'),
    STORE_FAST('e'),

    # __BLOCK__('try*_cleanup_body: 6'),
    LOAD_GLOBAL('b'),
    POP_TOP(0),
    LOAD_CONST(None),
    STORE_FAST('e'),
    DELETE_FAST('e'),
    JUMP_FORWARD(Block(8, label='next_except_0')),

    # __BLOCK__('try*_cleanup_end: 7'),
    LOAD_CONST(None),
    STORE_FAST('e'),
    DELETE_FAST('e'),
    LIST_APPEND(3),
    POP_TOP(0),
    JUMP_FORWARD(Block(10, label='except_with_error_0')),

    # __BLOCK__('next_except_0: 8'),
    JUMP_FORWARD(Block(10, label='except_with_error_0')),

    # __BLOCK__('no_match_0: 9'),
    POP_TOP(0),

    # __BLOCK__('except_with_error_0: 10'),
    LIST_APPEND(1),

    # __BLOCK__('try*_reraise_star: 11'),
    CALL_INTRINSIC_2(1),
    COPY(1),
    POP_JUMP_IF_NOT_NONE(Block(13, label='try*_reraise')),

    # __BLOCK__('try*_insert_block: 12'),
    POP_TOP(0),
    POP_EXCEPT(0),
    LOAD_GLOBAL('c'),
    POP_TOP(0),
    RETURN_CONST(None),

    # __BLOCK__('try*_reraise: 13'),
    SWAP(2),
    POP_EXCEPT(0),
    RERAISE(0),

    # __BLOCK__('try*_cleanup: 14'),
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),

    # __BLOCK__('try_finally_end: 17'),
    PUSH_EXC_INFO(0),
    LOAD_GLOBAL('c'),
    POP_TOP(0),
    RERAISE(0),

    # __BLOCK__('cleanup: 18'),
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),
]
# EXCEPTION_TABLE:
{
    "f": """
        4 to 14 -> 30 [0]
        30 to 74 -> 144 [1] lasti
        76 to 86 -> 96 [4] lasti
        88 to 120 -> 144 [1] lasti
        122 to 122 -> 150 [0]
        138 to 148 -> 150 [0]
        150 to 164 -> 166 [1] lasti
    """
}


def f():
   try:
       a
   except:
       b
# EXPECTED:
[
    ...,

    __BLOCK__('start: 1'),
    NOP(0),

    __BLOCK__('try_body: 2'),
    LOAD_GLOBAL('a'),
    POP_TOP(0),
    RETURN_CONST(None),

    __BLOCK__('try_except: 3'),
    PUSH_EXC_INFO(0),
    POP_TOP(0),

    __BLOCK__('try_cleanup_body_0: 4'),
    LOAD_GLOBAL('b'),
    POP_TOP(0),
    POP_EXCEPT(0),
    RETURN_CONST(None),

    __BLOCK__('try_cleanup: 6'),
    COPY(3),
    POP_EXCEPT(0),
    RERAISE(1),
]
# EXCEPTION_TABLE:
{
    "f": """
      4 to 14 -> 18 [0]
      18 to 32 -> 38 [1] lasti
    """
}

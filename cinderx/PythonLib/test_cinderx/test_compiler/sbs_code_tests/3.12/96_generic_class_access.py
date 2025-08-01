# pyre-ignore-all-errors
class ClassA[A, B](dict[A, B]):
    pass
# EXPECTED:
[
    ...,
    PUSH_NULL(0),
    LOAD_CONST(Code((2,13))),
    MAKE_FUNCTION(0),
    CALL(0),
    STORE_NAME('ClassA'),
    RETURN_CONST(None),
    
    CODE_START('<generic parameters of ClassA>'),
    ...,
    LOAD_GLOBAL('dict'),
    LOAD_FAST('A'),
    LOAD_FAST('B'),
    BUILD_TUPLE(2),
    BINARY_SUBSCR(0),
    LOAD_FAST('.generic_base'),
    CALL(4),
    RETURN_VALUE(0),
    
    CODE_START('ClassA'),
    ...,
]

# pyre-ignore-all-errors
class C[T]:
    global X
    type X = T
# EXPECTED:
[
    ...,
    PUSH_NULL(0),
    LOAD_CONST(Code((2,8))),
    MAKE_FUNCTION(0),
    CALL(0),
    STORE_NAME('C'),
    RETURN_CONST(None),
    
    CODE_START('<generic parameters of C>'),
    ...,
    LOAD_CONST('T'),
    CALL_INTRINSIC_1(7),
    COPY(1),
    STORE_DEREF('T'),
    BUILD_TUPLE(1),
    STORE_DEREF('.type_params'),
    PUSH_NULL(0),
    LOAD_BUILD_CLASS(0),
    LOAD_CLOSURE('.type_params'),
    LOAD_CLOSURE('T'),
    BUILD_TUPLE(2),
    LOAD_CONST(Code((2,0))),
    MAKE_FUNCTION(8),
    LOAD_CONST('C'),
    LOAD_DEREF('.type_params'),
    CALL_INTRINSIC_1(10),
    STORE_FAST('.generic_base'),
    LOAD_FAST('.generic_base'),
    CALL(3),
    RETURN_VALUE(0),
    
    CODE_START('C'),
    ...,
    LOAD_NAME('__name__'),
    STORE_NAME('__module__'),
    LOAD_CONST('C'),
    STORE_NAME('__qualname__'),
    LOAD_LOCALS(0),
    LOAD_FROM_DICT_OR_DEREF('.type_params'),
    STORE_NAME('__type_params__'),
    LOAD_LOCALS(0),
    STORE_DEREF('__classdict__'),
    LOAD_CONST('X'),
    LOAD_CONST(None),
    LOAD_CLOSURE('T'),
    LOAD_CLOSURE('__classdict__'),
    BUILD_TUPLE(2),
    LOAD_CONST(Code('X')),
    MAKE_FUNCTION(8),
    BUILD_TUPLE(3),
    CALL_INTRINSIC_1(11),
    STORE_GLOBAL('X'),
    LOAD_CLOSURE('__classdict__'),
    STORE_NAME('__classdictcell__'),
    RETURN_CONST(None),
    
    CODE_START('X'),
    ...,
    LOAD_DEREF('__classdict__'),
    LOAD_FROM_DICT_OR_DEREF('T'),
    RETURN_VALUE(0),
]

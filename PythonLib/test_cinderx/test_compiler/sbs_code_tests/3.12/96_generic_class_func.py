# pyre-ignore-all-errors
class C[T]:
    def f(self, x: T): pass

# EXPECTED:
[
    ...,
    LOAD_CONST(Code(('<generic parameters of C>'))),
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
    LOAD_CONST(Code("C")),
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
    LOAD_CONST('x'),
    LOAD_LOCALS(0),
    LOAD_FROM_DICT_OR_DEREF('T'),
    BUILD_TUPLE(2),
    LOAD_CONST(Code("f")),
    MAKE_FUNCTION(4),
    STORE_NAME('f'),
    RETURN_CONST(None),

    CODE_START('f'),
    ...,
    RETURN_CONST(None),
]

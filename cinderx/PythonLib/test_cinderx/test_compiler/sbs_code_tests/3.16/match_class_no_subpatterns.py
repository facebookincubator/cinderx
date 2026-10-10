match x:
    case str():
        pass
    case int(y):
        pass
# EXPECTED:
[
    ...,
    LOAD_NAME("str"),
    CALL_INTRINSIC_2(7),
    POP_JUMP_IF_FALSE(Any),
    ...,
    LOAD_NAME("int"),
    LOAD_COMMON_CONSTANT(13),
    MATCH_CLASS(1),
    ...,
]

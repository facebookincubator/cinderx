{*()}
{*[], *{}}
[*()]
[*(), *[]]
[1, *()]
tuple_value = (*(), 1)
{1, 2, 3, *()}
[1, 2, 3, *()]
# EXPECTED:
[
    ...,
    BUILD_SET(0),
    POP_TOP(0),
    BUILD_SET(0),
    POP_TOP(0),
    BUILD_LIST(0),
    POP_TOP(0),
    BUILD_LIST(0),
    POP_TOP(0),
    LOAD_SMALL_INT(1),
    BUILD_LIST(1),
    POP_TOP(0),
    LOAD_CONST((1,)),
    STORE_NAME("tuple_value"),
    BUILD_SET(0),
    LOAD_CONST(5),
    SET_UPDATE(1),
    POP_TOP(0),
    BUILD_LIST(0),
    LOAD_CONST(6),
    LIST_EXTEND(1),
    POP_TOP(0),
    ...,
]

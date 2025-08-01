# pyre-ignore-all-errors
fun(a, kw=1, *c, **d)
# EXPECTED:
[
    LOAD_NAME("fun"),
    LOAD_NAME("a"),
    BUILD_LIST(1),
    LOAD_NAME("c"),
    LIST_EXTEND(1),
    LIST_TO_TUPLE(0),
    LOAD_CONST("kw"),
    LOAD_CONST(1),
    BUILD_MAP(1),
    LOAD_NAME("d"),
    DICT_MERGE(1),
    CALL_FUNCTION_EX(1),
    ...,
]

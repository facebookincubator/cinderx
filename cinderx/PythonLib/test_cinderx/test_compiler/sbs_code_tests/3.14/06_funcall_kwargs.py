# pyre-ignore-all-errors
c = {a: 1, b: 2}
fun(a, b, **c)
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    LOAD_NAME('fun'),
    ...,
    BUILD_TUPLE(2),
    ...,
    DICT_MERGE(1),
    CALL_FUNCTION_EX(0),
    ...,
]

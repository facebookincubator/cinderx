# pyre-ignore-all-errors
{1, *[1, 2, 3], *[4, 5, 6]}
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    LOAD_SMALL_INT(1),
    BUILD_SET(1),
    BUILD_LIST(0),
    LOAD_CONST(7),
    LIST_EXTEND(1),
    SET_UPDATE(1),
    BUILD_LIST(0),
    LOAD_CONST(8),
    LIST_EXTEND(1),
    SET_UPDATE(1),
    ...,
]

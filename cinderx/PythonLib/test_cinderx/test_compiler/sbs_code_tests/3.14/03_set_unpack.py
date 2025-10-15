# pyre-ignore-all-errors
{*[1, 2, 3]}
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    BUILD_SET(0),
    BUILD_LIST(0),
    LOAD_CONST(4),
    LIST_EXTEND(1),
    SET_UPDATE(1),
    ...,
]

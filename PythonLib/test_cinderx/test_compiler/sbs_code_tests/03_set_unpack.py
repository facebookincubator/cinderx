# pyre-ignore-all-errors
{*[1, 2, 3]}
# EXPECTED:
[..., BUILD_SET(0), BUILD_LIST(0), LOAD_CONST((1, 2, 3)), LIST_EXTEND(1), SET_UPDATE(1), ...]

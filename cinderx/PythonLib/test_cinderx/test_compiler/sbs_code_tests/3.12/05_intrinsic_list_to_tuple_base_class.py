# pyre-ignore-all-errors
class A(0, 1, 2, **d): pass
# EXPECTED:
[
    ...,
    CALL_INTRINSIC_1(6),
    ...,
]

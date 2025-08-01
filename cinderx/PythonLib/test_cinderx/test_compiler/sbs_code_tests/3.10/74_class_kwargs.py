# pyre-ignore-all-errors
class Foo(x=42):
    pass
# EXPECTED:
[
    ...,
    LOAD_CONST(Code(('Foo'))),
    ...,
    LOAD_CONST(42),
    LOAD_CONST(('x',)),
    CALL_FUNCTION_KW(3),
    ...
]

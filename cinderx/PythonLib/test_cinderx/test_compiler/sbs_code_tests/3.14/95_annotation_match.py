# pyre-ignore-all-errors
match f:
    case _:
        x: int = 3
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    MAKE_CELL(0),
    RESUME(0),
    LOAD_CONST(Code('__annotate__')),
    MAKE_FUNCTION(0),
    STORE_NAME('__annotate__'),    
    ...,
]

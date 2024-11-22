# pyre-ignore-all-errors
def f(a):
    a0 = a1 = a2 = a3 = a4 = a5 = a6 = a7 = a8 = a9 = 0
    b0 = b1 = b2 = b3 = b4 = b5 = b6 = b7 = b8 = b9 = 0
    c0 = c1 = c2 = c3 = c4 = c5 = c6 = c7 = c8 = c9 = 0
    d0 = d1 = d2 = d3 = d4 = d5 = d6 = d7 = d8 = d9 = 0
    e0 = e1 = e2 = e3 = e4 = e5 = e6 = e7 = e8 = e9 = 0
    f0 = f1 = f2 = f3 = f4 = f5 = f6 = f7 = f8 = f9 = 0
    g0 = g1 = g2 = g3 = g4 = g5 = g6 = g7 = g8 = g9 = 0
    if g0:
        z
    z = 42
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('f'),

    # __BLOCK__('entry: 0'),
    ...,

    # __BLOCK__('start: 1'),
    ...,
    POP_JUMP_IF_FALSE(Block(3, label='if_end')),

    # __BLOCK__(': 2'),
    LOAD_FAST_CHECK('z'),
    
    ...,
    RETURN_CONST(None),
]

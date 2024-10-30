# pyre-ignore-all-errors
try:
    a
finally:
    b
# EXPECTED:
[
  RESUME(0),
  SETUP_FINALLY(Block(2)),
  LOAD_NAME('a'),

  POP_TOP(0),
  POP_BLOCK(0),
  LOAD_NAME('b'),
  POP_TOP(0),
  RETURN_CONST(None),

  SETUP_CLEANUP(Block(3)),
  PUSH_EXC_INFO(0),

  LOAD_NAME('b'),
  POP_TOP(0),
  RERAISE(0),

  COPY(3),
  POP_EXCEPT(0),
  RERAISE(1),
]

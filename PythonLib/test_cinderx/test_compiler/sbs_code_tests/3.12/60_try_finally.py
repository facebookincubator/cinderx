# pyre-ignore-all-errors
try:
    a
finally:
    b
# EXPECTED:
[
  RESUME(0),
  NOP(0),
  LOAD_NAME('a'),

  POP_TOP(0),
  LOAD_NAME('b'),
  POP_TOP(0),
  RETURN_CONST(None),

  PUSH_EXC_INFO(0),

  LOAD_NAME('b'),
  POP_TOP(0),
  RERAISE(0),

  COPY(3),
  POP_EXCEPT(0),
  RERAISE(1),
]
# EXCEPTION_TABLE:
"""
  4 to 14 -> 30 [0]
  30 to 44 -> 46 [1] lasti
"""

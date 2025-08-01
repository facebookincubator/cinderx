# pyre-ignore-all-errors
class Foo:
    def __init__(self):
        a = super().__init__


# EXPECTED:
[
    ...,
    CODE_START("__init__"),
    ...,
    LOAD_GLOBAL("super"),
    LOAD_DEREF("__class__"),
    LOAD_FAST("self"),
    LOAD_SUPER_ATTR(("LOAD_ZERO_SUPER_ATTR", "__init__", True)),
    ...,
]

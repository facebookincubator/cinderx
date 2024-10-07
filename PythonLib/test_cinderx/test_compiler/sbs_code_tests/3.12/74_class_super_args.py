# pyre-ignore-all-errors
class Foo:
    def __init__(self):
        a = super(Foo, self).__init__


# EXPECTED:
[
    ...,
    CODE_START("__init__"),
    ...,
    LOAD_GLOBAL("super"),
    LOAD_GLOBAL("Foo"),
    LOAD_FAST("self"),
    LOAD_SUPER_ATTR(("LOAD_SUPER_ATTR", "__init__", False)),
    ...,
]

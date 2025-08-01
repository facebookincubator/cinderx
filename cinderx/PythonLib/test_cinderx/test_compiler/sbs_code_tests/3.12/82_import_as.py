# pyre-ignore-all-errors
import foo.bar.baz as quox
# EXPECTED:
[
    ...,
    IMPORT_FROM('bar'),
    SWAP(2),
    ...,
    STORE_NAME('quox'),
    ...,
]

from types import CodeType

from test_cinderx.test_compiler.test_static.pyrefly_binder import PyreBinderTests


def verify(test: PyreBinderTests, code: CodeType) -> None:
    """Verify that the compiled code includes FAST_LEN."""

    f = test.find_code(code, "f")
    test.assertInBytecode(f, "FAST_LEN")

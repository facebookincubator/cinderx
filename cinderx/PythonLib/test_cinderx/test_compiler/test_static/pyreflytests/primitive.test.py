from types import CodeType

from test_cinderx.test_compiler.test_static.pyrefly_binder import PyreBinderTests


def verify(test: PyreBinderTests, code: CodeType) -> None:
    """Verify that the compiled code includes PRIMITIVE_BINARY_OP."""

    g = test.find_code(code, "f")
    test.assertInBytecode(g, "PRIMITIVE_BINARY_OP")

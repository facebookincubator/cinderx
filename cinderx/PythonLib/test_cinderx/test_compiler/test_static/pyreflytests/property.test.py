from types import CodeType

from test_cinderx.test_compiler.test_static.pyrefly_binder import PyreBinderTests


def verify(test: PyreBinderTests, code: CodeType) -> None:
    """Verify that the compiled code includes INVOKE_METHOD."""

    g = test.find_code(code, "g")
    test.assertInBytecode(g, "INVOKE_METHOD")

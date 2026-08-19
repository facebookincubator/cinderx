# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import cinderx.jit
from cinderx.test_support import skip_if_prefork, skip_unless_jit

from .common import StaticTestBase


class ReturnCastInsertionTests(StaticTestBase):
    def test_no_cast_to_object(self) -> None:
        """We never cast to object, for object or dynamic or no annotation."""
        for is_async in [True, False]:
            for ann in ["object", "open", None]:
                with self.subTest(ann=ann, is_async=is_async):
                    prefix = "async " if is_async else ""
                    full_ann = f" -> {ann}" if ann else ""
                    codestr = f"""
                        {prefix}def f(x){full_ann}:
                            return x
                    """
                    f_code = self.find_code(self.compile(codestr), "f")
                    self.assertNotInBytecode(f_code, "CAST")

    def test_annotated_method_does_not_cast_lower(self) -> None:
        codestr = """
            def f() -> str:
                return 'abc'.lower()
        """
        f_code = self.find_code(self.compile(codestr), "f")
        self.assertNotInBytecode(f_code, "CAST")
        self.assertInBytecode(f_code, "REFINE_TYPE")

    def test_annotated_method_does_not_cast_upper(self) -> None:
        codestr = """
            def f() -> str:
                return 'abc'.upper()
        """
        f_code = self.find_code(self.compile(codestr), "f")
        self.assertNotInBytecode(f_code, "CAST")
        self.assertInBytecode(f_code, "REFINE_TYPE")

    def test_annotated_method_does_not_cast_isdigit(self) -> None:
        codestr = """
            def f() -> bool:
                return 'abc'.isdigit()
        """
        f_code = self.find_code(self.compile(codestr), "f")
        self.assertNotInBytecode(f_code, "CAST")
        self.assertInBytecode(f_code, "REFINE_TYPE")

    def test_annotated_method_does_not_cast_known_subclass(self) -> None:
        codestr = """
            class C(str):
                pass

            def f() -> bool:
                return C('abc').isdigit()
        """
        f_code = self.find_code(self.compile(codestr), "f")
        self.assertNotInBytecode(f_code, "CAST")
        self.assertInBytecode(f_code, "REFINE_TYPE")

    def test_annotated_method_casts_arbitrary_subclass(self) -> None:
        codestr = """
            def f(x: str) -> bool:
                return x.isdigit()
        """
        f_code = self.find_code(self.compile(codestr), "f")
        self.assertInBytecode(f_code, "CAST")
        self.assertNotInBytecode(f_code, "REFINE_TYPE")

    def test_annotated_method_does_not_cast_if_valid_on_subclasses(self) -> None:
        codestr = """
            from __static__ import ContextDecorator
            class C(ContextDecorator):
                pass

            def f() -> ContextDecorator:
                return C()._recreate_cm()
        """
        f_code = self.find_code(self.compile(codestr), "f")
        self.assertNotInBytecode(f_code, "CAST")

    @skip_unless_jit("Testing Static Python + JIT behavior")
    @skip_if_prefork("exec + compile leaks memory in prefork")
    def test_failed_return_cast_raises_in_jit(self) -> None:
        """A failing return cast must raise rather than crash under the JIT.

        rt::cast is inlined from hand-written LIR whose error path calls
        PyErr_Format, which is variadic.  That LIR passes the two tp_name
        arguments in registers, which is wrong on platforms that pass variadic
        arguments on the stack, so PyErr_Format read NULL for '%s' and the
        raise became a segfault.

        This checks TypeError rather than StaticTypeError because the two
        paths disagree on the exception type: the inlined LIR raises
        PyExc_TypeError while the out-of-line rt::cast raises
        CiExc_StaticTypeError, which is a TypeError subclass.
        """
        for ann, good, bad, message in [
            ("int", 42, None, "expected 'int', got 'NoneType'"),
            ("str", "abc", 42, "expected 'str', got 'int'"),
        ]:
            with self.subTest(ann=ann):
                codestr = f"""
                    def f(x) -> {ann}:
                        return x
                """
                with self.in_module(codestr) as mod:
                    self.assertInBytecode(mod.f, "CAST")

                    cinderx.jit.force_compile(mod.f)
                    self.assertTrue(cinderx.jit.is_jit_compiled(mod.f))

                    self.assertEqual(mod.f(good), good)
                    with self.assertRaisesRegex(TypeError, message):
                        mod.f(bad)

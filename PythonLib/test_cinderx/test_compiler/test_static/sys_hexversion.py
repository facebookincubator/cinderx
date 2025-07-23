# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import sys
from itertools import product

from .common import StaticTestBase


class SysHexVersionTests(StaticTestBase):
    def test_sys_hexversion(self):
        current_version = sys.hexversion

        for version_check_should_pass, is_on_left in product(
            (False, True), (False, True)
        ):
            with self.subTest(msg=f"{version_check_should_pass=}, {is_on_left=}"):
                if version_check_should_pass:
                    checked_version = current_version - 1
                    expected_attribute = "a"
                else:
                    checked_version = current_version + 1
                    expected_attribute = "b"

                if is_on_left:
                    condition_str = f"sys.hexversion >= {hex(checked_version)}"
                else:
                    condition_str = f"{hex(checked_version)} <= sys.hexversion"

                codestr = f"""
                import sys

                if {condition_str}:
                    class A:
                        def a(self):
                            pass
                else:
                    class A:
                        def b(self):
                            pass
                """
                with self.in_strict_module(codestr) as mod:
                    self.assertTrue(hasattr(mod.A, expected_attribute))

    def test_sys_hexversion_unsupported_operator(self):
        op_to_err = {
            "in": "in",
            "is": "is",
            "is not": "is",
        }
        for op, is_on_left in product(op_to_err.keys(), (True, False)):
            if is_on_left:
                condition_str = f"sys.hexversion {op} 50988528"
            else:
                condition_str = f"50988528 {op} sys.hexversion"

            with self.subTest(msg=f"{op=}, {is_on_left=}"):
                codestr = f"""
                import sys

                if {condition_str}:
                    class A:
                        def a(self):
                            pass
                else:
                    class A:
                        def b(self):
                            pass
                """
                if op == "in":
                    self.assertRaisesRegex(
                        TypeError,
                        "argument of type 'int' is not iterable",
                    )
                else:
                    with self.in_module(codestr) as mod:
                        self.assertIn("A", mod.__dict__)
                        self.assertTrue(
                            hasattr(mod.A, "a") or hasattr(mod.A, "b"),
                            f"neither A.a nor A.b is defined in {mod.__dict__}",
                        )
                        self.assertTrue(
                            not hasattr(mod.A, "a") or not hasattr(mod.A, "b"),
                            f"both A.a and A.b are defined in {mod.__dict__}",
                        )

    def test_sys_hexversion_dynamic_compare(self):
        somethingcodestr = """
        X: int = 1
        """

        codestr = """
        import sys
        from something import X

        if sys.hexversion >= X:
            class A:
                def a(self):
                    pass
        else:
            class A:
                def b(self):
                    pass
        """

        with self.in_module(somethingcodestr, name="something"):
            with self.in_module(codestr) as mod:
                self.assertIn("A", mod.__dict__)
                self.assertTrue(
                    hasattr(mod.A, "a") or hasattr(mod.A, "b"),
                    f"neither A.a nor A.b is defined in {mod.__dict__}",
                )
                self.assertTrue(
                    not hasattr(mod.A, "a") or not hasattr(mod.A, "b"),
                    f"both A.a and A.b are defined in {mod.__dict__}",
                )

                a = mod.A()
                self.assertTrue(hasattr(a, "a"))

    # comparing with double is kinda meaningless
    def test_sys_hexversion_compare_double(self):
        codestr = """
        import sys

        if sys.hexversion >= 3.12:
            class A:
                def a(self):
                    pass
        else:
            class A:
                def b(self):
                    pass
        """
        with self.in_module(codestr) as mod:
            self.assertIn("A", mod.__dict__)
            self.assertTrue(
                hasattr(mod.A, "a") or hasattr(mod.A, "b"),
                f"neither A.a nor A.b is defined in {mod.__dict__}",
            )
            self.assertTrue(
                not hasattr(mod.A, "a") or not hasattr(mod.A, "b"),
                f"both A.a and A.b are defined in {mod.__dict__}",
            )

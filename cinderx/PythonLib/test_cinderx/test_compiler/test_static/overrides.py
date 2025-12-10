# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import sys

from cinderx.test_support import passIf

from .common import StaticTestBase


MISSING_TYPING_OVERRIDE: bool = sys.version_info < (3, 12)


class OverridesTests(StaticTestBase):
    def test_literal_false_return_override(self) -> None:
        codestr = """
            from typing import Literal

            class A:
                def f(self) -> Literal[False]:
                    return False

            class B(A):
                def f(self) -> bool:
                    return False
        """
        self.type_error(
            codestr,
            (
                r"<module>.B.f overrides <module>.A.f inconsistently. "
                r"Returned type `bool` is not a subtype of the overridden return `Literal\[False\]`"
            ),
            at="def f(self) -> bool",
        )

    def test_name_irrelevant_for_posonlyarg(self) -> None:
        codestr = """
            class A:
                def f(self, x: int, /) -> int:
                    return x + 1

            class B(A):
                def f(self, y: int, /) -> int:
                    return y + 2

            def f(a: A) -> int:
                return a.f(1)
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.f(mod.A()), 2)
            self.assertEqual(mod.f(mod.B()), 3)

    def test_cannot_override_normal_arg_with_posonly(self) -> None:
        codestr = """
            class A:
                def f(self, x: int) -> None:
                    pass

            class B(A):
                def f(self, x: int, /) -> None:
                    pass
        """
        self.type_error(
            codestr,
            r"<module>.B.f overrides <module>.A.f inconsistently. `x` is positional-only in override, not in base",
            at="def f(self, x: int, /)",
        )

    def test_can_override_posonly_arg_with_normal(self) -> None:
        codestr = """
            class A:
                def f(self, x: int, /) -> int:
                    return x + 1

            class B(A):
                def f(self, y: int) -> int:
                    return y + 2

            def f(a: A) -> int:
                return a.f(1)
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.f(mod.A()), 2)
            self.assertEqual(mod.f(mod.B()), 3)

    @passIf(MISSING_TYPING_OVERRIDE, "typing.override is new in 3.12")
    def test_typing_override(self) -> None:
        codestr = """
            from typing import override
            class Base:
                def run(self) -> str:
                    return "base"


            class Child(Base):
                @override
                def run(self) -> str:
                    return "child"

            def run():
                return Child().run()
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.run(), "child")

    @passIf(MISSING_TYPING_OVERRIDE, "typing.override is new in 3.12")
    def test_typing_override2(self) -> None:
        codestr = """
            from typing import override
            class Base:
                def run(self) -> str:
                    return "base"

            class Child(Base):
                @override
                def run(self) -> str:
                    return "child"

            class Grandchild(Child):
                def run(self) -> str:
                    return "grandchild"
                
            def run():
                return Grandchild().run()
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.run(), "grandchild")

    @passIf(MISSING_TYPING_OVERRIDE, "typing.override is new in 3.12")
    def test_abc_override_init(self) -> None:
        codestr = """
            from typing import override
            
            class Base:
                @override
                def __init__(self):
                    pass
        """
        with self.in_module(codestr) as mod:
            pass

    @passIf(MISSING_TYPING_OVERRIDE, "typing.override is new in 3.12")
    def test_typing_override_property(self) -> None:
        codestr = """
            from typing import override
            class Base:
                @property
                def run(self) -> str:
                    return "base"


            class Child(Base):
                @property
                @override
                def run(self) -> str:
                    return "child"

            def run():
                return Child().run
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.run(), "child")

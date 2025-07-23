# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe
from .common import StaticTestBase


class MixinTests(StaticTestBase):
    def test_inheritance(self) -> None:
        codestr = """
        class C:
            x: int = 42

        class D(C):
            pass

        def f():
            d = D()
            return (D.x, d.x)
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.f(), (42, 42))

    # TODO: This will be fixed later.
    def test_multiple_inheritance(self) -> None:
        codestr = """
        class C:
            x: int = 42

        class D:
            y: int = 3

        class E(C, D):
            pass

        def f():
            e = E()
            return (E.x, e.x, E.y, e.y)
        """
        self.assertRaisesRegex(
            TypeError,
            "multiple bases have instance lay-out conflict",
            self.run_code,
            codestr,
        )

    def test_multiple_inheritance_with_mixin(self) -> None:
        codestr = """
        from __static__ import mixin

        class C:
            x: int = 42

        @mixin
        class D:
            y: int = 3

        class E(C, D):
            pass

        def f():
            e = E()
            return (E.x, e.x, E.y, e.y)
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.f(), (42, 42, 3, 3))

    def test_multiple_inheritance_with_mixin_mro(self) -> None:
        codestr = """
        from __static__ import mixin

        class C:
            x: int = 42

        @mixin
        class D:
            x: int = 3

        class E(C, D):
            pass

        def f():
            e = E()
            return (E.x, e.x)
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.f(), (42, 42))

    def test_multiple_inheritance_with_mixin_incorrect_order(self) -> None:
        codestr = """
        from __static__ import mixin

        class C:
            x: int = 42

        @mixin
        class D:
            x: int = 3

        class E(D, C):
            pass

        def f():
            e = E()
            return (E.x, e.x)
        """
        self.assertRaisesRegex(
            TypeError,
            "Static compiler cannot verify that static type 'E' is a valid override of static base 'C' because intervening base 'D' is non-static.",
            self.run_code,
            codestr,
        )

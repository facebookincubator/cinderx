# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from .common import StaticTestBase


class AugAssignTests(StaticTestBase):
    def test_aug_assign(self) -> None:
        codestr = """
        def f(l):
            l[0] += 1
        """
        with self.in_module(codestr) as mod:
            f = mod.f
            li = [1]
            f(li)
            self.assertEqual(li[0], 2)

    def test_field(self) -> None:
        codestr = """
        class C:
            def __init__(self):
                self.x = 1

        def f(a: C):
            a.x += 1
        """
        code = self.compile(codestr, modname="foo")
        code = self.find_code(code, name="f")
        self.assertInBytecode(code, "LOAD_FIELD", (("foo", "C"), "x"))
        self.assertInBytecode(code, "STORE_FIELD", (("foo", "C"), "x"))

    def test_primitive_int(self) -> None:
        codestr = """
        from __static__ import int8, box, unbox

        def a(i: int) -> int:
            j: int8 = unbox(i)
            j += 2
            return box(j)
        """
        with self.in_module(codestr) as mod:
            a = mod.a
            self.assertInBytecode(a, "PRIMITIVE_BINARY_OP", 0)
            self.assertEqual(a(3), 5)

    def test_primitive_int_dynamic_rhs(self) -> None:
        codestr = """
        from __static__ import int64

        class C:
            def __init__(self) -> None:
                self.total: int64 = 0

            def add_values(self, values: dict[str, int]) -> None:
                for _, value in values.items():
                    self.total += value
        """
        self.type_error(codestr, r"cannot add int64 and dynamic", at="value")

    def test_primitive_int_dynamic_rhs_explicit_cast(self) -> None:
        codestr = """
        from __static__ import box, int64

        class C:
            def __init__(self) -> None:
                self.total: int64 = 0

            def add_values(self, values: dict[str, int]) -> int:
                for _, value in values.items():
                    self.total += int64(value)
                return box(self.total)
        """
        with self.in_module(codestr) as mod:
            instance = mod.C()
            self.assertEqual(instance.add_values({"a": 2, "b": 3}), 5)

    def test_inexact(self) -> None:
        codestr = """
        def something():
            return 3

        def t():
            a: int = something()

            b = 0
            b += a
            return b
        """
        with self.in_module(codestr) as mod:
            t = mod.t
            self.assertBinOpInBytecode(t, "INPLACE_ADD")
            self.assertEqual(t(), 3)

    def test_list(self) -> None:
        for prim_idx in [True, False]:
            with self.subTest(prim_idx=prim_idx):
                codestr = f"""
                    from __static__ import int32

                    def f(x: int):
                        l = [x]
                        i: {"int32" if prim_idx else "int"} = 0
                        l[i] += 1
                        return l[i]
                """
                with self.in_module(codestr) as mod:
                    self.assertEqual(mod.f(3), 4)

    def test_checked_list(self) -> None:
        for prim_idx in [True, False]:
            with self.subTest(prim_idx=prim_idx):
                codestr = f"""
                    from __static__ import CheckedList, int32

                    def f(x: int):
                        l: CheckedList[int] = [x]
                        i: {"int32" if prim_idx else "int"} = 0
                        l[i] += 1
                        return l[i]
                """
                with self.in_module(codestr) as mod:
                    self.assertEqual(mod.f(3), 4)

    def test_checked_dict(self) -> None:
        codestr = """
            from __static__ import CheckedDict

            def f(x: int):
                d: CheckedDict[int, int] = {0: x}
                d[0] += 1
                return d[0]
        """
        with self.in_module(codestr) as mod:
            self.assertEqual(mod.f(3), 4)

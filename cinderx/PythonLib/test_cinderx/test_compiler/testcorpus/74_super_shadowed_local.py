def test_shadowed_local(self) -> None:
    class super:
        msg = "quite super"

    class C:
        def method(self):
            return super().msg

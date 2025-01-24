class super:
    msg = "quite super"

class C:
    def method(self):
        return super().msg

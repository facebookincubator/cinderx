# pyre-ignore-all-errors
def test_assignment_idiom_in_comprehensions():
    def listcomp():
        return [y for x in a for y in [f(x)]]

# Static Python (Programming Patterns)

By necessity, Static Python discourages or disallows certain patterns
that are common in Python. These patterns are highly dynamic (i.e,
cannot be understood or optimized by statically analyzing the code).

This document discusses such patterns, and provides reasonable
alternatives to them.

## 1. Using a class-var to override instance-var

Example:

```python
class Parent:
    def __init__(self) -> None:
        self.is_serializable: bool = True


class Child(Parent):
    is_serializable = False
```

### Why is it used?

To avoid having to declare an [\_\_init\_\_]{.title-ref} method on the
subclass, with potentially more boilerplate (passing arguments, etc).

### Why is it bad?

This pattern is confusing. Can you tell (without running the code), what
would be printed by this snippet?

``` python
print(Child().is_serializable)
print(Child.is_serializable)
```

Is that even the intended result? :)

### Solutions

1.  Explicitly define an `__init__()` method in the
    `Child` class, that sets the instance attribute to the
    desired value. Example:

    > ```python
    > class Parent:
    >     def __init__(self) -> None:
    >         self.is_serializable: bool = True
    >
    >
    > class Child(Parent):
    >     def __init__(self) -> None:
    >         self.is_serializable: bool = False
    > ```



## 2. Overriding an attribute with a method

Example:

```python
class someClass:
    attr: str

    def __init__(self, attr):
        self.attr = attr

    def attr(self):
        return self.attr
```

### Why is it bad?

Static Python will give the error `"TypedSyntaxError: function conflicts with other member attr in Type[main.someClass]"` because it tries to create slots for both the attribute and the method, which have the same name.

### Solutions

1.  Rename the attribute to a name different than that of the method. Example:

    > ```python
    > class someClass:
    >     attribute: str
    >
    >     def __init__(self, attribute):
    >         self.attribute = attribute
    >
    >     def attr(self):
    >         return self.attribute
    >
    >     @attr.setter
    >     def set_attr(self, new_attr):
    >         self.attribute = new_attr
    >
    > ```

### Note
A method can be overridden with another method, however.

## 3. Inheriting from multiple classes

Example:

```python
class A:
    a_attr
class B:
    b_attr
class C(A, B):
    pass
```

### Why is it used?

To combine functionalities from more than one class.
### Why is it bad?

You'll get a runtime exception: `TypeError: multiple bases have instance lay-out conflict`

### Solutions

1. Make a single class that has all the functionality of the parent classes and inherit from this class
    > ```python
    > class A:
    >     a_attr
    > class B:
    >     b_attr
    > class newParent(A):
    >     b_attr
    > class C(newParent):
    >     pass
    >
    > ```
2. Mark one of the classes with `@__static__.mixin` (making it dynamic instead of static). Example:
   > ```python
    > class A:
    >     a_attr
    > @__static__.mixin
    > class B:
    >     b_attr
    > class C(A,B):
    >     pass
    > ```

## 4. Expecting keyword arguments in tests that mock functions

Example:

```python
from unittest import TestCase
from unittest.mock import patch


class testClass(TestCase):

    def someFunc(self, some_kwarg):
        pass
    def test_something(self)
        with patch(f'{__name__}.testClass.someFunc') as mock_some_func:
            self.someFunc(some_kwarg='some_kwarg_value')
            for args, kwargs in mock_some_func.call_args_list:
                self.assertEqual('some_kwarg_value', kwargs.get('some_kwarg'))

```

### Why is it used?

In testing to check a keyword argument was correctly passed into a function.
### Why is it bad?

Static Python turns arguments passed as keywords into positional arguments.

### Solutions

1.  Get the desired keyword argument from positional args. Example:

    > ```python
    > from unittest import TestCase
    > from unittest.mock import patch
    >
    > class testClass(TestCase):
    >     def someFunc(self, some_kwarg):
    >         pass
    >     def test_something(self):
    >         with patch(f'{__name__}.testClass.someFunc') as mock_some_func:
    >             someFunc(some_kwarg='some_kwarg_value')
    >             for args, kwargs in mock_some_func.call_args_list:
    >                 self.assertTrue('some_kwarg_value' in args[0])
    > ```

### Note
If you want to keep your tests compatibile with and without Static Python, you can use a check seeing if kwargs exists to know whether Static Python is on. Example:

 > ```python
 > from unittest import TestCase
 > from unittest.mock import patch
 >
 > class testClass(UnitTest):
 >     def someFunc(self, some_kwarg):
 >         pass
 >     def test_something(self):
 >         with patch('someFunc') as mock_some_func:
 >             someFunc(some_kwarg='some_kwarg_value')
 >             for args, kwargs in mock_some_func.call_args_list:
 >                 if kwargs:
 >                       self.assertEqual('some_kwarg_value', kwargs.get('some_kwarg'))
 >                 else:
 >                       self.assertEqual('some_kwarg_value',args[0])
 > ```

 ## 4. Redefinition (even in an if-else block)

Example:

```python
    if True:
        x: bool = True
    else:
        x: bool = False
```

### Why is it bad?

Static Python does not support redefinition. If you define a variable in the if and else blocks of an if-else block, you will get a Static Python error because the Static Python compiler believes you have redefined something.
### Solutions

1. To use if-else statements with redefinition you can use a ternary operator. Example:
    > ```python
    > x = True if True else False
    > ```
2. Remove a type annotation from one of the blocks if they are the same annotation

 ## 5. Using weakref without having manually declaring a lot for it

Example:

```python
    import weakref

    class someClass():
        pass

    a = someClass()
    b = weakref.ref(a)
```

### Why is it bad?

Using weakrefs on a class will lead to a runtime exception: `cannot create weak reference to 'someClass' object`. This is because weakrefs requires that there’s a `__weakrefs__` slots available, but Static Python autoslotifies things.

### Solutions

1. Make sure to add `__weakref__` to the slots for that class. Example:
    > ```python
    > import weakref
    > class someClass():
    >    __weakref__: Any
    > ```

 ## 6. Expecting Static Python to understand the return type of a `with` statement

Example:

```python
class someClass():
    def test_with(self) -> int:
        with open("womp") as f:
            return 5
```

### Why is it bad?

In Python `with` statements can suppress the exception from being thrown, and therefore the `return` is not guaranteed, depending on the return type of `open`. Currently static Python does not understand that opening a file will never suppress the exception (this is a bug in static Python).

### Solutions

1. Have an unreachable runtime error after the `with` statement. Example:
    > ```python
    > class someClass():
    >   def test_with(self) -> int:
    >     with open("womp") as f:
    >         return 5
    >     raise RuntimeError("womp")
    > ```

 ## 7. Having a static class inherit from a non-static class that inherits from a static class

A static class is a class defined in a file that is compiled with Static Python (there is a `__static__` at the top of the file)
Example:

```python
File A:
    import __static__
    def GrandParent():
        pass
File B:
    from A import Grandparent
    def Parent(Grandparent):
        pass
File C:
    import __static__
    from B import Parent
    def Child(Parent):
        pass
```

### Why is it bad?

Static Python can't verify that C has overridden things properly from A because we didn't know that A even existed. So we may have static methods which have overrides that we would default to not type checking (because they're static), but those overrides may not be correct.

### Solutions

1. Convert the parent class to running with Static Python
2. Add `@mixin` above the static child class. This is an identity function that makes Static Python treat the class as dynamic (non-static). This will eliminate gains from Static Python asides from typing benefits. Example:
    > ```python
    > File A:
    >     import __static__
    >     def GrandParent():
    >         pass
    > File B:
    >     from A import Grandparent
    >     def Parent(Grandparent):
    >         pass
    > File C:
    >     import __static__
    >     from B import Parent
    >     @__static__.mixin
    >     def Child(Parent):
    >         pass
    > ```


 ## 8. Tests where mock usage conflicts with typing information

Examples:

```python
from unittest import TestCase
class A():
    def __init__(self) -> None:
        pass
    def someFunc(self) -> None:
        pass
class testClass(TestCase):
    def someFunc(self, a: A):
        a.someFunc()
    def test_something(self):
        a_magic_mock = MagicMock()
        self.someFunc(a_magic_mock)
        a_magic_mock.someFunc.assert_called()
```

```python
from unittest import TestCase
from unittest.mock import patch

class testClass(TestCase):
    def someFunc(self) -> int:
        return 5
    def test_something(self):
        with patch(f'{__name__}.testClass.someFunc'):
            self.someFunc()
```

### Why is it bad?

Tests often use `Mock` objects. When these objects are used in places that are typed, Static Python crashes due to a mismatched type. For example, if a function expects an `int` but receives a MagicMock. Another common case is when a function returns a `MagicMock` but it is typed to return a specific type. This is often seen with the `patch` call since it returns `Magicmock` by default.
### Solutions

1. Create a new class that subclasses the mocked object, with attributes set to mocks as needed. Use an instance of this new class as the mock. Example:
    > ```python
    > from unittest import TestCase
    > class A():
    >     def __init__(self) -> None:
    >         pass
    >     def someFunc(self) -> None:
    >         pass
    >
    > class MockA(A):
    >     def __init__(self) -> None:
    >         self.someFunc = MagicMock()
    >
    > class testClass(TestCase):
    >     def someFunc(self, a: A):
    >         a.someFunc()
    >
    >     def some_test(self):
    >         a_magic_mock = MockA()
    >         someFunc(a_magic_mock)
    >         assert a_magic_mock.someFunc.called()
    > ```
2. Create an instance of the mocked object, setting attributes to mocks as needed. Example:
    > ```python
    > from unittest import TestCase
    > class A():
    >     def __init__(self) -> None:
    >         pass
    >     def someFunc(self) -> None:
    >         pass
    >
    > class testClass(TestCase):
    >     def someFunc(self, a: A):
    >         a.someFunc()
    >
    >     def some_test(self):
    >         a_magic_mock = A()
    >         a_magic_mock.someFunc = MagicMock(return_value=None)
    >         someFunc(a_magic_mock)
    >         assert a_magic_mock.someFunc.called()
    > ```
### Note
The `patch` call by default returns a MagicMock. If the function that is being patched has a return type you will need to override the return value given by patch in one of the manners given above.

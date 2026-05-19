class Item:
    def __init__(self, v: int, next: Item | None = None) -> None:
        self.v = v
        self.next = next


class VerySimpleQueue:
    def __init__(self) -> None:
        self.head: Item | None = None
        self.tail: Item | None = None

    def put(self, v: int) -> None:
        item = Item(v)
        if self.tail is None:
            self.head = item
            self.tail = item
            return
        self.tail.next = item
        self.tail = item

    def get(self) -> int:
        item = self.head
        if item is None:
            raise IndexError("get from empty queue")
        self.head = item.next
        if self.head is None:
            self.tail = None
        return item.v

    def empty(self) -> bool:
        return self.head is None

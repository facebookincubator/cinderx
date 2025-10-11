# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict
from typing import Callable


class Opcode:
    CMP_OP = (
        "<",
        "<=",
        "==",
        "!=",
        ">",
        ">=",
        "BAD",
    )
    CONTAINS_OP_ARGS = (
        "in",
        "not in",
    )
    IS_OP_ARGS = (
        "is",
        "is not",
    )

    HAVE_ARGUMENT = 90  # Opcodes from here have an argument:
    EXTENDED_ARG = 144
    EXTENDED_OPCODE = 126
    CODEUNIT_SIZE = 2

    def __init__(self) -> None:
        self.hasconst: set[str] = set()
        self.hasname: set[int] = set()
        self.hasjrel: set[int] = set()
        self.hasjabs: set[int] = set()
        self.haslocal: set[int] = set()
        self.hascompare: set[int] = set()
        self.hasfree: set[int] = set()
        self.shadowop: set[int] = set()

        self.opmap: dict[str, int] = {}
        self.opname: list[str] = ["<{!r}>".format(op) for op in range(256)]
        self.stack_effects: dict[str, object] = {}

        # New for 3.14, we need to know popped vs pushed for the load borrow analysis.
        self.popped: dict[str, int | Callable[[object], int]] = {}
        self.pushed: dict[str, int | Callable[[object], int]] = {}

    def stack_effect(self, opcode: int, oparg, jump: int) -> int:  # pyre-ignore[2]
        oparg_int = 0
        if opcode >= self.HAVE_ARGUMENT:
            if oparg is None:
                raise ValueError(
                    "stack_effect: opcode requires oparg but oparg was not specified"
                )
            oparg_int = int(oparg)
        elif oparg is not None:
            raise ValueError(
                "stack_effect: opcode does not permit oparg but oparg was specified"
            )
        # pyre-fixme[6]: For 1st argument expected `Optional[bool]` but got `int`.
        jump_int = {None: -1, True: 1, False: 0}.get(jump)
        if jump_int is None:
            raise ValueError("stack_effect: jump must be False, True or None")
        opname = self.opname[opcode]
        return self.stack_effect_raw(opname, oparg_int, jump_int)

    def stack_effect_raw(self, opname: str, oparg, jump: int) -> int:  # pyre-ignore[2]
        effect = self.stack_effects.get(opname)
        if effect is None:
            raise ValueError(
                f"Error, opcode {opname} was not found, please update opcode.stack_effects"
            )
        if isinstance(effect, int):
            return effect
        else:
            return effect(oparg, jump)  # pyre-ignore[29]

    def get_num_popped(self, opname: str, oparg: object) -> int:
        popped = self.popped.get(opname)
        if popped is None:
            raise ValueError(
                f"Error, opcode {opname} was not found, please update opcode.stack_effects"
            )
        if isinstance(popped, int):
            return popped
        else:
            return popped(oparg)

    def get_num_pushed(self, opname: str, oparg: object) -> int:
        pushed = self.pushed.get(opname)
        if pushed is None:
            raise ValueError(
                f"Error, opcode {opname} was not found, please update opcode.stack_effects"
            )
        if isinstance(pushed, int):
            return pushed
        else:
            return pushed(oparg)

    def def_op(self, name: str, op: int) -> None:
        # Fill in any missing space in the opname list for
        # opargs which are > 255.
        while op >= len(self.opname):
            self.opname.append("<{!r}>".format(op))
        self.opname[op] = name
        self.opmap[name] = op
        setattr(self, name, op)

    def name_op(self, name: str, op: int) -> None:
        self.def_op(name, op)
        self.hasname.add(op)

    def jrel_op(self, name: str, op: int) -> None:
        self.def_op(name, op)
        self.hasjrel.add(op)

    def jabs_op(self, name: str, op: int) -> None:
        self.def_op(name, op)
        self.hasjabs.add(op)

    def has_jump(self, op: int) -> bool:
        return op in self.hasjrel or op in self.hasjabs

    def shadow_op(self, name: str, op: int) -> None:
        self.def_op(name, op)
        self.shadowop.add(op)

    def remove_op(self, opname: str) -> None:
        op = self.opmap[opname]
        self.hasconst.discard(opname)
        self.hasname.discard(op)
        self.hasjrel.discard(op)
        self.hasjabs.discard(op)
        self.haslocal.discard(op)
        self.hascompare.discard(op)
        self.hasfree.discard(op)
        self.shadowop.discard(op)
        self.opmap.pop(opname)
        self.opname[op] = None  # pyre-ignore[6]
        self.stack_effects.pop(opname)
        delattr(self, opname)

    def copy(self) -> "Opcode":
        result = Opcode()
        result.hasconst = self.hasconst.copy()
        result.hasname = self.hasname.copy()
        result.hasjrel = self.hasjrel.copy()
        result.hasjabs = self.hasjabs.copy()
        result.haslocal = self.haslocal.copy()
        result.hascompare = self.hascompare.copy()
        result.hasfree = self.hasfree.copy()
        result.shadowop = self.shadowop.copy()
        result.opmap = self.opmap.copy()
        result.opname = self.opname.copy()
        result.stack_effects = self.stack_effects.copy()
        for name, op in self.opmap.items():
            setattr(result, name, op)
        return result

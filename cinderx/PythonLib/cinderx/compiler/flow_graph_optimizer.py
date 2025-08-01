# Portions copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

from typing import cast

from .optimizer import safe_lshift, safe_mod, safe_multiply, safe_power

TYPE_CHECKING = False
if TYPE_CHECKING:
    from typing import Callable

    from .pyassem import Block, Instruction, PyFlowGraph

    Handler = Callable[
        [
            "FlowGraphOptimizer",
            int,
            Instruction,
            Instruction | None,
            Instruction | None,
            Block,
        ],
        int | None,
    ]


PyCmp_LT = 0
PyCmp_LE = 1
PyCmp_EQ = 2
PyCmp_NE = 3
PyCmp_GT = 4
PyCmp_GE = 5
PyCmp_IN = 6
PyCmp_NOT_IN = 7
PyCmp_IS = 8
PyCmp_IS_NOT = 9
PyCmp_EXC_MATCH = 10

UNARY_OPS: dict[str, object] = {
    "UNARY_INVERT": lambda v: ~v,
    "UNARY_NEGATIVE": lambda v: -v,
    "UNARY_POSITIVE": lambda v: +v,
}


BINARY_OPS: dict[str, object] = {
    "BINARY_POWER": safe_power,
    "BINARY_MULTIPLY": safe_multiply,
    "BINARY_TRUE_DIVIDE": lambda left, right: left / right,
    "BINARY_FLOOR_DIVIDE": lambda left, right: left // right,
    "BINARY_MODULO": safe_mod,
    "BINARY_ADD": lambda left, right: left + right,
    "BINARY_SUBTRACT": lambda left, right: left - right,
    "BINARY_SUBSCR": lambda left, right: left[right],
    "BINARY_LSHIFT": safe_lshift,
    "BINARY_RSHIFT": lambda left, right: left >> right,
    "BINARY_AND": lambda left, right: left & right,
    "BINARY_XOR": lambda left, right: left ^ right,
    "BINARY_OR": lambda left, right: left | right,
}

SWAPPABLE: set[str] = {"STORE_FAST", "STORE_FAST_MAYBE_NULL", "POP_TOP"}


class FlowGraphOptimizer:
    """Flow graph optimizer."""

    JUMP_ABS: str = "<INVALID JUMP OPCODE>"  # Set to different opcodes in 3.10 and 3.12

    def __init__(self, graph: PyFlowGraph) -> None:
        self.graph = graph

    def optimize_basic_block(self, block: Block) -> None:
        raise NotImplementedError()

    def set_to_nop(self, instr: Instruction) -> None:
        raise NotImplemented()

    def dispatch_instr(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        handler = self.handlers.get(instr.opname)
        if handler is not None:
            return handler(self, instr_index, instr, next_instr, target, block)

    def clean_basic_block(self, block: Block, prev_lineno: int) -> None:
        """Remove all NOPs from a function when legal."""
        new_instrs = []
        num_instrs = len(block.insts)
        for idx in range(num_instrs):
            instr = block.insts[idx]
            if instr.opname == "NOP":
                lineno = instr.lineno
                # Eliminate no-op if it doesn't have a line number
                if lineno < 0:
                    continue
                # or, if the previous instruction had the same line number.
                if prev_lineno == lineno:
                    continue
                # or, if the next instruction has same line number or no line number
                if idx < num_instrs - 1:
                    next_instr = block.insts[idx + 1]
                    next_lineno = next_instr.lineno
                    if next_lineno == lineno:
                        continue
                    elif next_lineno < 0:
                        next_instr.loc = instr.loc
                        continue
                else:
                    next_block = block.next
                    while next_block and len(next_block.insts) == 0:
                        next_block = next_block.next
                    # or if last instruction in BB and next BB has same line number
                    if next_block:
                        if lineno == next_block.insts[0].lineno:
                            continue
            new_instrs.append(instr)
            prev_lineno = instr.lineno
        block.insts = new_instrs

    def jump_thread(self, instr: Instruction, target: Instruction, opname: str) -> int:
        raise NotImplementedError()

    def opt_jump_if_false_or_pop(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        assert target is not None
        if target.opname == "POP_JUMP_IF_FALSE":
            return instr_index + self.jump_thread(instr, target, "POP_JUMP_IF_FALSE")
        elif target.opname in (self.JUMP_ABS, "JUMP_FORWARD", "JUMP_IF_FALSE_OR_POP"):
            return instr_index + self.jump_thread(instr, target, "JUMP_IF_FALSE_OR_POP")
        elif target.opname in ("JUMP_IF_TRUE_OR_POP", "POP_JUMP_IF_TRUE"):
            if instr.lineno == target.lineno:
                target_block = instr.target
                assert target_block and target_block != target_block.next
                instr.opname = "POP_JUMP_IF_FALSE"
                instr.target = target_block.next
                return instr_index
            return instr_index + 1

    def opt_jump_if_true_or_pop(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        assert target is not None
        if target.opname == "POP_JUMP_IF_TRUE":
            return instr_index + self.jump_thread(instr, target, "POP_JUMP_IF_TRUE")
        elif target.opname in (self.JUMP_ABS, "JUMP_FORWARD", "JUMP_IF_TRUE_OR_POP"):
            return instr_index + self.jump_thread(instr, target, "JUMP_IF_TRUE_OR_POP")
        elif target.opname in ("JUMP_IF_FALSE_OR_POP", "POP_JUMP_IF_FALSE"):
            if instr.lineno == target.lineno:
                target_block = instr.target
                assert target_block and target_block != target_block.next
                instr.opname = "POP_JUMP_IF_TRUE"
                instr.target = target_block.next
                return instr_index
            return instr_index + 1

    def opt_pop_jump_if(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        assert target is not None
        if target.opname in (self.JUMP_ABS, "JUMP_FORWARD", "JUMP"):
            return instr_index + self.jump_thread(instr, target, instr.opname)

    def opt_jump(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        assert target is not None
        if target.opname in (self.JUMP_ABS, "JUMP_FORWARD"):
            return instr_index + self.jump_thread(instr, target, self.JUMP_ABS)

    def opt_for_iter(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        assert target is not None
        if target.opname == "JUMP_FORWARD":
            return instr_index + self.jump_thread(instr, target, "FOR_ITER")

    def opt_rot_n(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        if instr.ioparg < 2:
            self.set_to_nop(instr)
            return
        elif instr.ioparg == 2:
            instr.opname = "ROT_TWO"
        elif instr.ioparg == 3:
            instr.opname = "ROT_THREE"
        elif instr.ioparg == 4:
            instr.opname = "ROT_FOUR"
        if instr_index >= instr.ioparg - 1:
            self.fold_rotations(
                block.insts[instr_index - instr.ioparg + 1 : instr_index + 1],
                instr.ioparg,
            )

    def fold_rotations(self, instrs: list[Instruction], n: int) -> None:
        for instr in instrs:
            if instr.opname == "ROT_N":
                rot = instr.ioparg
            elif instr.opname == "ROT_FOUR":
                rot = 4
            elif instr.opname == "ROT_THREE":
                rot = 3
            elif instr.opname == "ROT_TWO":
                rot = 2
            else:
                return
            if rot != n:
                return
        for instr in instrs:
            self.set_to_nop(instr)

    def opt_load_const(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        # Remove LOAD_CONST const; conditional jump
        const = instr.oparg
        if next_instr is None:
            return
        if next_instr.opname in (
            "POP_JUMP_IF_FALSE",
            "POP_JUMP_IF_TRUE",
        ):
            is_true = bool(const)
            self.set_to_nop(block.insts[instr_index])
            jump_if_true = next_instr.opname == "POP_JUMP_IF_TRUE"
            if is_true == jump_if_true:
                next_instr.opname = self.JUMP_ABS
                block.has_fallthrough = False
            else:
                next_instr.target = None
                self.set_to_nop(next_instr)
        elif next_instr.opname in ("JUMP_IF_FALSE_OR_POP", "JUMP_IF_TRUE_OR_POP"):
            is_true = bool(const)
            jump_if_true = next_instr.opname == "JUMP_IF_TRUE_OR_POP"
            if is_true == jump_if_true:
                next_instr.opname = self.JUMP_ABS
                block.has_fallthrough = False
            else:
                self.set_to_nop(block.insts[instr_index])
                self.set_to_nop(next_instr)

    def fold_tuple_on_constants(
        self, instr_index: int, instr: Instruction, block: Block
    ) -> None:
        load_const_instrs = []
        for i in range(instr_index - instr.ioparg, instr_index):
            maybe_load_const = block.insts[i]
            if maybe_load_const.opname != "LOAD_CONST":
                return
            load_const_instrs.append(maybe_load_const)
        newconst = tuple(lc.oparg for lc in load_const_instrs)
        for lc in load_const_instrs:
            self.set_to_nop(lc)
        instr.opname = "LOAD_CONST"
        instr.oparg = newconst
        instr.ioparg = self.graph.convertArg("LOAD_CONST", newconst)

    def opt_return_value(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        block.insts = block.insts[: instr_index + 1]

    handlers: dict[str, Handler] = {
        "JUMP_IF_FALSE_OR_POP": opt_jump_if_false_or_pop,
        "JUMP_IF_TRUE_OR_POP": opt_jump_if_true_or_pop,
        "POP_JUMP_IF_FALSE": opt_pop_jump_if,
        "POP_JUMP_IF_TRUE": opt_pop_jump_if,
        "JUMP_FORWARD": opt_jump,
        "FOR_ITER": opt_for_iter,
        "ROT_N": opt_rot_n,
        "LOAD_CONST": opt_load_const,
        "RETURN_VALUE": opt_return_value,
    }


class FlowGraphOptimizer310(FlowGraphOptimizer):
    """Python 3.10-specifc optimizations."""

    def opt_build_tuple(
        self: FlowGraphOptimizer,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        if (
            next_instr
            and next_instr.opname == "UNPACK_SEQUENCE"
            and instr.ioparg == next_instr.ioparg
        ):
            if instr.ioparg == 1:
                self.set_to_nop(instr)
                self.set_to_nop(next_instr)
            elif instr.ioparg == 2:
                instr.opname = "ROT_TWO"
                self.set_to_nop(next_instr)
            elif instr.ioparg == 3:
                instr.opname = "ROT_THREE"
                next_instr.opname = "ROT_TWO"
            return
        if instr_index >= instr.ioparg:
            self.fold_tuple_on_constants(instr_index, instr, block)

    JUMP_ABS = "JUMP_ABSOLUTE"
    handlers: dict[str, Handler] = {
        **FlowGraphOptimizer.handlers,
        JUMP_ABS: FlowGraphOptimizer.opt_jump,
        "BUILD_TUPLE": opt_build_tuple,
    }

    def set_to_nop(self, instr: Instruction) -> None:
        instr.opname = "NOP"

    def jump_thread(self, instr: Instruction, target: Instruction, opname: str) -> int:
        """Attempt to eliminate jumps to jumps by updating inst to jump to
        target->i_target using the provided opcode. Return 0 if successful, 1 if
        not; this makes it easier for our callers to revisit the same
        instruction again only if we changed it."""
        assert instr.is_jump(self.graph.opcode)
        assert target.is_jump(self.graph.opcode)
        if instr.lineno == target.lineno and instr.target != target.target:
            instr.target = target.target
            instr.opname = opname
            return 0
        return 1

    def optimize_basic_block(self, block: Block) -> None:
        instr_index = 0

        while instr_index < len(block.insts):
            instr = block.insts[instr_index]

            target_instr: Instruction | None = None
            if instr.is_jump(self.graph.opcode):
                target = instr.target
                assert target is not None
                # Skip over empty basic blocks.
                while len(target.insts) == 0:
                    instr.target = target.next
                    target = instr.target
                    assert target is not None
                target_instr = target.insts[0]

            next_instr = (
                block.insts[instr_index + 1]
                if instr_index + 1 < len(block.insts)
                else None
            )

            new_index = self.dispatch_instr(
                instr_index, instr, next_instr, target_instr, block
            )
            instr_index = instr_index + 1 if new_index is None else new_index


class FlowGraphOptimizer312(FlowGraphOptimizer):
    """Python 3.12-specifc optimizations."""

    JUMP_ABS = "JUMP"

    def set_to_nop(self, instr: Instruction) -> None:
        instr.set_to_nop()

    def optimize_basic_block(self, block: Block) -> None:
        instr_index = 0
        opt_instr: Instruction | None = None
        # The handler needs to be called with the original instr and index
        # because the instr_index may be updated after the first call to
        # dispatch_instr.
        while instr_index < len(block.insts):
            instr = block.insts[instr_index]
            target_instr: Instruction | None = None

            is_copy_of_load_const = (
                opt_instr is not None
                and opt_instr.opname == "LOAD_CONST"
                and instr.opname == "COPY"
                and instr.ioparg == 1
            )

            if not is_copy_of_load_const:
                opt_instr = instr
                if instr.is_jump(self.graph.opcode):
                    target = instr.target
                    assert target is not None
                    assert target.insts, f"{instr} {target.label} {target.bid}"
                    # Skip over empty basic blocks.
                    while len(target.insts) == 0:
                        instr.target = target.next
                        target = instr.target
                        assert target is not None
                    target_instr = target.insts[0]

            next_instr = (
                block.insts[instr_index + 1]
                if instr_index + 1 < len(block.insts)
                else None
            )

            assert opt_instr is not None
            new_index = self.dispatch_instr(
                instr_index, opt_instr, next_instr, target_instr, block
            )

            instr_index = instr_index + 1 if new_index is None else new_index

    def opt_load_const(
        self: FlowGraphOptimizer,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        if next_instr and next_instr.opname == "RETURN_VALUE":
            next_instr.opname = "RETURN_CONST"
            next_instr.oparg = instr.oparg
            next_instr.ioparg = instr.ioparg
            block.insts[instr_index].set_to_nop()
        elif next_instr is not None and next_instr.opname == "IS_OP":
            jmp_op = (
                block.insts[instr_index + 2]
                if instr_index + 2 < len(block.insts)
                else None
            )
            if (
                jmp_op is not None
                and jmp_op.opname in ("POP_JUMP_IF_FALSE", "POP_JUMP_IF_TRUE")
                and instr.oparg is None
            ):
                nextarg = next_instr.oparg == 1
                instr.set_to_nop()
                next_instr.set_to_nop()
                jmp_op.opname = (
                    "POP_JUMP_IF_NOT_NONE"
                    if nextarg ^ (jmp_op.opname == "POP_JUMP_IF_FALSE")
                    else "POP_JUMP_IF_NONE"
                )
                return instr_index + 2
        else:
            # The rest of the optimizations are common to 3.10 and 3.12
            return super().opt_load_const(instr_index, instr, next_instr, target, block)

    def opt_push_null(
        self: FlowGraphOptimizer,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        if next_instr is None:
            return

        if next_instr.opname == "LOAD_GLOBAL" and (next_instr.ioparg & 1) == 0:
            instr.set_to_nop()
            next_instr.oparg = (next_instr.oparg, 1)
            next_instr.ioparg |= 1

    def opt_build_tuple(
        self: FlowGraphOptimizer,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        if (
            next_instr
            and next_instr.opname == "UNPACK_SEQUENCE"
            and instr.ioparg == next_instr.ioparg
        ):
            if instr.ioparg == 1:
                instr.set_to_nop()
                next_instr.set_to_nop()
            elif instr.ioparg == 2 or instr.ioparg == 3:
                instr.set_to_nop()
                next_instr.opname = "SWAP"
            return
        if instr_index >= instr.ioparg:
            self.fold_tuple_on_constants(instr_index, instr, block)

    def opt_swap(
        self,
        instr_index: int,
        instr: Instruction,
        next_instr: Instruction | None,
        target: Instruction | None,
        block: Block,
    ) -> int | None:
        if instr.oparg == 1:
            instr.set_to_nop()
            return

        new_index = self.swaptimize(instr_index, block)
        self.apply_static_swaps(new_index or instr_index, block)
        return new_index

    def next_swappable_instruction(self, i: int, block: Block, lineno: int) -> int:
        while i + 1 < len(block.insts):
            i += 1
            inst = block.insts[i]
            if lineno >= 0 and inst.lineno != lineno:
                # Optimizing across this instruction could cause user-visible
                # changes in the names bound between line tracing events!
                return -1
            elif inst.opname == "NOP":
                continue
            elif inst.opname in SWAPPABLE:
                return i

            return -1

        return -1

    # Attempt to apply SWAPs statically by swapping *instructions* rather than
    # stack items. For example, we can replace SWAP(2), POP_TOP, STORE_FAST(42)
    # with the more efficient NOP, STORE_FAST(42), POP_TOP.
    def apply_static_swaps(self, instr_index: int, block: Block) -> None:
        # SWAPs are to our left, and potential swaperands are to our right:
        for i in range(instr_index, -1, -1):
            swap = block.insts[i]
            if swap.opname != "SWAP":
                if swap.opname == "NOP" or swap.opname in SWAPPABLE:
                    # Nope, but we know how to handle these. Keep looking:
                    continue
                # We can't reason about what this instruction does. Bail:
                return

            j = self.next_swappable_instruction(i, block, -1)
            if j < 0:
                return

            k = j
            lineno = block.insts[j].lineno
            for i in range(swap.ioparg - 1, 0, -1):
                k = self.next_swappable_instruction(k, block, lineno)
                if k < 0:
                    return

            # The reordering is not safe if the two instructions to be swapped
            # store to the same location, or if any intervening instruction stores
            # to the same location as either of them.
            store_j = block.insts[j].stores_to
            store_k = block.insts[k].stores_to
            if store_j is not None or store_k is not None:
                if store_j == store_k:
                    return
                for idx in range(j + 1, k):
                    store_idx = block.insts[idx].stores_to
                    if store_idx is not None and (
                        store_idx == store_j or store_idx == store_k
                    ):
                        return

            swap.set_to_nop()
            temp = block.insts[j]
            block.insts[j] = block.insts[k]
            block.insts[k] = temp

    def swaptimize(self, instr_index: int, block: Block) -> int | None:
        """Replace an arbitrary run of SWAPs and NOPs with an optimal one that has the
        same effect."""

        # Find the length of the current sequence of SWAPs and NOPs, and record the
        # maximum depth of the stack manipulations:
        instructions = block.insts
        depth = instructions[instr_index].ioparg
        more = False
        cnt = 0
        for cnt in range(instr_index + 1, len(instructions)):
            opname = instructions[cnt].opname
            if opname == "SWAP":
                depth = max(depth, instructions[cnt].ioparg)
                more = True
            elif opname != "NOP":
                break

        if not more:
            return

        # Create an array with elements {0, 1, 2, ..., depth - 1}:
        stack = [i for i in range(depth)]
        # Simulate the combined effect of these instructions by "running" them on
        # our "stack":
        for i in range(instr_index, cnt):
            if block.insts[i].opname == "SWAP":
                oparg = instructions[i].ioparg
                top = stack[0]
                #  SWAPs are 1-indexed:
                stack[0] = stack[oparg - 1]
                stack[oparg - 1] = top

        ## Now we can begin! Our approach here is based on a solution to a closely
        ## related problem (https://cs.stackexchange.com/a/13938). It's easiest to
        ## think of this algorithm as determining the steps needed to efficiently
        ## "un-shuffle" our stack. By performing the moves in *reverse* order,
        ## though, we can efficiently *shuffle* it! For this reason, we will be
        ## replacing instructions starting from the *end* of the run. Since the
        ## solution is optimal, we don't need to worry about running out of space:
        current = cnt - 1
        VISITED = -1
        for i in range(depth):
            if stack[i] == VISITED or stack[i] == i:
                continue

            # Okay, we've found an item that hasn't been visited. It forms a cycle
            # with other items; traversing the cycle and swapping each item with
            # the next will put them all in the correct place. The weird
            # loop-and-a-half is necessary to insert 0 into every cycle, since we
            # can only swap from that position:
            j = i
            while True:
                # Skip the actual swap if our item is zero, since swapping the top
                # item with itself is pointless:
                if j:
                    # SWAPs are 1-indexed:
                    instructions[current].opname = "SWAP"
                    instructions[current].ioparg = j + 1
                    current -= 1

                if stack[j] == VISITED:
                    # Completed the cycle:
                    assert j == i
                    break

                next_j = stack[j]
                stack[j] = VISITED
                j = next_j

        while instr_index <= current:
            instructions[current].set_to_nop()
            current -= 1

        return cnt - 1

    def jump_thread(self, instr: Instruction, target: Instruction, opname: str) -> int:
        """Attempt to eliminate jumps to jumps by updating inst to jump to
        target->i_target using the provided opcode. Return 0 if successful, 1 if
        not; this makes it easier for our callers to revisit the same
        instruction again only if we changed it."""
        assert instr.is_jump(self.graph.opcode)
        assert target.is_jump(self.graph.opcode)
        if (
            instr.lineno == target.lineno or target.lineno == -1
        ) and instr.target != target.target:
            instr.target = target.target
            instr.opname = opname
            return 0
        return 1

    handlers: dict[str, Handler] = {
        **FlowGraphOptimizer.handlers,
        JUMP_ABS: FlowGraphOptimizer.opt_jump,
        "LOAD_CONST": opt_load_const,
        "PUSH_NULL": opt_push_null,
        "BUILD_TUPLE": opt_build_tuple,
        "SWAP": cast("Handler", opt_swap),
        "POP_JUMP_IF_NONE": FlowGraphOptimizer.opt_pop_jump_if,
        "POP_JUMP_IF_NOT_NONE": FlowGraphOptimizer.opt_pop_jump_if,
    }

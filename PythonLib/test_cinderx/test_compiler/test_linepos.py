# Copyright (c) Meta Platforms, Inc. and affiliates.

from dis import get_instructions
from textwrap import dedent
from types import CodeType
from unittest import skipIf, TestCase

from cinderx.compiler.pyassem import LinePositionTable, SrcLocation
from cinderx.compiler.pycodegen import CodeGenerator312

from .common import CompilerTest


class LinePositionTests(CompilerTest):
    test_cases = [
        (
            """
                def f():
                    return a + b
            """,
            [
                (SrcLocation(1, 1, 0, 0), 1),
                (SrcLocation(2, 2, 11, 12), 5),
                (SrcLocation(2, 2, 15, 16), 5),
                (SrcLocation(2, 2, 11, 16), 2),
                (SrcLocation(2, 2, 4, 16), 1),
            ],
            b"\x80\x00\xdc\x0b\x0c\x8cq\x895\x80L",
        ),
        (
            # Identifier longer than 16 chars
            """
                def f():
                    return abcdefghijklmnopqrstuvwxyz
            """,
            [
                (SrcLocation(1, 1, 0, 0), 1),
                (SrcLocation(2, 2, 11, 37), 5),
                (SrcLocation(2, 2, 4, 37), 1),
            ],
            b"\x80\x00\xdc\x0b%\xd0\x04%",
        ),
        (
            # Identifier longer than 128 chars
            """
                def f():
                    return abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz
            """,
            [
                (SrcLocation(1, 1, 0, 0), 1),
                (SrcLocation(2, 2, 11, 141), 5),
                (SrcLocation(2, 2, 4, 141), 1),
            ],
            b"\x80\x00\xf4\x02\x00\x0cN\x02\xf0\x00\x00\x05N\x02",
        ),
        (
            # Jump 2 lines
            """
                def f():
                    a = 42


                    b = 100
            """,
            [
                (SrcLocation(1, 1, 0, 0), 1),
                (SrcLocation(2, 2, 8, 10), 1),
                (SrcLocation(2, 2, 4, 5), 1),
                (SrcLocation(4, 4, 8, 11), 1),
                (SrcLocation(4, 4, 4, 5), 2),
            ],
            b"\x80\x00\xd8\x08\n\x80A\xe0\x08\x0b\x81A",
        ),
        (
            # Jump 3 lines
            """
                def f():
                    a = 42



                    b = 100
            """,
            [
                (SrcLocation(1, 1, 0, 0), 1),
                (SrcLocation(2, 2, 8, 10), 1),
                (SrcLocation(2, 2, 4, 5), 1),
                (SrcLocation(5, 5, 8, 11), 1),
                (SrcLocation(5, 5, 4, 5), 2),
            ],
            b"\x80\x00\xd8\x08\n\x80A\xf0\x06\x00\t\x0c\x81A",
        ),
        (
            """
                def f():
                    return a + b + c
            """,
            [
                (SrcLocation(1, 1, 0, 0), 1),
                (SrcLocation(2, 2, 11, 12), 5),
                (SrcLocation(2, 2, 15, 16), 5),
                (SrcLocation(2, 2, 11, 16), 2),
                (SrcLocation(2, 2, 19, 20), 5),
                (SrcLocation(2, 2, 11, 20), 2),
                (SrcLocation(2, 2, 4, 20), 1),
            ],
            b"\x80\x00\xdc\x0b\x0c\x8cq\x895\x941\x899\xd0\x04\x14",
        ),
        # long-form
        (
            """
                def f():
                    return (a +
                            b)
            """,
            [
                (SrcLocation(1, 1, 0, 0), 1),
                (SrcLocation(2, 2, 12, 13), 5),
                (SrcLocation(3, 3, 12, 13), 5),
                (SrcLocation(2, 3, 12, 13), 2),
                (SrcLocation(2, 3, 4, 14), 1),
            ],
            b"\x80\x00\xdc\x0c\r\xdc\x0c\r\xf1\x03\x01\r\x0e\xf0\x00\x01\x05\x0f",
        ),
        # no column
        (
            """
                def f():
                    yield 42
            """,
            [
                (SrcLocation(1, 1, -1, -1), 1),
                (SrcLocation(-1, -1, -1, -1), 1),
                (SrcLocation(1, 1, 0, 0), 1),
                (SrcLocation(2, 2, 10, 12), 1),
                (SrcLocation(2, 2, 4, 12), 4),
                (SrcLocation(-1, -1, -1, -1), 2),
            ],
            b"\xe8\x00\xf8\x80\x00\xd8\n\x0c\x83H\xf9",
        ),
    ]

    def test_scenarios(self):
        for code, positions, expected in self.test_cases:
            with self.subTest(code=code):
                postab = LinePositionTable(1)
                for pos, length in positions:
                    postab.emit_location(pos, length)

                table = bytes(postab.getTable())
                self.assertEqual(table, expected)

    @skipIf(
        not hasattr(test_scenarios.__code__, "co_positions"), "requires 312 or later"
    )
    def test_scenarios_312(self):
        for code, positions, expected in self.test_cases:
            with self.subTest(code=code):
                d = {}
                exec(dedent(code).strip() + "\n", d, d)
                f = d["f"]

                # Transform the actual positions from the C compiler back into the location
                # data that we would feed into the position calculator.
                lastpos = None
                length = 1
                positions = []
                for position in f.__code__.co_positions():
                    if lastpos is None:
                        # First time through, just set the last position.
                        lastpos = position
                        continue
                    if lastpos != position:
                        # The position changed, so append the previous run
                        positions.append(
                            (
                                SrcLocation(
                                    *(val if val is not None else -1 for val in lastpos)
                                ),
                                length,
                            )
                        )
                        lastpos = position
                        length = 1
                    else:
                        # The position didn't change, so increment the length
                        length += 1

                # Add the last run of positions
                positions.append(
                    (
                        SrcLocation(
                            *(val if val is not None else -1 for val in lastpos)
                        ),
                        length,
                    )
                )

                # Send the data into the table
                postab = LinePositionTable(1)
                for position in positions:
                    postab.emit_location(*position)

                # And verify we get the same thing
                table = bytes(postab.getTable())
                self.assertEqual(table, f.__code__.co_linetable)

    def get_position(self, compiled: CodeType, opcode: str) -> tuple[...] | None: 
        position: tuple[...] | None = None
        for instr in get_instructions(compiled):
            if instr.opname == opcode:
                # pyre-ignore[16]: No co_positions
                position = list(compiled.co_positions())[instr.offset // 2]
                break
        else:
            self.fail("Could not find opcode in bytecode")

        return position

    @skipIf(
        not hasattr(test_scenarios.__code__, "co_positions"), "requires 312 or later"
    )
    def test_actual_positions(self):
        test_cases = [
            (
                """
                    def f():
                        return 42
                """,
                "RETURN_CONST",
            ),
            (
                """
                    def f():
                        x += 42
                """,
                "BINARY_OP",
            )

        ]
        for code, opcode in test_cases:
            with self.subTest(code=code):
                test_code = dedent(code).strip() + "\n"
                d = {}
                exec(test_code, d, d)
                
                c_compiled = d["f"]
                py_compiled = self.run_code(test_code, generator=CodeGenerator312)["f"]

                c_position = self.get_position(c_compiled.__code__, opcode)
                py_position = self.get_position(py_compiled.__code__, opcode)

                self.assertEqual(c_position, py_position)

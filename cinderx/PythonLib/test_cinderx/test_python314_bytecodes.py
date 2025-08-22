# Copyright (c) Meta Platforms, Inc. and affiliates.

import dis
import sys
import unittest

import cinderx.test_support as cinder_support


@unittest.skipUnless(sys.version_info >= (3, 14), "Python 3.14+ only")
class Python314Bytecodes(unittest.TestCase):
    def _assertBytecodeContains(self, func, expected_opcode):
        try:
            inner_function = func.inner_function
        except AttributeError:
            pass
        else:
            func = inner_function
        bytecode_instructions = list(dis.get_instructions(func))
        opcodes = [instr.opname for instr in bytecode_instructions]

        self.assertIn(
            expected_opcode,
            opcodes,
            f"{expected_opcode} opcode should be present in {func.__name__} bytecode",
        )

    def test_LOAD_SMALL_INT(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            return 1

        self.assertEqual(x(), 1)
        self._assertBytecodeContains(x, "LOAD_SMALL_INT")

    def test_BINARY_OP_oparg_SUBSCR(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            return [1, 2][0]

        self.assertEqual(x(), 1)
        self._assertBytecodeContains(x, "BINARY_OP")


if __name__ == "__main__":
    unittest.main()
